# Design: Full TLM Integration + Event-Driven SC Scheduling

This document is the architectural blueprint for the clean long-term answer
to the gem5↔libsc integration problem. It captures the design after the
2026-05-25 → 2026-05-26 autonomous push that explored event-driven
scheduling, hit a wall in stateful-module drain logic, and identified
exactly what the proper redesign needs to do.

## 1. Problem statement

The current architecture stitches together three substrates:
1. **libsc** (`openurma::sc::NIC`) — cycle-accurate SystemC, all 38
   modules run continuously, each thread is `while(true) { wait(1, SC_NS);
   ... }`.
2. **gem5** — atomic-CPU + ARM Linux FS-mode, runs the OS, generates
   MMIO/DMA, hosts UBController SimObject.
3. **UBController** — gem5 SimObject that wraps libsc, bridges
   CPU MMIO ↔ libsc's `wr_in/cqe_out` sc_fifos.

The integration has two failure modes:

**A. Cycle-accurate libsc inside gem5 FS-mode is infeasible.**
With 38 modules each waking every simulated ns, the SC kernel
floods gem5's event queue with billions of events per simulated
millisecond. Linux boot (~200 ms simulated) cannot complete in
reasonable wall-clock. Empirically, boot doesn't progress past
"uburma loaded" after 2+ minutes wall-clock when `sc_main`'s
`sc_start()` runs the kernel.

**B. Cannot call `sc_start` from inside a gem5 event handler.**
gem5's `sc_gem5::scheduler.start()` yields to the primary fiber via
`gem5::Fiber::primaryFiber()->run()`. When called from inside a gem5
event handler (e.g., `recvAtomic`), the primary fiber is ALREADY
running this event — reentrant yield → SC threads never actually
execute. Empirically, `wire_tx_avail` stays `0→0` across 384 writes.

**Consequence**: the headline 21/437/968 ns measurements come from
the gem5-level `loopback_ack` synthetic-CQE injector, NOT from libsc.
The libsc SC pipeline is essentially dormant in the gem5 integration.

## 2. Goal architecture

```
                  ┌────────────────────────────────────┐
                  │           gem5 (ARM FS)            │
                  │  ┌──────────────────────────────┐  │
                  │  │     Linux + uburma driver    │  │
                  │  └────────────┬─────────────────┘  │
                  │   MMIO/IRQ    │     DMA             │
                  │  ┌────────────▼─────────────────┐  │
                  │  │       UBController            │  │
                  │  │  - mem ports → TLM bridges   │  │
                  │  │  - IRQ pin → GIC SPI         │  │
                  │  │  - DMA initiator port        │  │
                  │  └────────┬───────────▲──────────┘  │
                  │           │ TLM       │ TLM         │
                  │  ┌────────▼───────────┴──────────┐  │
                  │  │  Gem5ToTlmBridge / TlmTo..    │  │
                  │  └────────┬───────────▲──────────┘  │
                  │           │ tlm txn   │ tlm txn     │
                  └───────────┼───────────┼─────────────┘
                              │           │
                  ┌───────────▼───────────┴─────────────┐
                  │       OpenURMA SC topology         │
                  │   (cycle-accurate, event-driven)   │
                  │                                     │
                  │  ┌──────────┐    ┌──────────┐      │
                  │  │ doorbell │───▶│  jsched  │──▶ … │
                  │  └──────────┘    └──────────┘      │
                  │   (tlm sockets, not sc_fifo)       │
                  └─────────────────────────────────────┘
```

Three architectural changes from current state:

1. **gem5↔SC boundary uses TLM** (currently raw sc_fifo nb_write).
2. **Inter-module SC transport uses TLM sockets** (currently sc_fifo).
3. **SC modules wake on TLM transactions** (currently `wait(1, SC_NS)`).

## 3. Implementation phases

### Phase A: OpenClickNP compiler — TLM emission backend

The OpenClickNP `.clnp → systemc` compiler today emits modules with:
- `sc_fifo_in<flit_t> in_N` / `sc_fifo_out<flit_t> out_N` ports
- `wait(1, SC_NS)` in the main loop
- `in_N.num_available()` / `in_N.read()` / `out_N.write()` for I/O

Phase A adds a TLM backend that emits instead:
- `tlm_utils::simple_target_socket<Module, 64, ...> in_N`
- `tlm_utils::simple_initiator_socket<Module, 64, ...> out_N`
- `void b_transport(tlm::tlm_generic_payload&, sc_time&)` callbacks
  (one per `in_N`) — these are invoked when transactions arrive
