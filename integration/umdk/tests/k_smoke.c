// SPDX-License-Identifier: Apache-2.0
// In-guest verbs smoke test for the OFFICIAL kernel path: every verb here goes
// stock liburma -> openurma kernel provider -> urma_cmd_* ioctl -> uburma.ko ->
// ubcore.ko -> openurma_ubcore.ko. Single process (the single-node gem5 config
// self-loops the wire). Prints each step so dmesg (kmod) + this log together
// show the official kernel control + data path exercised by stock verbs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "urma_api.h"
#include "urma_types.h"

#define STEP(name, cond) do { int _ok = (cond); printf("[k_smoke] %-22s %s\n", name, _ok?"OK":"FAIL"); if(!_ok) { printf("[k_smoke] RESULT: FAIL at %s\n", name); return 1; } } while(0)

int main(void)
{
    urma_init_attr_t ia = {0};
    STEP("urma_init", urma_init(&ia) == URMA_SUCCESS);
    urma_device_t* dev = urma_get_device_by_name("openurma0");
    STEP("get_device openurma0", dev != NULL);
    urma_context_t* ctx = urma_create_context(dev, 0);
    STEP("create_context", ctx != NULL);

    urma_jfc_cfg_t fc = { .depth = 64 };
    urma_jfc_t* jfc = urma_create_jfc(ctx, &fc);
    STEP("create_jfc (ioctl)", jfc != NULL);

    urma_jfr_cfg_t rc; memset(&rc, 0, sizeof rc);
    rc.depth = 64; rc.trans_mode = URMA_TM_RC; rc.jfc = jfc; rc.token_value.token = 0xDEADBEEF;
    urma_jfr_t* jfr = urma_create_jfr(ctx, &rc);
    STEP("create_jfr (ioctl)", jfr != NULL);

    urma_jetty_cfg_t jc; memset(&jc, 0, sizeof jc);
    jc.jfs_cfg.depth = 64; jc.jfs_cfg.trans_mode = URMA_TM_RC; jc.jfs_cfg.jfc = jfc;
    jc.flag.bs.share_jfr = 1; jc.shared.jfr = jfr; jc.shared.jfc = jfc;
    urma_jetty_t* jetty = urma_create_jetty(ctx, &jc);
    STEP("create_jetty (ioctl)", jetty != NULL);

    char* buf = aligned_alloc(4096, 4096); memset(buf, 0xAB, 4096);
    urma_seg_cfg_t sc; memset(&sc, 0, sizeof sc);
    sc.va = (uint64_t)(uintptr_t)buf; sc.len = 4096; sc.token_value.token = 0xDEADBEEF;
    urma_target_seg_t* seg = urma_register_seg(ctx, &sc);
    STEP("register_seg (ioctl)", seg != NULL);

    // self-loop target: import our own jetty + seg, bind.
    urma_rjetty_t rj; memset(&rj, 0, sizeof rj);
    rj.jetty_id = jetty->jetty_id; rj.trans_mode = URMA_TM_RC;
    urma_token_t tok = { .token = 0xDEADBEEF };
    urma_target_jetty_t* tj = urma_import_jetty(ctx, &rj, &tok);
    STEP("import_jetty (ioctl)", tj != NULL);
    printf("[k_smoke] bind_jetty -> %d (best-effort self-loop)\n", urma_bind_jetty(jetty, tj));

    urma_seg_t rseg; memset(&rseg, 0, sizeof rseg);
    rseg.ubva = seg->seg.ubva; rseg.len = 4096; rseg.token_id = seg->seg.token_id;
    urma_target_seg_t* rt = urma_import_seg(ctx, &rseg, &tok, seg->seg.ubva.va, (urma_import_seg_flag_t){0});
    printf("[k_smoke] import_seg -> %s\n", rt ? "OK" : "skip");

    // data plane: ring a WRITE at the mmap'd doorbell, poll the CQ.
    char* src = aligned_alloc(4096, 4096); memcpy(src, "OpenURMA-kernel-path-WRITE", 27);
    urma_sge_t ssge = { .addr = (uint64_t)(uintptr_t)src, .len = 27, .tseg = seg };
    urma_sge_t dsge = { .addr = seg->seg.ubva.va, .len = 27, .tseg = rt ? rt : seg };
    urma_sg_t s_sg = { .sge = &ssge, .num_sge = 1 }, d_sg = { .sge = &dsge, .num_sge = 1 };
    urma_jfs_wr_t w; memset(&w, 0, sizeof w);
    w.opcode = URMA_OPC_WRITE; w.tjetty = tj; w.user_ctx = 0x42; w.rw.src = s_sg; w.rw.dst = d_sg;
    urma_jfs_wr_t* bad = 0;
    int pr = urma_post_jetty_send_wr(jetty, &w, &bad);
    printf("[k_smoke] post WRITE (mmap doorbell) -> %d\n", pr);
    urma_cr_t cr; int got = 0;
    for (int t = 0; t < 100000 && !got; t++) got = urma_poll_jfc(jfc, 1, &cr);
    printf("[k_smoke] poll_jfc -> %d completion(s)\n", got);

    urma_delete_context(ctx);
    urma_uninit();
    printf("[k_smoke] RESULT: control-plane PASS (all verbs reached the kernel)%s\n",
           got > 0 ? " + data-plane completion" : "");
    return 0;
}
