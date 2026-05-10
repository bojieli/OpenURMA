// SPDX-License-Identifier: Apache-2.0
//
// libopenurma — SW-emulator backend.
//
// Maps URMA verbs to in-process SwStream operations against a separately
// linked OpenURMA topology (the auto-generated `kernel_*` functions).
// Each verb either:
//   - configures element state via the SignalChannel-style RPC
//     (urma_create_jetty, urma_register_seg, urma_import_*)
//   - or pushes a WR-shaped flit into the doorbell input stream
//     (urma_post_jetty_send_wr) and reads completion flits out of the
//     completion stream (urma_poll_jfc).

#include "openurma/urma.h"
#include "openurma/ub_flit.hpp"
#include "openclicknp/sw_runtime.hpp"

#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct OpenURMAContext {
    urma_backend_t backend;
    uint32_t       local_cna;
    // SwStream injection points. The owner of these is the test harness;
    // libopenurma wires them via set_swemu_streams().
    openclicknp::SwStream* doorbell_in = nullptr;   // host_stream_1
    openclicknp::SwStream* cqe_out     = nullptr;   // host_stream_0
    std::mutex     m;
    uint32_t       next_jetty_id = 1000;
    uint32_t       next_seg_id   = 1;
    uint32_t       next_jfc_id   = 1;
    uint16_t       next_tassn    = 0;
};

struct OpenURMAJfc {
    OpenURMAContext* ctx;
    uint32_t         id;
    int              depth;
    std::mutex                m;
    std::vector<urma_cr_t>    crs;
};

struct OpenURMAJetty {
    OpenURMAContext* ctx;
    uint32_t         jid;
    OpenURMAJfc*     jfc;
    urma_jetty_cfg_t cfg;
    std::mutex                rq_m;
    std::vector<urma_jfr_wr_t> rq;       // pre-posted Receive Queue Entries
    size_t                    rq_head = 0;
};

struct OpenURMASeg {
    OpenURMAContext* ctx;
    uint32_t         seg_id;
    void*            va;
    size_t           len;
    urma_token_t     token;
};

struct OpenURMATargetJetty {
    urma_jetty_id_t id;
    urma_token_t    token;
};

struct OpenURMATargetSeg {
    urma_seg_id_t id;
    urma_token_t  token;
};

}  // namespace

extern "C" struct urma_context        : public OpenURMAContext {};
extern "C" struct urma_jetty          : public OpenURMAJetty {};
extern "C" struct urma_jfc            : public OpenURMAJfc {};
extern "C" struct urma_seg            : public OpenURMASeg {};
extern "C" struct urma_target_jetty   : public OpenURMATargetJetty {};
extern "C" struct urma_target_seg     : public OpenURMATargetSeg {};

// ---- harness wiring (called by test code or by the FPGA backend setup) ----
extern "C" void openurma_set_swemu_streams(urma_context_t* ctx,
                                           openclicknp::SwStream* doorbell_in,
                                           openclicknp::SwStream* cqe_out) {
    ctx->doorbell_in = doorbell_in;
    ctx->cqe_out     = cqe_out;
}

// ---- API impl ----

extern "C" urma_status_t urma_context_create(urma_backend_t backend,
                                             uint32_t local_cna,
                                             urma_context_t** out) {
    if (backend != URMA_BACKEND_SWEMU) return URMA_E_NOT_IMPLEMENTED;
    auto* c = new urma_context_t;
    c->backend = backend;
    c->local_cna = local_cna;
    *out = c;
    return URMA_OK;
}

extern "C" urma_status_t urma_context_destroy(urma_context_t* ctx) {
    delete ctx;
    return URMA_OK;
}

extern "C" urma_status_t urma_create_jfc(urma_context_t* ctx, int depth,
                                          urma_jfc_t** out) {
    auto* j = new urma_jfc_t;
    j->ctx = ctx;
    j->depth = depth;
    {
        std::lock_guard<std::mutex> lk(ctx->m);
        j->id = ctx->next_jfc_id++;
    }
    *out = j;
    return URMA_OK;
}

extern "C" urma_status_t urma_destroy_jfc(urma_jfc_t* jfc) {
    delete jfc;
    return URMA_OK;
}

extern "C" urma_status_t urma_create_jetty(urma_context_t* ctx,
                                            const urma_jetty_cfg_t* cfg,
                                            urma_jetty_t** out) {
    auto* j = new urma_jetty_t;
    j->ctx = ctx;
    j->cfg = *cfg;
    j->jfc = nullptr;   // bind via cfg->jfc_id at create_jfc time in v2
    {
        std::lock_guard<std::mutex> lk(ctx->m);
        j->jid = ctx->next_jetty_id++;
    }
    *out = j;
    return URMA_OK;
}

