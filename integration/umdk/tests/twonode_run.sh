#!/bin/bash
# Run a REAL two-node test: TWO gem5 full-system processes (separate OLK-6.6 kernels +
# physical memory), each restoring the single-node checkpoint, their NIC peer channels
# bridged by a shared-mmap ring. node 0 = server, node 1 = client.
#   usage: twonode_run.sh <app>        (app run as "<app> server" / "<app> client")
set -u
APP="${1:-twonode_write}"
G=/home/ubuntu/gem5/build/ARM/gem5.opt
CFG=/home/ubuntu/OpenURMA/eval/twonode/gem5_scaffold/configs/single_node_fs_clean.py
KERN=/tmp/oe66/vmlinux
INITRD=/tmp/openurma_cpt.cpio.gz
CPT=/tmp/m5_cpt/cpt
RING=/tmp/ou_peer_ring
export M5_PATH=/tmp/gem5_sys

# fresh, zeroed ring (head=tail=0 each direction)
dd if=/dev/zero of=$RING bs=1M count=2 2>/dev/null
echo "/bin/$APP server" > /tmp/tn_s0
echo "/bin/$APP client" > /tmp/tn_s1
rm -rf /tmp/m5_n0 /tmp/m5_n1

echo "[twonode_run] launching node 0 (server) + node 1 (client) ..."
timeout -k 5 300 $G --outdir=/tmp/m5_n0 $CFG --kernel $KERN --initrd $INITRD \
  --script=/tmp/tn_s0 --restore-from=$CPT --peer-ring=$RING --peer-node=0 >/tmp/m5_n0_out.txt 2>&1 &
P0=$!
timeout -k 5 300 $G --outdir=/tmp/m5_n1 $CFG --kernel $KERN --initrd $INITRD \
  --script=/tmp/tn_s1 --restore-from=$CPT --peer-ring=$RING --peer-node=1 >/tmp/m5_n1_out.txt 2>&1 &
P1=$!
wait $P0; wait $P1
echo "[twonode_run] both processes done"
echo "=== node 0 (server) ==="; grep -aiE "openurma-2n|peer-rx|peer-tx ring" /tmp/m5_n0/system.terminal 2>/dev/null | tail -4
echo "=== node 1 (client) ==="; grep -aiE "openurma-2n" /tmp/m5_n1/system.terminal 2>/dev/null | tail -4
