# gem5 sc_gem5 scheduler patch

## gem5_sc_deschedule.patch

Target: `gem5 v24.0.0.1`, file `src/systemc/core/scheduler.hh`.

### Symptom

When OpenURMA's `NICTopologyRoCE` SimObject forwards the first 64-byte
doorbell into the OpenRoCE TLM cascade
(`doorbell → qptx → bthb → dcqcn → retrans → txmux → ethenc`), gem5
aborts inside `SC_ethenc_TLM::b_transport_in_1` with:

```
panic: Descheduling event at time with no events.
  at src/systemc/core/scheduler.hh:299
```

Backtrace:

```
AtomicSimpleCPU → CoherentXBar → Gem5ToTlmBridge<512>
  → NICTopologyRoCE::mmio_b
  → SC_doorbell_TLM → SC_qptx_TLM → SC_bthb_TLM → SC_dcqcn_TLM
  → SC_retrans_TLM → SC_txmux_TLM → SC_ethenc_TLM::b_transport_in_1
  → sc_core::sc_event::notify(sc_time)
  → sc_gem5::Event::notify(const sc_core::sc_time &t)
  → sc_gem5::Scheduler::deschedule(ScEvent *)    ← PANIC
```

The OpenURMA-side cascade does not trip it because the chain is shorter
and the recycled `TimeSlot` happens not to be reused at exactly the
wrong moment; the OpenRoCE chain reuses it on every WR.

### Root cause

`Scheduler::deschedule(ScEvent *event)` looks up the time slot that
owns the event by matching `event->when()` against
`(*tsit)->targeted_when`. That match is fragile:

* `acquireTimeSlot(tick)` reuses an entry from `freeTimeSlots`, resets
  `targeted_when`, and clears the `events` list — but the storage of
  the `events` list itself is the same memory.
* If an event was scheduled into a slot, the slot completed and was
  released, and then a different code path reused the slot for a new
  `targeted_when`, an event still pointing at `&slot.events` via its
  `_events` member is now adrift of its cached `_when`.
* In that state the `Event::notify(const sc_core::sc_time &t)` path
  sees `delayedNotify.scheduled() == true` (because `_events` is
  non-null) and calls `Scheduler::deschedule(...)`, which walks
  `timeSlots` looking for a slot at the stale `_when` value, finds
  none, and panics.

The authoritative source of truth is `event->scheduledOn()` — the same
pointer the scheduler uses to push the event onto `slot.events`.

### Fix

Walk `timeSlots` by pointer identity against `event->scheduledOn()`
first, fall back to the legacy `when()`-based lookup for the
common-case fast path, and if both fail, gracefully fall back to a
direct `event->deschedule()` (which uses the event's own iterator and
list pointer, both of which remain valid).

After the patch, the OpenRoCE pipeline runs end-to-end in gem5 FS mode
and the dual-NIC config produces apples-to-apples per-WR latency
numbers for the §7 UB-vs-RoCE comparison.

### Reproducer

A standalone reproducer in OpenURMA's tree:

```
M5_PATH=/tmp/gem5_sys /home/ubuntu/gem5/build/ARM/gem5.opt \
  --outdir=/tmp/m5_dual_nic_patched \
  eval/twonode/gem5_scaffold/configs/single_node_fs_dual_nic.py \
  --kernel=vmlinux \
  --initrd=eval/twonode/gem5_scaffold/driver/initramfs.cpio.gz \
  --mem-size=2GB --link-delay-ns=0
```

Without the patch: panic on the first RoCE doorbell.

With the patch (plus the ODR namespacing and RoCE codec fixes, see
`ROCE_FS_FINDINGS.md`): both NICs produce `CSV,…` lines and full
completions, kernel halts cleanly. Captured run output is at
`eval/twonode/gem5_scaffold/results/exp11_dual_nic_roce_fixed.txt`.
Headline rows from that run (the OpenRoCE NIC now runs its *own*
22-module pipeline, not OpenURMA's — the earlier identical-looking
rows were the ODR-merge artifact):

```
CSV,ROCE,16,a,16,16,19,40    ← OpenRoCE pipeline, 16/16 hits
CSV,ROCE,PL4096,a,16,16,22,40 ← OpenRoCE pipeline, 4 KB payload, 16/16 hits
CSV,16,a,16,16,17,40         ← OpenURMA  pipeline, 16/16 hits
CSV,PL4096,a,16,16,17,40     ← OpenURMA  pipeline, 4 KB payload, 16/16 hits
```

Both NICs' per-WR means coincide at this AtomicCPU instruction floor:
this dual-NIC config does not enable Tier-2 SC-delay propagation, so
the CPU sees only its own memory-access timing, not the SC-pipeline
internal latency. The quantitative UB-vs-RoCE comparison therefore
still comes from the standalone two-node SystemC simulator
(`eval/twonode/`), the source of the headline 4.37× number. The
gem5+patch contribution is to prove *both* the OpenURMA and the
matched OpenRoCE 22-module pipelines are functional end-to-end —
producing real CQEs across N- and payload-sweeps — inside the
OS-aware simulation environment. (Tier-2-propagated, cycle-accurate
per-WR latency for the OpenURMA NIC is reported separately under
`single_node_fs_clean.py` / the TimingCPU-switch config.)

### Applying

```
cd /home/ubuntu/gem5
git apply /home/ubuntu/OpenURMA/eval/twonode/gem5_scaffold/patches/gem5_sc_deschedule.patch
scons build/ARM/gem5.opt USE_SYSTEMC=1 -j$(nproc)
```

### Upstreaming

A minimal reproducer (no OpenURMA dependency) can be built from
`sc_event::notify(t)` cascaded across seven `SC_MODULE`s in a
`b_transport` chain — the panic reproduces deterministically under
gem5 v24.0.0.1 with `USE_SYSTEMC=1`. Worth filing upstream.