extern "C" urma_status_t urma_destroy_jetty(urma_jetty_t* j) {
    delete j;
    return URMA_OK;
}

extern "C" urma_status_t urma_import_jetty(urma_context_t* ctx, urma_jetty_id_t id,
                                            urma_token_t token,
                                            urma_target_jetty_t** out) {
    (void)ctx;
    auto* t = new urma_target_jetty_t;
    t->id = id;
    t->token = token;
    *out = t;
    return URMA_OK;
}

extern "C" urma_status_t urma_register_seg(urma_context_t* ctx, void* va, size_t len,
                                            urma_token_t token, urma_seg_t** out) {
    auto* s = new urma_seg_t;
    s->ctx = ctx;
    s->va = va;
    s->len = len;
    s->token = token;
    {
        std::lock_guard<std::mutex> lk(ctx->m);
        s->seg_id = ctx->next_seg_id++;
    }
    *out = s;
    return URMA_OK;
}

extern "C" urma_status_t urma_unregister_seg(urma_seg_t* s) {
    delete s;
    return URMA_OK;
}

extern "C" urma_status_t urma_import_seg(urma_context_t* ctx, urma_seg_id_t id,
                                          urma_token_t token,
                                          urma_target_seg_t** out) {
    (void)ctx;
    auto* t = new urma_target_seg_t;
    t->id = id;
    t->token = token;
    *out = t;
    return URMA_OK;
}

namespace {

// Convert a URMA WR into a pair of metadata + extension flits and push them
// into the doorbell stream. Returns OK on success.
urma_status_t post_wr_to_swemu(urma_jetty_t* j, const urma_jfs_wr_t* wr) {
    auto* ctx = j->ctx;
    if (!ctx->doorbell_in) return URMA_E_FAULT;

    openurma::ub_meta m{};
    m.set_dcna(0);            // must be set by caller via target_jetty/seg ID
    m.set_scna(ctx->local_cna);
    m.set_lbf(0);
    m.set_sl(0);
    m.set_nth_nlp(wr->svc == URMA_SVC_UNO ? openurma::NTH_NLP_UTPH
                                          : openurma::NTH_NLP_RTPH);
    m.set_valid(true);

    m.set_tp_opcode(0);   // filled by TPChannel_TX
    m.set_tp_ver(0);
    m.set_padding(0);
    m.set_rtph_nlp(0);
    m.set_psn(0);
    m.set_tpmsn(0);
    m.set_a_flag(true);
    m.set_f_flag(false);
    uint8_t svc;
    switch (wr->svc) {
        case URMA_SVC_ROI: svc = openurma::SVC_ROI; break;
        case URMA_SVC_ROT: svc = openurma::SVC_ROT; break;
        case URMA_SVC_ROL: svc = openurma::SVC_ROL; break;
        case URMA_SVC_UNO: svc = openurma::SVC_UNO; break;
        default: svc = openurma::SVC_ROL; break;
    }
    m.set_svc_mode(svc);
    m.set_is_response(false);
    m.set_last_pkt(true);   // single-packet WR for the MVP

    uint8_t taop;
    switch (wr->opc) {
        case URMA_OPC_READ:        taop = openurma::TAOP_READ; break;
        case URMA_OPC_WRITE:       taop = openurma::TAOP_WRITE; break;
        case URMA_OPC_SEND:        taop = openurma::TAOP_SEND; break;
        case URMA_OPC_ATOMIC_CAS:  taop = openurma::TAOP_ATOMIC_CAS; break;
        case URMA_OPC_ATOMIC_FAA:  taop = openurma::TAOP_ATOMIC_FAA; break;
        default: return URMA_E_INVAL;
    }
    m.set_ta_opcode(taop);
    m.set_ta_ver(0);
    m.set_ee_bits(1);
    m.set_tv_en(true);
    m.set_poison(false);
    m.set_target_hint(false);
    m.set_ud_flag(false);
    {
        std::lock_guard<std::mutex> lk(ctx->m);
        uint16_t ts = wr->tassn ? wr->tassn : ctx->next_tassn++;
        m.set_ini_tassn(ts);
    }
    uint8_t odr_exec = (wr->order == URMA_ORD_NO) ? openurma::ODR_NO
                     : (wr->order == URMA_ORD_RO) ? openurma::ODR_RO
                     :                              openurma::ODR_SO;
    m.set_odr_exec(odr_exec);
    m.set_odr_compl(wr->completion_in_order != 0);
    m.set_fence(wr->fence != 0);
    m.set_mt_en(taop == openurma::TAOP_SEND || taop == openurma::TAOP_SEND_IMM);
    m.set_fce(wr->fce != 0);
    m.set_retry(false);
    m.set_alloc(false);
    m.set_ini_rc_type(0);
    m.set_e_bit(false);
    m.set_ini_rc_id(wr->rc_id);
    m.f.set_sop(true);
    m.f.set_eop(false);
    ctx->doorbell_in->write(m.f);

    openurma::ub_ext e{};
    e.set_address(wr->remote_va);
    e.set_token_id(wr->token_id);
    e.set_length(wr->length);
    e.set_token_value(wr->token);
    e.set_op_data((wr->opc == URMA_OPC_ATOMIC_CAS) ? wr->cas_compare : wr->immediate);
    e.f.set_sop(false);
    e.f.set_eop(true);
    ctx->doorbell_in->write(e.f);

    return URMA_OK;
}

}  // namespace

