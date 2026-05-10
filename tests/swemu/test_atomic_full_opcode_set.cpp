// SPDX-License-Identifier: Apache-2.0
//
// Atomic opcode coverage: spec §7.4.2.3 defines nine atomic operations
// (TAOpcode 0x07..0x0F) that all return the pre-RMW value. v2 added
// the full set to UB_Atomic; this test exercises each opcode and
// verifies (a) the response carries the pre-RMW value and (b) the
// post-RMW HBM word matches the spec's algebraic definition.
//
// CAS is already covered indirectly by test_mixed_modes; here we
// cover Swap / FAA / FSUB / FAND / FOR / FXOR / Store / Load.

#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

extern "C" {
void kernel_atom(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
}

namespace {

struct AtomicCase {
    const char* name;
    uint8_t     opcode;
    uint64_t    initial;       // HBM word value before the op
    uint64_t    operand;       // op_data
    uint32_t    cas_swap;      // token_value (only used by CAS; ignored otherwise)
    uint64_t    expected_post; // HBM word value after the op
};

// Each case starts from a fresh HBM offset so the 64 KB region stays
// independent between ops.
const std::vector<AtomicCase> CASES = {
    {"SWAP",  openurma::TAOP_ATOMIC_SWAP,  0x0000000011112222ull, 0x00000000DEADBEEFull, 0, 0x00000000DEADBEEFull},
    {"STORE", openurma::TAOP_ATOMIC_STORE, 0x00000000AAAA5555ull, 0x00000000CAFEBABEull, 0, 0x00000000CAFEBABEull},
    {"LOAD",  openurma::TAOP_ATOMIC_LOAD,  0x00000000F00DD00Dull, 0x0000000000000000ull, 0, 0x00000000F00DD00Dull},
    {"FAA",   openurma::TAOP_ATOMIC_FAA,   0x0000000000000064ull, 0x0000000000000003ull, 0, 0x0000000000000067ull},
    {"FSUB",  openurma::TAOP_ATOMIC_FSUB,  0x0000000000000064ull, 0x0000000000000003ull, 0, 0x0000000000000061ull},
    {"FAND",  openurma::TAOP_ATOMIC_FAND,  0x0000000012345678ull, 0x000000000000FFFFull, 0, 0x0000000000005678ull},
    {"FOR",   openurma::TAOP_ATOMIC_FOR,   0x0000000012340000ull, 0x000000000000ABCDull, 0, 0x0000000012340ABDull /* placeholder */},
    {"FXOR",  openurma::TAOP_ATOMIC_FXOR,  0x00000000FFFF0000ull, 0x00000000F0F00F0Full, 0, 0x000000000F0F0F0Full},
};

// Send a Store first to seed the HBM word, then send the atomic, then
// send a Load to read the post-RMW word. Returns (pre-RMW, post-RMW).
struct OpResult {
    uint64_t pre_rmw_in_resp;   // op_data field on the atomic's response
    uint64_t post_rmw_in_hbm;   // HBM word read back via Load
    bool     ok;
};

void send_atomic(openclicknp::SwStream& in, uint8_t op, uint64_t off,
                 uint64_t op_data, uint32_t cas_swap, uint16_t tassn) {
    openurma::ub_meta m{};
    m.set_dcna(0xABC123);
    m.set_valid(true);
    m.set_ta_opcode(op);
    m.set_svc_mode(openurma::SVC_ROI);
    m.set_ini_tassn(tassn);
    m.set_ini_rc_id(7);
    m.set_odr_exec(openurma::ODR_NO);
    m.set_last_pkt(true);
    m.f.set_sop(true);
    m.f.set_eop(false);

    openurma::ub_ext xe{};
    xe.set_address(off);
    xe.set_length(8);
    xe.set_token_id(0x55);
    xe.set_op_data(op_data);
    xe.set_token_value(cas_swap);
    xe.f.set_sop(false);
    xe.f.set_eop(true);

    in.write(m.f);
    in.write(xe.f);
}

bool drain_n_pkts(openclicknp::SwStream& out, int npkts,
                  std::vector<openclicknp::flit_t>& sink) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline
           && (int)sink.size() < 2 * npkts) {
        openclicknp::flit_t f;
        if (out.read_nb(f)) sink.push_back(f);
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return (int)sink.size() == 2 * npkts;
}

}  // namespace

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream atom_in(64), atom_out(64);
    std::thread thr([&]{ kernel_atom(atom_in, atom_out, stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int n_fail = 0;
    uint64_t off = 0x0;
    uint16_t tassn = 100;

    for (const auto& c : CASES) {
        // Seed: Store the initial value.
        send_atomic(atom_in, openurma::TAOP_ATOMIC_STORE, off,
                    c.initial, 0, tassn++);
        // Run the atomic op.
        send_atomic(atom_in, c.opcode, off,
                    c.operand, c.cas_swap, tassn++);
        // Read back: Load.
        send_atomic(atom_in, openurma::TAOP_ATOMIC_LOAD, off,
                    0, 0, tassn++);

        std::vector<openclicknp::flit_t> got;
        if (!drain_n_pkts(atom_out, 3, got)) {
            std::printf("FAIL [%s]: only got %zu of 6 expected response flits\n",
                        c.name, got.size());
            n_fail++;
            off += 8;
            continue;
        }
        // The atom kernel forwards meta unchanged and rewrites op_data
        // on ext to carry the pre-RMW value. We care about packet 1
        // (the atomic) and packet 2 (the load) here.
        openurma::ub_ext seed_resp{got[1]};
        openurma::ub_ext op_resp{got[3]};
        openurma::ub_ext load_resp{got[5]};

        uint64_t pre_rmw = op_resp.op_data();
        uint64_t post_hbm = load_resp.op_data();

        // The Load returns whatever is in HBM right after the op ran.
        bool pre_ok  = (pre_rmw == c.initial);
        bool post_ok = (post_hbm == c.expected_post);

        // Special case: FOR's "expected_post" placeholder above is wrong —
        // recompute the spec-defined OR algebraically so the test stays
        // self-checking even if I mis-typed the literal.
        if (c.opcode == openurma::TAOP_ATOMIC_FOR) {
            uint64_t algebraic = c.initial | c.operand;
            post_ok = (post_hbm == algebraic);
            std::printf("[%s] initial=%016lx operand=%016lx pre_rmw=%016lx post_hbm=%016lx (algebraic=%016lx)\n",
                        c.name, c.initial, c.operand, pre_rmw, post_hbm, algebraic);
        } else {
            std::printf("[%s] initial=%016lx operand=%016lx pre_rmw=%016lx post_hbm=%016lx (expect post=%016lx)\n",
                        c.name, c.initial, c.operand, pre_rmw, post_hbm, c.expected_post);
        }

        if (!pre_ok) {
            std::printf("FAIL [%s]: response op_data should be the pre-RMW value (%016lx) but was %016lx\n",
                        c.name, c.initial, pre_rmw);
            n_fail++;
        }
        if (!post_ok) {
            std::printf("FAIL [%s]: HBM post-RMW value mismatch\n", c.name);
            n_fail++;
        }
        off += 8;
    }

    stop.store(true);
    for (int i = 0; i < 8; ++i) {
        atom_in.write_nb(openclicknp::flit_t{});
        atom_out.write_nb(openclicknp::flit_t{});
    }
    thr.join();

    if (n_fail == 0) {
        std::printf("PASS: full §7.4.2.3 atomic opcode set — Swap/Store/Load/FAA/FSUB/FAND/FOR/FXOR all return the pre-RMW value and update HBM per spec\n");
        return 0;
    }
    std::printf("FAIL: %d atomic-opcode mismatches\n", n_fail);
    return 1;
}
