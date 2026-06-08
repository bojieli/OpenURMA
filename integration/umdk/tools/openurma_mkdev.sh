#!/usr/bin/env bash
# Populate a fake ubcore sysfs tree + /dev node for one OpenURMA Tier-S device,
# under $OPENURMA_FAKE_ROOT. The openurma_shim LD_PRELOAD redirects liburma's
# accesses of /sys/class/ubcore and /dev/uburma into this tree, so stock liburma
# discovers and opens the device with no kernel module loaded.
#
# Usage: openurma_mkdev.sh <dev_name> <eid_str> [fake_root]
#   e.g. openurma_mkdev.sh openurma0 fe80::1 /tmp/openurma_rootA
#
# The capability numbers are deliberately generous so liburma's client-side
# sanity checks (depths, sizes, transport modes) accept whatever urma_perftest
# / urma_sample / URPC request. The actual semantics live in the in-process
# provider, not in these files.
set -euo pipefail

DEV="${1:?dev name}"
EID="${2:?eid string}"
ROOT="${3:-${OPENURMA_FAKE_ROOT:?set OPENURMA_FAKE_ROOT or pass root}}"

SYS="$ROOT/sys/class/ubcore/$DEV"
DEVN="$ROOT/dev/uburma"

rm -rf "$SYS"
mkdir -p "$SYS/eids" "$SYS/port0" "$DEVN"

put() { printf '%s\n' "$2" > "$SYS/$1"; }

# identity / matching
put ubdev          "$DEV"
put driver_name    "openurma"      # MUST equal provider ops->name (urma_match_device)
put transport_type "0"             # URMA_TRANSPORT_UB
put guid           "$EID"

# capabilities (generous)
put feature                 "0"
put max_jfc                 "1048576"
put max_jfs                 "1048576"
put max_jfr                 "1048576"
put max_jetty               "1048576"
put max_jetty_grp           "65536"
put max_jetty_in_jetty_grp  "64"
put max_jfc_depth           "65536"
put max_jfs_depth           "65536"
put max_jfr_depth           "65536"
put max_jfs_inline_size     "912"
put max_jfs_sge             "8"
put max_jfs_rsge            "8"
put max_jfr_sge             "8"
put max_msg_size            "2147483648"
put max_read_size           "1073741824"
put max_write_size          "1073741824"
put max_cas_size            "8"
put max_swap_size           "8"
put max_fetch_and_add_size  "8"
put max_fetch_and_sub_size  "8"
put max_fetch_and_and_size  "8"
put max_fetch_and_or_size   "8"
put max_fetch_and_xor_size  "8"
put atomic_feat             "255"
put trans_mode              "7"     # RM|RC|UM = 1|2|4
put sub_trans_mode_cap      "0"
put congestion_ctrl_alg     "0"
put ceq_cnt                 "4"
put max_tp_in_tpg           "1"
put port_count              "1"
put max_eid_cnt             "1"
put page_size_cap           "4096"
put max_oor_cnt             "0"
put mn                      "1"
put max_netaddr_cnt         "1"
put reserved_jetty_id       "0-0"

# port0 attributes
pput() { printf '%s\n' "$2" > "$SYS/port0/$1"; }
pput max_mtu       "5"   # URMA_MTU_4096
pput state         "5"   # PORT_ACTIVE
pput active_width  "1"
pput active_speed  "32"
pput active_mtu    "5"

# eid table
printf '%s\n' "$EID" > "$SYS/eids/eid0"

# character device node — only needs to be openable; the in-process provider
# never ioctls or mmaps it (control + data plane run via the SystemC facade).
: > "$DEVN/$DEV"

echo "openurma device '$DEV' (eid $EID) created under $ROOT"
