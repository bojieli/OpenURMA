#!/usr/bin/env bash
# Two-process verbs ping-pong (server + client) over the OpenURMA Tier-S
# provider: two device names → two CNAs, a UNIX-datagram UB wire, and a TCP
# out-of-band handshake. Validates WRITE + SEND data integrity end-to-end
# through stock liburma verbs.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$HERE/env.sh"

ROOT=/tmp/oupp_$$; WIRE=/tmp/oupp_$$.wire; PORT="${PP_PORT:-$((21300 + RANDOM % 200))}"
rm -rf "$ROOT" "$WIRE"*
bash "$HERE/tools/openurma_mkdev.sh" openurma0 fe80::1 "$ROOT" >/dev/null
bash "$HERE/tools/openurma_mkdev.sh" openurma1 fe80::2 "$ROOT" >/dev/null

E=( OPENURMA_FAKE_ROOT="$ROOT" LD_LIBRARY_PATH="$OU_LIBURMA:$OU_LIBCOMMON"
    LD_PRELOAD="$OU_SHIM" PP_PORT="$PORT" OPENURMA_WIRE_PATH="$WIRE"
    OPENURMA_PROVIDER_LOG="${OPENURMA_PROVIDER_LOG:-0}" )

env "${E[@]}" PP_DEV=openurma0 OPENURMA_WIRE_ROLE=listen  timeout 40 "$HERE/build/tier_s/verbs_pingpong" server >/tmp/pp_server.log 2>&1 &
SRV=$!
sleep 0.5
env "${E[@]}" PP_DEV=openurma1 OPENURMA_WIRE_ROLE=connect timeout 40 "$HERE/build/tier_s/verbs_pingpong" client >/tmp/pp_client.log 2>&1 &
CLI=$!
wait $CLI; CR=$?
wait $SRV; SR=$?
rm -rf "$ROOT" "$WIRE"*
echo "=== client ==="; grep -E "CLIENT:" /tmp/pp_client.log
echo "=== server ==="; grep -E "SERVER:" /tmp/pp_server.log
echo "client_rc=$CR server_rc=$SR"
[[ $CR -eq 0 && $SR -eq 0 ]] && { echo "PINGPONG: PASS"; exit 0; } || { echo "PINGPONG: FAIL"; exit 1; }
