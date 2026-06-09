// SPDX-License-Identifier: Apache-2.0
// In-guest data-plane test: a real RDMA WRITE that MOVES DATA end-to-end through
// the official kernel stack + the gem5 NICTopologySC. Two MR segments are
// registered (the provider backs them with NIC aperture memory, ldst_mem_); the
// client writes a pattern into src, posts a WRITE src->dst over RC, polls the
// completion, and verifies dst now holds the pattern — i.e. the NIC actually
// moved the bytes and produced a CQE.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "urma_api.h"
#include "urma_types.h"
#include <fcntl.h>
#include <unistd.h>
static void kmsg(const char*m){int fd=open("/dev/console",O_WRONLY);if(fd>=0){char b[256];int n=snprintf(b,sizeof b,"openurma-kdp: %s\n",m);(void)!write(fd,b,n);close(fd);}}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    urma_init_attr_t ia = {0};
    if (urma_init(&ia) != URMA_SUCCESS) { printf("[kdp] urma_init FAIL\n"); return 1; }
    urma_device_t* dev = urma_get_device_by_name("openurma0");
    if (!dev) { printf("[kdp] no device\n"); return 1; }
    urma_context_t* ctx = urma_create_context(dev, 0);
    if (!ctx) { printf("[kdp] create_context FAIL\n"); return 1; }
    urma_jfc_cfg_t fc = { .depth = 64 };
    urma_jfc_t* jfc = urma_create_jfc(ctx, &fc);
    urma_jfr_cfg_t rc; memset(&rc, 0, sizeof rc);
    rc.depth = 64; rc.trans_mode = URMA_TM_RC; rc.jfc = jfc; rc.token_value.token = 0xDEADBEEF;
    urma_jfr_t* jfr = urma_create_jfr(ctx, &rc);
    urma_jetty_cfg_t jc; memset(&jc, 0, sizeof jc);
    jc.jfs_cfg.depth = 64; jc.jfs_cfg.trans_mode = URMA_TM_RC; jc.jfs_cfg.jfc = jfc;
    jc.flag.bs.share_jfr = 1; jc.shared.jfr = jfr; jc.shared.jfc = jfc;
    urma_jetty_t* jetty = urma_create_jetty(ctx, &jc);
    if (!jetty) { printf("[kdp] create_jetty FAIL\n"); return 1; }

    // two MR segments (provider backs them with NIC-aperture memory)
    char dummy_s[256], dummy_d[256];
    urma_seg_cfg_t sc; memset(&sc, 0, sizeof sc); sc.va = (uint64_t)(uintptr_t)dummy_s; sc.len = 256; sc.token_value.token = 0xDEADBEEF;
    urma_target_seg_t* sseg = urma_register_seg(ctx, &sc);
    urma_seg_cfg_t dc; memset(&dc, 0, sizeof dc); dc.va = (uint64_t)(uintptr_t)dummy_d; dc.len = 256; dc.token_value.token = 0xDEADBEEF;
    urma_target_seg_t* dseg = urma_register_seg(ctx, &dc);
    if (!sseg || !dseg) { printf("[kdp] register_seg FAIL\n"); return 1; }
    volatile char* src = (volatile char*)(uintptr_t)sseg->seg.ubva.va;
    volatile char* dst = (volatile char*)(uintptr_t)dseg->seg.ubva.va;

    const char* MSG = "OpenURMA-gem5-data-plane-WRITE-roundtrip";
    int n = (int)strlen(MSG) + 1;
    int n8 = (n + 7) & ~7;
    // The MR aperture is mapped uncached/Device on arm64; only ALIGNED accesses
    // are allowed (memcpy/memset/memcmp would SIGBUS). Use 8-byte volatile ops.
    {
        uint8_t tmp[256] = {0}; memcpy(tmp, MSG, n);
        volatile uint64_t* sp = (volatile uint64_t*)src;
        volatile uint64_t* dp = (volatile uint64_t*)dst;
        for (int i = 0; i < n8/8; i++) { uint64_t v; memcpy(&v, tmp + i*8, 8); sp[i] = v; dp[i] = 0; }
    }
    printf("[kdp] src=0x%lx dst=0x%lx; wrote '%s' to src, dst cleared\n",
           (unsigned long)sseg->seg.ubva.va, (unsigned long)dseg->seg.ubva.va, MSG);

    // self-loop: import own jetty + dst seg, bind
    urma_rjetty_t rj; memset(&rj, 0, sizeof rj); rj.jetty_id = jetty->jetty_id; rj.trans_mode = URMA_TM_RC;
    urma_token_t tok = { .token = 0xDEADBEEF };
    urma_target_jetty_t* tj = urma_import_jetty(ctx, &rj, &tok);
    urma_bind_jetty(jetty, tj);
    urma_seg_t rseg; memset(&rseg, 0, sizeof rseg); rseg.ubva = dseg->seg.ubva; rseg.len = 256; rseg.token_id = dseg->seg.token_id;
    urma_target_seg_t* rt = urma_import_seg(ctx, &rseg, &tok, dseg->seg.ubva.va, (urma_import_seg_flag_t){0});

    // RDMA WRITE: src -> remote dst
    urma_sge_t ssge = { .addr = (uint64_t)(uintptr_t)src, .len = n, .tseg = sseg };
    urma_sge_t dsge = { .addr = dseg->seg.ubva.va, .len = n, .tseg = rt ? rt : dseg };
    urma_sg_t s_sg = { .sge = &ssge, .num_sge = 1 }, d_sg = { .sge = &dsge, .num_sge = 1 };
    urma_jfs_wr_t w; memset(&w, 0, sizeof w);
    w.opcode = URMA_OPC_WRITE; w.tjetty = tj; w.user_ctx = 0x42; w.rw.src = s_sg; w.rw.dst = d_sg;
    urma_jfs_wr_t* bad = 0;
    int pr = urma_post_jetty_send_wr(jetty, &w, &bad);
    printf("[kdp] post WRITE -> %d\n", pr);
    urma_cr_t cr; int got = 0;
    for (int t = 0; t < 200000 && !got; t++) got = urma_poll_jfc(jfc, 1, &cr);
    printf("[kdp] poll_jfc -> %d completion(s)%s\n", got,
           got > 0 ? "" : " (timeout)");

    // read dst back via aligned 8-byte loads, then compare
    uint8_t got_dst[256] = {0};
    { volatile uint64_t* dp = (volatile uint64_t*)dst;
      for (int i = 0; i < n8/8; i++) { uint64_t v = dp[i]; memcpy(got_dst + i*8, &v, 8); } }
    int match = (memcmp(got_dst, MSG, n) == 0);
    { char b[256]; snprintf(b,sizeof b,"completion=%d data_moved=%d dst='%s'",got>0,match,(char*)got_dst); kmsg(b); }
    printf("[kdp] dst after WRITE = '%s'\n", (char*)got_dst);
    printf("[kdp] RESULT: %s (completion=%d data_moved=%d)\n",
           (got > 0 && match) ? "PASS" : "FAIL", got > 0, match);

    urma_delete_context(ctx); urma_uninit();
    return ((got > 0) ? 2 : 0) | (match ? 1 : 0);
}
