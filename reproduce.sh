#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# reproduce.sh — one entry point to build, test, and reproduce OpenURMA
# from a clean checkout. Run from the repo root.
#
# Usage:
#   ./reproduce.sh doctor   check prerequisites (toolchains, libs) and exit
#   ./reproduce.sh smoke    build + SW-emu tests + verify headline claims  (~2 min)
#   ./reproduce.sh tests    just the SW-emu correctness suite
#   ./reproduce.sh verify   just the headline-claims check (builds the sim)
#   ./reproduce.sh paper     full dataset + every figure + rebuild the PDF (~15 min)
#   ./reproduce.sh           same as 'smoke'
#
# Env:
#   OPENCLICKNP_ROOT   path to a built OpenClickNP (default ~/OpenClickNP)
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "${ROOT}"
export OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-$HOME/OpenClickNP}"

c_grn=$'\033[32m'; c_red=$'\033[31m'; c_yel=$'\033[33m'; c_bld=$'\033[1m'; c_0=$'\033[0m'
say()  { printf '%s==> %s%s\n' "$c_bld" "$*" "$c_0"; }
ok()   { printf '  %sok%s   %s\n'   "$c_grn" "$c_0" "$*"; }
warn() { printf '  %swarn%s %s\n'   "$c_yel" "$c_0" "$*"; }
bad()  { printf '  %sFAIL%s %s\n'   "$c_red" "$c_0" "$*"; }

doctor() {  # returns non-zero only if a HARD prerequisite is missing
  local hard=0
  say "Checking prerequisites"

  if command -v g++ >/dev/null; then
    local gv; gv=$(g++ -dumpfullversion 2>/dev/null | cut -d. -f1)
    if [ "${gv:-0}" -ge 11 ] 2>/dev/null; then ok "g++ $(g++ -dumpfullversion) (>= 11)"
    else bad "g++ ${gv} too old (need >= 11)"; hard=1; fi
  else bad "g++ not found (need >= 11)"; hard=1; fi

  if [ -x "${OPENCLICKNP_ROOT}/build/compiler/openclicknp-cc" ]; then
    ok "OpenClickNP compiler at ${OPENCLICKNP_ROOT}"
  else
    bad "OpenClickNP compiler not found at ${OPENCLICKNP_ROOT}/build/compiler/openclicknp-cc"
    echo "       git clone https://github.com/bojieli/OpenClickNP.git \"${OPENCLICKNP_ROOT}\" && \\"
    echo "       cmake -B \"${OPENCLICKNP_ROOT}/build\" -S \"${OPENCLICKNP_ROOT}\" -DCMAKE_BUILD_TYPE=Release && \\"
    echo "       cmake --build \"${OPENCLICKNP_ROOT}/build\" -j   # then set OPENCLICKNP_ROOT if needed"
    hard=1
  fi

  if echo '#include <systemc.h>' | g++ -x c++ -c - -o /dev/null 2>/dev/null; then
    ok "SystemC headers found (two-node sim + tests/systemc)"
  else
    warn "SystemC not found — SW-emu tier works; SystemC/two-node tiers need it"
    echo "       Debian/Ubuntu: sudo apt-get install libsystemc-dev"
  fi

  if python3 -c 'import matplotlib' 2>/dev/null; then ok "python3 + matplotlib (figure regeneration)"
  else warn "python3/matplotlib missing — needed only to regenerate figures (pip install matplotlib)"; fi

  command -v vitis_hls >/dev/null && ok "Vitis HLS (optional, RTL tier)" || warn "Vitis HLS absent (optional — RTL synthesis tier only)"
  command -v vivado    >/dev/null && ok "Vivado (optional, RTL tier)"     || warn "Vivado absent (optional — RTL place & route tier only)"

  return $hard
}

run_tests() { say "SW-emu correctness suite (17 tests)"; bash "${ROOT}/scripts/run_all_tests.sh"; }

verify() { say "Verifying headline paper claims"; bash "${ROOT}/eval/twonode/verify_claims.sh"; }

smoke() {
  doctor || { bad "Missing a hard prerequisite — see above."; exit 1; }
  run_tests || { bad "SW-emu tests failed."; exit 1; }
  verify    || { bad "Headline claims not reproduced."; exit 1; }
  say "Smoke reproduction complete — tests pass and headline claims verified."
}

paper() {
  doctor || { bad "Missing a hard prerequisite — see above."; exit 1; }
  run_tests
  say "Regenerating full evaluation dataset + figures"
  bash "${ROOT}/eval/twonode/reproduce_figures.sh"
  say "Rebuilding the paper PDF"
  ( cd "${ROOT}/paper" && make )
  say "Full reproduction complete — see paper/main.pdf"
}

case "${1:-smoke}" in
  doctor) doctor ;;
  tests)  run_tests ;;
  verify) verify ;;
  smoke)  smoke ;;
  paper)  paper ;;
  -h|--help|help) sed -n '3,20p' "$0" ;;
  *) echo "unknown mode '$1' (try: doctor | smoke | tests | verify | paper)"; exit 2 ;;
esac
