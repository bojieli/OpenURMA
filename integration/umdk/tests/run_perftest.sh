#!/usr/bin/env bash
# Stock urma_perftest (UMDK, unmodified) over the OpenURMA Tier-S provider.
# server: -d openurma0 (wire listen); client: -d openurma1 -S 127.0.0.1 (connect).
# RC mode (-p 1). Usage: run_perftest.sh <verb> [size] [iters]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$HERE/env.sh"
VERB="${1:-write_lat}"; SIZE="${2:-64}"; ITERS="${3:-20}"
PT="$OU_BIN_URMA/urma_perftest/urma_perftest"

ROOT=/tmp/oupt_$$; WIRE=/tmp/oupt_$$.wire; PORT="${PT_PORT:-$((21500 + RANDOM % 200))}"
rm -rf "$ROOT" "$WIRE"*
bash "$HERE/tools/openurma_mkdev.sh" openurma0 fe80::1 "$ROOT" >/dev/null
bash "$HERE/tools/openurma_mkdev.sh" openurma1 fe80::2 "$ROOT" >/dev/null

E=( OPENURMA_FAKE_ROOT="$ROOT" LD_LIBRARY_PATH="$OU_LIBURMA:$OU_LIBCOMMON"
    LD_PRELOAD="$OU_SHIM" OPENURMA_WIRE_PATH="$WIRE"
    OPENURMA_PROVIDER_LOG="${OPENURMA_PROVIDER_LOG:-0}" )

env "${E[@]}" OPENURMA_WIRE_ROLE=listen  timeout 60 "$PT" "$VERB" -d openurma0 -p 1 -P "$PORT" -n "$ITERS" -s "$SIZE" >/tmp/pt_server.log 2>&1 &
SRV=$!
sleep 0.6
env "${E[@]}" OPENURMA_WIRE_ROLE=connect timeout 60 "$PT" "$VERB" -d openurma1 -S 127.0.0.1 -p 1 -P "$PORT" -n "$ITERS" -s "$SIZE" >/tmp/pt_client.log 2>&1 &
CLI=$!
wait $CLI; CR=$?; wait $SRV; SR=$?
rm -rf "$ROOT" "$WIRE"*
echo "=== client ($VERB s=$SIZE n=$ITERS) ==="; tail -25 /tmp/pt_client.log
echo "client_rc=$CR server_rc=$SR"
