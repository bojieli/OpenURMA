#!/usr/bin/env bash
# Comprehensive end-to-end suite: every stock/official URMA application and test
# the OpenURMA SystemC NIC (Tier S) supports, run back-to-back with a summary.
#   1. urma_perftest   (official UMDK perf tool)        WRITE latency
#   2. verbs ping-pong (WRITE+SEND data integrity)
#   3. URPC umq echo   (official UMDK RPC framework app)
#   4. urma_sample     (official UMDK URMA sample)       WRITE+READ
#   5. kv_store        (key-value store application)     PUT/GET/DELETE + scale
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pass=0; fail=0
run(){ local name="$1"; shift
  if "$@" >/tmp/e2e_$name.log 2>&1; then echo "  [PASS] $name"; pass=$((pass+1));
  else echo "  [FAIL] $name (see /tmp/e2e_$name.log)"; fail=$((fail+1)); fi; }

echo "=== OpenURMA SystemC NIC — end-to-end application suite ==="
run perftest    bash "$HERE/tests/run_perftest.sh" write_lat 64 20
run pingpong    bash "$HERE/tests/run_pingpong.sh"
run urpc_echo   bash "$HERE/tests/run_urpc_echo.sh"
run urma_sample bash "$HERE/tests/run_urma_sample.sh"
run kv_store    bash "$HERE/apps/run_kv_store.sh"
echo "=== SUITE: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ] && echo "ALL_E2E: PASS" || echo "ALL_E2E: FAIL"
exit $fail