extern "C" urma_status_t urma_post_jetty_send_wr(urma_jetty_t* j,
                                                  const urma_jfs_wr_t* wr) {
    return post_wr_to_swemu(j, wr);
}

extern "C" urma_status_t urma_post_jetty_recv_wr(urma_jetty_t* j,
                                                  const urma_jfr_wr_t* wr) {
    if (!j || !wr) return URMA_E_INVAL;
    std::lock_guard<std::mutex> lk(j->rq_m);
    if (j->cfg.rq_depth > 0 &&
        (int)(j->rq.size() - j->rq_head) >= j->cfg.rq_depth) {
        return URMA_E_NOMEM;
    }
    j->rq.push_back(*wr);
    return URMA_OK;
}

extern "C" urma_status_t urma_consume_recv_wr(urma_jetty_t* j,
                                              urma_jfr_wr_t* out) {
    // Internal helper used by Send-handler completion path: pop one
    // pre-posted RQE matching the jetty's RQ. Returns URMA_E_NOMEM if
    // the RQ is empty (the caller will surface RNR upstream).
    if (!j || !out) return URMA_E_INVAL;
    std::lock_guard<std::mutex> lk(j->rq_m);
    if (j->rq_head >= j->rq.size()) return URMA_E_NOMEM;
    *out = j->rq[j->rq_head++];
    if (j->rq_head > 64 && j->rq_head * 2 > j->rq.size()) {
        j->rq.erase(j->rq.begin(), j->rq.begin() + j->rq_head);
        j->rq_head = 0;
    }
    return URMA_OK;
}

extern "C" int urma_poll_jfc(urma_jfc_t* jfc, int max, urma_cr_t* out) {
    auto* ctx = jfc->ctx;
    if (!ctx->cqe_out) return 0;
    int got = 0;
    // Drain pending CR queue first.
    {
        std::lock_guard<std::mutex> lk(jfc->m);
        while (!jfc->crs.empty() && got < max) {
            out[got++] = jfc->crs.front();
            jfc->crs.erase(jfc->crs.begin());
        }
    }
    while (got < max) {
        openclicknp::flit_t f;
        if (!ctx->cqe_out->read_nb(f)) break;
        urma_cr_t cr{};
        uint64_t l0 = f.get(0);
        cr.status   = (urma_status_t)((l0 >> 56) & 0x7);
        cr.tassn    = (uint16_t)((l0 >> 32) & 0xFFFF);
        uint8_t taop = (uint8_t)((l0 >> 24) & 0xFF);
        cr.rc_id    = (uint32_t)(l0 & 0xFFFFFu);
        switch (taop) {
            case openurma::TAOP_READ_RESP:    cr.opc = URMA_OPC_READ; break;
            case openurma::TAOP_ATOMIC_RESP:  cr.opc = URMA_OPC_ATOMIC_CAS; break;
            default:                           cr.opc = URMA_OPC_WRITE; break;
        }
        // Look for the optional second flit carrying read/atomic data.
        openclicknp::flit_t f2;
        if (ctx->cqe_out->read_nb(f2)) {
            cr.atomic_orig = f2.get(3);
        }
        out[got++] = cr;
    }
    return got;
}

extern "C" urma_status_t urma_rearm_jfc(urma_jfc_t* jfc) {
    (void)jfc;
    return URMA_OK;
}

extern "C" urma_status_t urma_wait_jfc(urma_jfc_t* jfc, int timeout_ms) {
    (void)jfc;
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    return URMA_OK;
}
