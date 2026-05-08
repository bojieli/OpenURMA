#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENROCE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"
XILINX_DIR="${XILINX_DIR:-/home/ubuntu/Xilinx/2025.2}"
USER_CLOCK_HZ="${USER_CLOCK_HZ:-322265625}"
JOBS="${JOBS:-$(nproc)}"

source "${XILINX_DIR}/Vitis/settings64.sh"

GEN="${OPENROCE_ROOT}/build/openroce_gen"
BD="${OPENROCE_ROOT}/build/hls"
mkdir -p "${BD}"

CLK_PERIOD_NS=$(awk -v hz="${USER_CLOCK_HZ}" 'BEGIN{printf "%.3f", 1e9/hz}')

KERNELS=()
while IFS= read -r f; do
    KERNELS+=("$(basename "$f" .cpp)")
done < <(find "${GEN}/kernels" -maxdepth 1 -name '*.cpp' ! -name '*_tb.cpp' -print | sort)

echo "Synthesising ${#KERNELS[@]} OpenRoCE kernels at ${CLK_PERIOD_NS} ns / ${USER_CLOCK_HZ} Hz with ${JOBS} parallel jobs"

run_one() {
    local k="$1"
    local proj_dir="${BD}/${k}_proj"
    local tcl="${BD}/${k}_synth.tcl"
    cat >"${tcl}" <<EOF
open_project -reset ${proj_dir}
add_files ${GEN}/kernels/${k}.cpp -cflags {-I ${OPENCLICKNP_ROOT}/runtime/include -I ${OPENROCE_ROOT}/runtime/openroce/include -std=c++17}
set_top ${k}
open_solution -reset solution1
set_part xcu50-fsvh2104-2-e
create_clock -period ${CLK_PERIOD_NS} -name default
csynth_design
exit
EOF
    echo "  vitis_hls ${k}"
    (cd "${BD}" && vitis_hls -f "${tcl}" >"${BD}/${k}.log" 2>&1) || {
        echo "FAIL: ${k}" >&2
    }
}

export -f run_one
export GEN BD OPENCLICKNP_ROOT OPENROCE_ROOT CLK_PERIOD_NS

printf '%s\n' "${KERNELS[@]}" | xargs -n1 -P "${JOBS}" -I{} bash -c 'run_one "$@"' _ {}

echo "Done. Aggregating..."
"${SCRIPT_DIR}/hls_summary.sh" "${BD}" || true