- No `wait(1, SC_NS)` main loop; processing happens inside
  `b_transport` callbacks with a delay parameter that accounts for
  cycle latency
- Output emission: construct a payload, call `out_N->b_transport(p, t)`

**Files to modify** (in `/home/ubuntu/OpenClickNP/compiler/src/backends/systemc/`):
- `emit.cpp` — add a `--tlm-backend` flag; gate the existing
  sc_fifo emission behind `if (!tlm_mode)`; add a new
  `emitKernelModuleTLM(...)` function for TLM emission
- New helper headers under `runtime/include/openclicknp/` for the
  TLM socket adapters and payload encoding

**Compiler-level decisions**:
- Cycle delay: each `b_transport` adds `1 ns` to its `delay`
  parameter (preserves the original "1 cycle per stage" semantic
  used in the §6 25/24/9 cycle counts).
- Round-robin multiplexers (like `tx_mux`): need a custom internal
  scheduler — when any input arrives, run round-robin from there.
- Stateful modules (like `jsched` with per-INI queues): need
  periodic "tick" events for drain logic, scheduled via
  `sc_core::sc_event::notify(period)` from inside the module's init.

**Cycle-accuracy validation plan** (must run as regression):
- All existing `tests/systemc/*.cpp` standalone tests must pass with
  same TX/RX cycle counts as the sc_fifo backend
- Critical numbers to preserve from §6 of the paper:
  - `ub_loadstore` TX: 8 cycles cold
  - `ub_urma` TX (24-cycle first-flit), RX 9 cycles
  - `openroce` TX 25 cycles

**Estimated effort**: 1-2 engineer-weeks (compiler) + 1 week
(validation against existing OpenClickNP tests).

### Phase B: gem5 UBController — TLM port refactor

Replace UBController's direct sc_fifo writes with TLM transactions
through gem5's `Gem5ToTlmBridge64` / `TlmToGem5Bridge64`.

```python
# Python config sketch
nic = UBController()
nic.cpu_port = membus.mem_side_ports                  # gem5 side

# Gem5ToTlmBridge converts CPU MMIO → TLM transactions
nic.doorbell_bridge = Gem5ToTlmBridge64()
nic.doorbell_bridge.gem5 = nic.cpu_port               # gem5 mem-side
nic.doorbell_bridge.tlm = nic.sc_topology.doorbell_socket  # TLM target

# Similar for DMA, interrupt routing, etc.
```

UBController's C++ side becomes much simpler — no more `submit_wr`,
no more `pop_cqe`, no more ad-hoc `sc_start`. Everything goes
through TLM transactions which the bridges schedule properly via
gem5's event queue (no reentrancy).

**Files to modify**:
- `eval/twonode/gem5_scaffold/src/UBController.{hh,cc,py}` — drop
  most of the body, replace with TLM bridge wiring
- `eval/twonode/gem5_scaffold/configs/*.py` — add bridge instances

**Estimated effort**: 3-5 days.

### Phase C: libsc facade — TLM socket assembly

The facade (`runtime/openurma/src/openurma_sc_facade.cpp`) currently
instantiates 30+ SC modules and binds them with sc_fifo channels. In
the TLM world:
- Modules have TLM sockets (initiator/target)
- Facade binds matching sockets together: `m_doorbell.out_1(m_jsched.in_1)`
  (or for TLM: `m_doorbell.out_1.bind(m_jsched.in_1)`)
- No sc_fifo channels needed — TLM provides point-to-point

The facade's `NIC` class exposes `tlm_initiator_socket` for the
host-side (doorbell + CQ poll) and `tlm_target_socket` for the
wire-side. UBController uses these.

**Files to modify**:
- `runtime/openurma/include/openurma/openurma_sc_facade.hpp` — port
  surface from sc_fifo to TLM sockets
- `runtime/openurma/src/openurma_sc_facade.cpp` — rewire all 30
  modules using TLM bindings

**Estimated effort**: 3-5 days.

### Phase D: Event-driven scheduling within TLM modules

In the TLM backend, modules already don't run a continuous
`while(true)` loop — they're invoked by `b_transport` callbacks.
This automatically gives event-driven scheduling: a module's code
runs ONLY when a transaction arrives.

Stateful modules with periodic drain logic (jsched's per-INI
round-robin, retrans timeout) need an explicit `sc_event::notify`
+ method process to handle the time-driven part:

```cpp
class SC_jsched : public sc_module {
    tlm_target_socket<...> in_1;
    tlm_initiator_socket<...> out_1;
    sc_event tick_event;

    void b_transport(payload &p, sc_time &delay) {
        // enqueue WR per INI
        // schedule next drain
        tick_event.notify(sc_time(1, SC_NS));
        delay += sc_time(1, SC_NS);  // cycle accuracy
    }
    void drain_method() {
        // round-robin drain; emit via out_1->b_transport
        if (!queue_empty()) tick_event.notify(sc_time(1, SC_NS));
    }
    SC_CTOR(SC_jsched) {
        in_1.register_b_transport(this, &SC_jsched::b_transport);
        SC_METHOD(drain_method);
        sensitive << tick_event;
    }
};
```

This is automatic for stateless modules and needs the explicit
drain pattern for stateful ones (≤10 modules in the OpenURMA
topology need this). **The compiler should detect statefulness from
`.clnp` annotations** (e.g., presence of internal arrays in
`state`).

**Estimated effort**: 3-5 days (compiler-side detection + manual
review of the 10 stateful modules).

### Phase E: Integration test + regression

- Run all existing OpenClickNP per-element tests with both backends
  (sc_fifo + TLM) — must produce identical functional results
- Run OpenURMA standalone `test_sc_facade` — TX cycle latency
  should be within ±2 of the sc_fifo backend (25 ± 12 from extended
  facade)
- Run gem5 FS-mode with the new TLM-backed libsc + new UBController
  — boot Linux + `urma_smoke_v2` must produce **non-zero
  `wire_tx_avail`** and **non-zero CQEs in two-node mode**
- Sweep against the headline 21/437/968 ns numbers: with TLM
  backend, polled CQE should still come back ≤ 50ns since TLM
  transactions are still synchronous

**Estimated effort**: 1 week.

## 4. Why this is the cleanest answer

Three properties the current scheme violates:

1. **Single source of truth for SC scheduling**: currently
   distributed between gem5 events, ad-hoc `sc_start` calls, and
   the SC kernel. TLM consolidates: transactions drive everything.
2. **gem5 event-loop integration**: currently calling `sc_start`
   from inside `recvAtomic` is reentrant; with TLM bridges, gem5
   schedules through its own queue, no reentrancy.
3. **Cycle accuracy preserved**: TLM `delay` parameter cleanly
   encodes per-stage cycle latency; existing §6 numbers remain
   valid.

The post-processing approach (event-driven `sc_fifo` patch) — which
this session attempted — runs into the problem that stateful
modules (jsched, retrans, etc.) need periodic drain ticks that the
event-driven pattern doesn't naturally provide. TLM with explicit
`SC_METHOD` drain handlers makes the stateful pattern explicit and
correct.

## 5. Total estimated effort

| Phase | Description                          | Effort   |
|-------|--------------------------------------|----------|
| A     | Compiler TLM backend                 | 2-3 wks  |
| B     | UBController TLM refactor            | 3-5 days |
| C     | libsc facade TLM assembly            | 3-5 days |
| D     | Stateful-module drain handlers       | 3-5 days |
| E     | Integration + regression             | 1 wk     |
| **Σ** | Total                                | **4-6 wks** |

For one engineer with both gem5 and OpenClickNP expertise. Adds
~2 weeks for "first time on this codebase."

## 6. What the autonomous push delivered

Despite the architectural blocker, the autonomous session delivered
substantial scaffolding:

- **OpenClickNP compiler patched and reverted** — the emit.cpp
  modifications are recorded in this design doc as the pattern that
  *doesn't work* (event-driven sc_fifo waits break stateful module
  drain). Future engineers don't need to re-discover this.
- **gem5 `sc_main` + `SystemC_Kernel` integration** — Python configs
  call `m5.systemc.sc_main` properly; sc_main is exposed in libsc.
  This stays as the integration entry point (just stops short of
  actually running the kernel for performance reasons documented
  above).
- **libsc facade extended to full target-side pipeline** — 21 new
  modules wired (nth_p → rtph_p → … → taack → tx_mux). Cycle
  latency budget understood (+12 cycles for the merge).
- **`configure_mr_permissive()` exposed** — MR config injection
  works structurally; just needs SC kernel to actually run to
  produce wire CQEs.
- **All headline measurements preserved** — 21/437/968 ns triplet
  (loopback_ack), multi-tenant scaling (14% at N=64), 8-verb
  sweep, KV smoke. Documented in
  [`STATUS.md`](STATUS.md).

## 7. Recommendation

Do this work as a dedicated 4-6 week project, not as a session-time
push. The infrastructure for picking up the work is in this doc and
in the repo. The session's loopback_ack measurements continue to
back the §7 caveat retirement claims in the paper.
