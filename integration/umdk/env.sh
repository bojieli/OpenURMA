#!/usr/bin/env bash
# Source this to get the Tier-S runtime environment: paths to the stock UMDK
# libraries, the LD_PRELOAD shim, and helpers to materialize a fake device and
# run a stock UMDK binary against the in-process OpenURMA provider.
#
#   source integration/umdk/env.sh
#   ou_mkdev openurma0 fe80::1 /tmp/rootA
#   ou_run /tmp/rootA -- ./build/tier_s/list_devices openurma0
#
HERE_ENV="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export OU_UMDK_BUILD="${OU_UMDK_BUILD:-$HERE_ENV/build/umdk}"
export OU_TIER_S="$HERE_ENV/build/tier_s"
export OU_LIBURMA="$OU_UMDK_BUILD/urma/lib/urma/core"
export OU_LIBCOMMON="$OU_UMDK_BUILD/urma/common"
export OU_SHIM="$OU_TIER_S/openurma_shim.so"
export OU_BIN_URMA="$OU_UMDK_BUILD/urma/tools"          # urma_perftest, urma_admin
export OU_BIN_SAMPLE="$OU_UMDK_BUILD/urma/examples"     # urma_sample

# Build a fake device tree (sysfs + /dev) under a fake root.
ou_mkdev() {  # name eid root
    bash "$HERE_ENV/tools/openurma_mkdev.sh" "$1" "$2" "$3"
}

# Run a command with the shim + correct library path against a fake root.
# Usage: ou_run <fake_root> -- <cmd...>   (extra OPENURMA_WIRE_* may be preset)
ou_run() {
    local root="$1"; shift
    [[ "$1" == "--" ]] && shift
    OPENURMA_FAKE_ROOT="$root" \
    LD_LIBRARY_PATH="$OU_LIBURMA:$OU_LIBCOMMON:${LD_LIBRARY_PATH:-}" \
    LD_PRELOAD="$OU_SHIM${LD_PRELOAD:+:$LD_PRELOAD}" \
    "$@"
}
