#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# verify_claims.sh — assert the paper's headline quantitative claims
# against a freshly built simulator, with tolerances. Exits non-zero if
# any claim is off, so it doubles as a regression test for the model.
#
# Claims checked:
#   C1  UB §8.3 LOAD, 64 B remote fetch        ≈  500 ns
#   C2  RoCEv2 RC READ, 64 B (matched baseline) ≈ 2236 ns
#   C3  Load/store latency advantage            ≈ 4.47x   (C2/C1)
#   C4  NIC-resident state at (1024,1024) apps/peers: UB/RoCE ratio  > 1000x
#       (paper claims 4855x; O(N+M) vs O(N*M))
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="$(cd "${HERE}/../.." && pwd)"
SIM="${ROOT}/build/twonode_sim"

pass=0; fail=0
check() {  # check <label> <actual> <expected> <tol-fraction>
  local label="$1" actual="$2" expected="$3" tol="$4"
  local ok
  ok=$(awk -v a="$actual" -v e="$expected" -v t="$tol" \
        'BEGIN{d=(a>e)?a-e:e-a; print (d<=e*t)?"1":"0"}')
  if [ "$ok" = "1" ]; then
    printf '  \033[32mPASS\033[0m %-46s actual=%-9s expected=%s (±%.0f%%)\n' \
           "$label" "$actual" "$expected" "$(awk -v t="$tol" 'BEGIN{print t*100}')"
    pass=$((pass+1))
  else
    printf '  \033[31mFAIL\033[0m %-46s actual=%-9s expected=%s (±%.0f%%)\n' \
           "$label" "$actual" "$expected" "$(awk -v t="$tol" 'BEGIN{print t*100}')"
    fail=$((fail+1))
  fi
}

mean() {  # mean <stack> <verb>: run one 64B/100ns/conc1 op, echo mean ns
  "${SIM}" --stack "$1" --workload ptr_chase --verb "$2" \
      --n-ops 500 --link-delay-ns 100 --concurrency 1 --payload-bytes 64 2>/dev/null \
    | sed -n 's/.*mean=\([0-9.]*\).*/\1/p'
}

echo "=== Verifying OpenURMA headline claims ==="

[ -x "${SIM}" ] || bash "${HERE}/build_twonode.sh" >/dev/null

UB_LS=$(mean ub_loadstore load)
ROCE=$(mean roce_dma read)
RATIO=$(awk -v r="$ROCE" -v u="$UB_LS" 'BEGIN{printf "%.2f", r/u}')

check "C1 UB §8.3 LOAD 64B latency (ns)"      "$UB_LS" 500  0.05
check "C2 RoCE RC READ 64B latency (ns)"      "$ROCE"  2236 0.05
check "C3 load/store advantage (x)"           "$RATIO" 4.47 0.05

# C4: state scaling.
STATE_BIN=/tmp/openurma_state_size
g++ -std=c++17 -O2 "${ROOT}/eval/state_size.cpp" -o "${STATE_BIN}" 2>/dev/null
RATIO_1K=$("${STATE_BIN}" 2>/dev/null \
  | awk '$1==1024 && $2==1024 {for(i=1;i<=NF;i++) if($i ~ /^[0-9.]+x$/){r=$i; sub(/x/,"",r); print r; exit}}')
check "C4 state ratio at (1024,1024) apps/peers" "${RATIO_1K:-0}" 4855 0.10

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ] && echo "  All headline claims verified." || echo "  Some claims FAILED."
exit "$fail"
