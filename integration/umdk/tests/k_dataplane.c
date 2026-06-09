// SPDX-License-Identifier: Apache-2.0
// In-guest data-plane test: real RDMA WRITE + READ that MOVE DATA end-to-end
// through the official kernel stack + the gem5 NICTopologySC. MR segments are
// registered (provider backs them with NIC aperture memory, ldst_mem_); the
// client moves data over RC, polls completions, and verifies the bytes moved.
// Exercises WRITE at several sizes and READ. Exit code = number of FAILED checks
// (0 = all pass); the kmsg() line carries the per-check summary.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "urma_api.h"
#include "urma_types.h"

static void say(const char* m) { int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) { char b[256]; int n = snprintf(b, sizeof b, "openurma-kdp: %s\n", m); (void)!write(fd, b, n); close(fd); } }

// aligned (uncached Device memory) helpers
static void ap_write(volatile char* p, const void* data, int len) {
    uint8_t tmp[512] = {0}; memcpy(tmp, data, len); int n8 = (len + 7) & ~7;
    volatile uint64_t* q = (volatile uint64_t*)p;
    for (int i = 0; i < n8/8; i++) { uint64_t v; memcpy(&v, tmp + i*8, 8); q[i] = v; } }
static void ap_read(volatile char* p, void* out, int len) {
    uint8_t tmp[512] = {0}; int n8 = (len + 7) & ~7;
    volatile uint64_t* q = (volatile uint64_t*)p;
    for (int i = 0; i < n8/8; i++) { uint64_t v = q[i]; memcpy(tmp + i*8, &v, 8); }
    memcpy(out, tmp, len); }
static void ap_clear(volatile char* p, int len) {
    int n8 = (len + 7) & ~7; volatile uint64_t* q = (volatile uint64_t*)p;
    for (int i = 0; i < n8/8; i++) q[i] = 0; }

static urma_context_t* ctx; static urma_jfc_t* jfc; static urma_jetty_t* jetty;
static urma_target_jetty_t* tj;

static int wait_cqe(void) { urma_cr_t cr;
    for (int t = 0; t < 400000; t++) if (urma_poll_jfc(jfc, 1, &cr) > 0) return 1;
    return 0; }

