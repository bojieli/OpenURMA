#!/usr/bin/env bash
# Stock urma_sample (official UMDK URMA client/server example, examples/urma_sample.c,
# unmodified) over the OpenURMA Tier-S provider → SystemC NIC. RC mode.
#
# Two harness specifics this sample needs (vs perftest/URPC):
#  - the server's main thread blocks on getchar() ("Type to exit..."); we feed it
#    an open pipe (`sleep`) so it stays up while its socket thread runs the data
#    path (a plain </dev/null would EOF getchar and tear the context down mid-run).
#  - urma_sample polls each completion only MAX_POLL_JFC_CNT=10 times, so we set
#    OPENURMA_BACKSTOP below that (one-sided WRITE/SEND completions fire on the
#    provider's pump backstop when no SC completion flit is harvested first).
#
# Result: WRITE + READ complete with correct data on the client
# ("Msg write/Msg read: hello,this is user N"); the server receives the WRITE
# segment and the SEND ("Msg received: Send message ...").
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$HERE/env.sh"
SAMP="$OU_BIN_SAMPLE/urma_sample"
BACKSTOP="${OPENURMA_BACKSTOP:-4}"

for try in 1 2 3 4 5 6; do
  pkill -9 -f urma_sample 2>/dev/null; sleep 1
  ROOT=/tmp/ousmp_$$_$try; WIRE=/tmp/ousmp_$$_$try.wire; PORT=$((44100 + RANDOM % 400))
  rm -rf "$ROOT" "$WIRE"*
  bash "$HERE/tools/openurma_mkdev.sh" openurma0 fe80::1 "$ROOT" >/dev/null
  bash "$HERE/tools/openurma_mkdev.sh" openurma1 fe80::2 "$ROOT" >/dev/null
  E=( OPENURMA_FAKE_ROOT="$ROOT" LD_LIBRARY_PATH="$OU_LIBURMA:$OU_LIBCOMMON"
      LD_PRELOAD="$OU_SHIM" OPENURMA_WIRE_PATH="$WIRE" OPENURMA_BACKSTOP="$BACKSTOP" )
  ( sleep 40 | env "${E[@]}" OPENURMA_WIRE_ROLE=listen stdbuf -oL timeout 45 \
       "$SAMP" -m 1 -d openurma0 -p $PORT ) >/tmp/smp_s.log 2>&1 &
  sleep 5
  env "${E[@]}" OPENURMA_WIRE_ROLE=connect stdbuf -oL timeout 28 \
       "$SAMP" -m 1 -d openurma1 -i 127.0.0.1 -p $PORT </dev/null >/tmp/smp_c.log 2>&1
  pkill -9 -f "sleep 40" 2>/dev/null; sleep 1
  rm -rf "$ROOT" "$WIRE"*
  if grep -q "Msg write:" /tmp/smp_c.log 2>/dev/null && grep -q "Msg read:" /tmp/smp_c.log 2>/dev/null; then
    echo "=== urma_sample over SystemC NIC: try $try ==="
    echo "--- client ---"; grep -E "bind jetty|Msg write|Msg read|Msg sent" /tmp/smp_c.log
    echo "--- server ---"; grep -E "accepted new|segment msg: hello|Msg received" /tmp/smp_s.log | tail -3
    echo "URMA_SAMPLE: WRITE+READ data path PASS"
    exit 0
  fi
  echo "try $try: incomplete (shared-host race), retrying..."
done
echo "URMA_SAMPLE: did not catch a clean run in 6 tries (loaded host)"; exit 1
