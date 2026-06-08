#!/usr/bin/env bash
# Stock URPC umq echo (UMDK examples/umq/umq_example, unmodified) over the
# OpenURMA Tier-S provider. server: -d openurma0 (wire listen); client:
# -d openurma1 (wire connect). RC mode (-T 1).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$HERE/env.sh"
URPC="${URPC_BUILD:-/tmp/umdk_urpc_build}"
UMQ="$URPC/urpc/examples/umq/umq_example"
ULIBS="/home/ubuntu/OpenURMA/integration/umdk/build/umdk/urma/lib/uvs/core:$URPC/urpc/umq:$URPC/urpc/umq/umq_ub:$URPC/urpc/umq/qbuf:$URPC/urpc/umq/umq_ipc:$URPC/urpc/framework"

ROOT=/tmp/ourpc_$$; WIRE=/tmp/ourpc_$$.wire; PORT="${RPC_PORT:-$((22000 + RANDOM % 200))}"
rm -rf "$ROOT" "$WIRE"*
bash "$HERE/tools/openurma_mkdev.sh" openurma0 fe80::1 "$ROOT" >/dev/null
bash "$HERE/tools/openurma_mkdev.sh" openurma1 fe80::2 "$ROOT" >/dev/null

E=( OPENURMA_FAKE_ROOT="$ROOT"
    LD_LIBRARY_PATH="$OU_LIBURMA:$OU_LIBCOMMON:$ULIBS"
    LD_PRELOAD="$OU_SHIM" OPENURMA_WIRE_PATH="$WIRE"
    OPENURMA_PROVIDER_LOG="${OPENURMA_PROVIDER_LOG:-0}" )

ARGS_COMMON="-T 0 -p $PORT -i 127.0.0.1"
env "${E[@]}" OPENURMA_WIRE_ROLE=listen  timeout 50 "$UMQ"  -d openurma0 --server $ARGS_COMMON ${SRV_EXTRA:-} >/tmp/rpc_server.log 2>&1 &
SRV=$!
sleep 0.8
env "${E[@]}" OPENURMA_WIRE_ROLE=connect timeout 50 "$UMQ"  -d openurma1 --client $ARGS_COMMON ${CLI_EXTRA:-} >/tmp/rpc_client.log 2>&1 &
CLI=$!
wait $CLI; CR=$?; wait $SRV; SR=$?
rm -rf "$ROOT" "$WIRE"*
echo "=== client ==="; grep -iE "bind success|dequeue data" /tmp/rpc_client.log
echo "=== server ==="; grep -iE "bind success|dequeue data" /tmp/rpc_server.log
echo "client_rc=$CR server_rc=$SR"
# Echo success = each side dequeued the peer's message.
if grep -q "dequeue data: hello, this is umq server" /tmp/rpc_client.log 2>/dev/null && \
   grep -q "dequeue data: hello, this is umq client" /tmp/rpc_server.log 2>/dev/null; then
   echo "URPC_ECHO: PASS"; exit 0
else echo "URPC_ECHO: FAIL"; exit 1; fi
