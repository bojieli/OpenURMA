#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# run_verilator.sh — Verilator unit-test for one HLS-synthesized kernel.
# Drives a small testbench against the post-HLS RTL and validates
# cycle-accurate behavior (the Verilator backend does not depend on
# Vivado).
set -uo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

KERNEL="${1:-doorbell}"
RTL="${ROOT}/build/hls/${KERNEL}_proj/solution1/syn/verilog"
OUT="${ROOT}/build/verilator/${KERNEL}"
mkdir -p "${OUT}"

if [[ ! -d "${RTL}" ]]; then
    echo "No RTL at ${RTL}; run synth_hls.sh first." >&2
    exit 1
fi

cat >"${OUT}/tb.cpp" <<'EOF'
#include "V${TOP}.h"
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <cstdio>
#include <cstdint>

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto* dut = new V${TOP};
    Verilated::traceEverOn(true);
    auto* tfp = new VerilatedVcdC; dut->trace(tfp, 99);
    tfp->open("trace.vcd");

    // Reset.
    dut->ap_clk = 0; dut->ap_rst_n = 0;
    dut->in_1_TVALID = 0;
    dut->out_1_TREADY = 1;
    for (int i = 0; i < 10; ++i) {
        dut->ap_clk = !dut->ap_clk; dut->eval(); tfp->dump(i);
    }
    dut->ap_rst_n = 1;
    int tick = 10;
    int n_in = 0, n_out = 0;
    int max_ticks = 200;
    while (tick < max_ticks) {
        dut->ap_clk = !dut->ap_clk; dut->eval(); tfp->dump(tick);

        if (dut->ap_clk == 1) {
            // Clocked behaviour: drive inputs / sample outputs after posedge.
            if (n_in < 3 && dut->in_1_TREADY) {
                dut->in_1_TVALID = 1;
                // TDATA is 512 bits; Verilator presents it as a uint32 array.
                for (int j = 0; j < 16; ++j) dut->in_1_TDATA[j] = 0xAB000000 + n_in;
                n_in++;
            } else {
                dut->in_1_TVALID = 0;
            }
            if (dut->out_1_TVALID) {
                n_out++;
            }
        }
        tick++;
    }

    tfp->close(); delete tfp; delete dut;
    std::printf("Verilator: ticks=%d  flits_in=%d  flits_out=%d\n", tick, n_in, n_out);
    return (n_out >= 1) ? 0 : 1;
}
EOF

# Substitute the top module name. doorbell's top is just "doorbell".
sed -i "s/\${TOP}/${KERNEL}/g" "${OUT}/tb.cpp"

cd "${OUT}"
verilator --cc --exe --build --trace -O2 -Wno-fatal -Wno-WIDTH -Wno-CASEINCOMPLETE -Wno-UNOPTFLAT -Wno-PINMISSING \
    --top-module "${KERNEL}" \
    -y "${RTL}" \
    "${RTL}/${KERNEL}.v" "${OUT}/tb.cpp" 2>&1 | tail -8

echo "[verilator] running ${KERNEL}"
"./obj_dir/V${KERNEL}" 2>&1 | tail -3
