#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENROCE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
XILINX_DIR="${XILINX_DIR:-/home/ubuntu/Xilinx/2025.2}"
USER_CLOCK_NS="${USER_CLOCK_NS:-3.106}"

source "${XILINX_DIR}/Vivado/settings64.sh"

KERNEL="${1:-retrans}"
HLS_PROJ="${OPENROCE_ROOT}/build/hls/${KERNEL}_proj"
RTL_DIR="${HLS_PROJ}/solution1/syn/verilog"
OUT="${OPENROCE_ROOT}/build/vivado/${KERNEL}"
mkdir -p "${OUT}"

if [[ ! -d "${RTL_DIR}" ]]; then
    echo "RTL dir ${RTL_DIR} not found" >&2
    exit 1
fi

cat >"${OUT}/${KERNEL}_synth.tcl" <<EOF
create_project -force -part xcu50-fsvh2104-2-e ${KERNEL} ${OUT}/${KERNEL}_proj
set_property target_language Verilog [current_project]
add_files [glob ${RTL_DIR}/*.v]
import_files -force
set_property top ${KERNEL} [current_fileset]
synth_design -mode out_of_context -top ${KERNEL} -part xcu50-fsvh2104-2-e
create_clock -period ${USER_CLOCK_NS} -name ap_clk [get_ports ap_clk]
report_utilization -file ${OUT}/utilization_synth.rpt
report_timing_summary -delay_type min_max -file ${OUT}/timing_summary_synth.rpt
opt_design
place_design
route_design
report_utilization -file ${OUT}/utilization_route.rpt
report_timing_summary -delay_type min_max -file ${OUT}/timing_summary_route.rpt
write_checkpoint ${OUT}/${KERNEL}_routed.dcp
exit
EOF

echo "[vivado-roce] synth+impl ${KERNEL} → ${OUT}"
(cd "${OUT}" && vivado -mode batch -source "${OUT}/${KERNEL}_synth.tcl" -nojournal -log "${OUT}/vivado.log" 2>&1) | tail -5