int main(void)
{
    urma_init_attr_t ia = {0};
    if (urma_init(&ia) != URMA_SUCCESS) { say("urma_init FAIL"); return 99; }
    urma_device_t* dev = urma_get_device_by_name("openurma0");
    if (!dev) { say("no device"); return 99; }
    ctx = urma_create_context(dev, 0);
    if (!ctx) { say("create_context FAIL"); return 99; }
    urma_jfc_cfg_t fc = { .depth = 64 }; jfc = urma_create_jfc(ctx, &fc);
    urma_jfr_cfg_t rc; memset(&rc, 0, sizeof rc);
    rc.depth = 64; rc.trans_mode = URMA_TM_RC; rc.jfc = jfc; rc.token_value.token = 0xDEADBEEF;
    urma_jfr_t* jfr = urma_create_jfr(ctx, &rc);
    urma_jetty_cfg_t jc; memset(&jc, 0, sizeof jc);
    jc.jfs_cfg.depth = 64; jc.jfs_cfg.trans_mode = URMA_TM_RC; jc.jfs_cfg.jfc = jfc;
    jc.flag.bs.share_jfr = 1; jc.shared.jfr = jfr; jc.shared.jfc = jfc;
    jetty = urma_create_jetty(ctx, &jc);
    if (!jetty) { say("create_jetty FAIL"); return 99; }

    // two MR segments (provider backs them with NIC aperture memory)
    char d0[256], d1[256];
    urma_seg_cfg_t sc; memset(&sc, 0, sizeof sc); sc.va = (uint64_t)(uintptr_t)d0; sc.len = 256; sc.token_value.token = 0xDEADBEEF;
    urma_target_seg_t* sseg = urma_register_seg(ctx, &sc);
    urma_seg_cfg_t dc; memset(&dc, 0, sizeof dc); dc.va = (uint64_t)(uintptr_t)d1; dc.len = 256; dc.token_value.token = 0xDEADBEEF;
    urma_target_seg_t* dseg = urma_register_seg(ctx, &dc);
    if (!sseg || !dseg) { say("register_seg FAIL"); return 99; }
    volatile char* A = (volatile char*)(uintptr_t)sseg->seg.ubva.va;
    volatile char* B = (volatile char*)(uintptr_t)dseg->seg.ubva.va;

    // self-loop: import own jetty + both segs, bind
    urma_rjetty_t rj; memset(&rj, 0, sizeof rj); rj.jetty_id = jetty->jetty_id; rj.trans_mode = URMA_TM_RC;
    urma_token_t tok = { .token = 0xDEADBEEF };
    tj = urma_import_jetty(ctx, &rj, &tok); urma_bind_jetty(jetty, tj);
    urma_seg_t ra; memset(&ra, 0, sizeof ra); ra.ubva = sseg->seg.ubva; ra.len = 256; ra.token_id = sseg->seg.token_id;
    urma_target_seg_t* rtA = urma_import_seg(ctx, &ra, &tok, sseg->seg.ubva.va, (urma_import_seg_flag_t){0});
    urma_seg_t rb; memset(&rb, 0, sizeof rb); rb.ubva = dseg->seg.ubva; rb.len = 256; rb.token_id = dseg->seg.token_id;
    urma_target_seg_t* rtB = urma_import_seg(ctx, &rb, &tok, dseg->seg.ubva.va, (urma_import_seg_flag_t){0});

    int pass = 0, total = 0;

    // ---- WRITE at several sizes: A -> B, verify B ----
    int sizes[] = { 8, 64, 200 };
    for (int si = 0; si < 3; si++) {
        int len = sizes[si];
        uint8_t pat[256]; for (int i = 0; i < len; i++) pat[i] = (uint8_t)(0x40 + si*16 + (i & 0x0f));
        ap_write(A, pat, len); ap_clear(B, len);
        urma_sge_t s = { .addr = sseg->seg.ubva.va, .len = len, .tseg = sseg };
        urma_sge_t d = { .addr = dseg->seg.ubva.va, .len = len, .tseg = rtB };
        urma_sg_t ssg = { .sge = &s, .num_sge = 1 }, dsg = { .sge = &d, .num_sge = 1 };
        urma_jfs_wr_t w; memset(&w, 0, sizeof w);
        w.opcode = URMA_OPC_WRITE; w.tjetty = tj; w.user_ctx = 0x100 + si; w.rw.src = ssg; w.rw.dst = dsg;
        urma_jfs_wr_t* bad = 0;
        urma_post_jetty_send_wr(jetty, &w, &bad);
        int cqe = wait_cqe();
        uint8_t out[256]; ap_read(B, out, len);
        int ok = cqe && (memcmp(out, pat, len) == 0);
        total++; pass += ok;
        char b[96]; snprintf(b, sizeof b, "WRITE len=%d cqe=%d data=%s", len, cqe, ok ? "OK" : "BAD"); say(b);
    }

    // ---- READ: B (remote) -> A (local), verify A ----
    {
        int len = 64;
        uint8_t pat[256]; for (int i = 0; i < len; i++) pat[i] = (uint8_t)(0xA0 + (i & 0x0f));
        ap_write(B, pat, len); ap_clear(A, len);
        urma_sge_t s = { .addr = dseg->seg.ubva.va, .len = len, .tseg = rtB };   // remote source = B
        urma_sge_t d = { .addr = sseg->seg.ubva.va, .len = len, .tseg = sseg };  // local dest = A
        urma_sg_t ssg = { .sge = &s, .num_sge = 1 }, dsg = { .sge = &d, .num_sge = 1 };
        urma_jfs_wr_t w; memset(&w, 0, sizeof w);
        w.opcode = URMA_OPC_READ; w.tjetty = tj; w.user_ctx = 0x200; w.rw.src = ssg; w.rw.dst = dsg;
        urma_jfs_wr_t* bad = 0;
        urma_post_jetty_send_wr(jetty, &w, &bad);
        int cqe = wait_cqe();
        uint8_t out[256]; ap_read(A, out, len);
        int ok = cqe && (memcmp(out, pat, len) == 0);
        total++; pass += ok;
        char b[96]; snprintf(b, sizeof b, "READ  len=%d cqe=%d data=%s", len, cqe, ok ? "OK" : "BAD"); say(b);
        (void)rtA;
    }

    char b[96]; snprintf(b, sizeof b, "RESULT %d/%d data-plane checks passed", pass, total); say(b);
    urma_delete_context(ctx); urma_uninit();
    return total - pass;   // 0 = all pass
}
