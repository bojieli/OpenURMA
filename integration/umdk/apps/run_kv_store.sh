#!/usr/bin/env bash
# Key-value store (apps/kv_store.c) over the OpenURMA SystemC NIC (Tier S).
# Two processes: a KV server (hash table) + a client running a PUT/GET/DELETE
# workload, the KV RPC carried by stock URMA SEND/RECV verbs.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$HERE/env.sh"
KV="$HERE/build/apps/kv_store"
ROOT=/tmp/oukv_$$; WIRE=/tmp/oukv_$$.wire; PORT="${KV_PORT:-$((26500 + RANDOM % 300))}"
rm -rf "$ROOT" "$WIRE"*
bash "$HERE/tools/openurma_mkdev.sh" openurma0 fe80::1 "$ROOT" >/dev/null
bash "$HERE/tools/openurma_mkdev.sh" openurma1 fe80::2 "$ROOT" >/dev/null
E=( OPENURMA_FAKE_ROOT="$ROOT" LD_LIBRARY_PATH="$OU_LIBURMA:$OU_LIBCOMMON"
    LD_PRELOAD="$OU_SHIM" OPENURMA_WIRE_PATH="$WIRE" PP_PORT="$PORT"
    OPENURMA_BACKSTOP="${OPENURMA_BACKSTOP:-4}" KV_NREQ="${KV_NREQ:-8}" )

env "${E[@]}" PP_DEV=openurma0 OPENURMA_WIRE_ROLE=listen  timeout 60 "$KV" server >/tmp/kv_s.log 2>&1 &
SRV=$!; sleep 1
env "${E[@]}" PP_DEV=openurma1 OPENURMA_WIRE_ROLE=connect timeout 60 "$KV" client >/tmp/kv_c.log 2>&1 &
CLI=$!
wait $CLI; CR=$?; wait $SRV; SR=$?
rm -rf "$ROOT" "$WIRE"*
echo "===== KV SERVER ====="; cat /tmp/kv_s.log
echo "===== KV CLIENT ====="; cat /tmp/kv_c.log
echo "client_rc=$CR server_rc=$SR"
[[ $CR -eq 0 ]] && echo "KV_STORE: PASS" || echo "KV_STORE: FAIL"
exit $CR
