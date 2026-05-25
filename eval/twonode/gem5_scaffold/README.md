# gem5 Integration Scaffolding for OpenURMA

This directory holds the future-work substrate for full-system gem5
co-simulation of OpenURMA. The SystemC two-node simulator in
`eval/twonode/` is the source of the latency numbers in the paper; the
gem5 scaffolding here is the next step that adds a real CPU model, real
caches, and a real OS on top of the same `libopenurma_sc.a`.

## Status

- gem5 v24.0.0.1 builds locally with `USE_SYSTEMC=1` and passes
  SE-mode hello-world (verified May 25 2026 against a static ARMv8
  binary compiled with `aarch64-linux-gnu-gcc -static`).
- `libopenurma_sc.a`, `libopenurma_ls_sc.a`, `libopenroce_sc.a` are
  consumable as static libraries.
- The SystemC TLM 2.0 bridge in `gem5/src/systemc/` is the right
  primitive; this directory contains the stub `UBController` SimObject
  that wraps it.

## Files

- `src/UBController.{hh,cc,py}` — minimal gem5 SimObject scaffold:
  one slave port on the membus, one EtherInt for the wire link,
  configurable membus latency, hosts a `openurma::sc::NIC` instance
  through gem5's SystemC TLM bridge.
- `configs/dual_node.py` — gem5 config that brings up two
  `ArmO3CPU` + L1I/D + L2 + DRAM + `UBController`, linked via
  `EtherLink`.

## Building

```sh
# 1. build gem5 with SystemC
cd /home/ubuntu/gem5
scons build/ARM/gem5.opt USE_SYSTEMC=1 -j$(nproc)

# 2. build OpenURMA SystemC libraries
cd /home/ubuntu/OpenURMA
bash scripts/build_libsc.sh                         # libopenurma_sc.a
OPENURMA_VARIANT=openurma_ls bash scripts/build_libsc.sh  # libopenurma_ls_sc.a
OPENURMA_VARIANT=openroce    bash scripts/build_libsc.sh  # libopenroce_sc.a

# 3. add the UBController scaffold to gem5
cp -r /home/ubuntu/OpenURMA/eval/twonode/gem5_scaffold/src/* \
       /home/ubuntu/gem5/src/dev/openurma/
# (rebuild gem5 with the new SimObject)
cd /home/ubuntu/gem5 && scons build/ARM/gem5.opt USE_SYSTEMC=1 -j$(nproc)

# 4. run
/home/ubuntu/gem5/build/ARM/gem5.opt \
    /home/ubuntu/OpenURMA/eval/twonode/gem5_scaffold/configs/dual_node.py \
    --nic-stack ub_loadstore --workload ptr_chase
```

## Why a SystemC simulator instead of gem5 for the paper numbers

The paper's headline latency numbers come from the SystemC two-node
simulator (`eval/twonode/`), not gem5. Reasons:

1. **NIC cycle counts are the source of truth.** The 8 / 25 / 9 cycle
   counts for the three stacks are *measured*, not modeled — they come
   from the same SystemC microbenches reported in §6 of the paper.
   Both gem5 and the SystemC simulator would use those cycle counts as
   inputs; running gem5 on top adds CPU-microarchitecture modelling
   (cache hits/misses, pipeline stalls) but does not change the
   controller-side latency budget that is the architectural question
   the paper asks.

2. **Honest claim scope.** The paper's claim is *controller-to-controller*
   latency, not *application-to-application* latency. The two-node
   SystemC simulator delivers exactly that. Adding gem5 would let us
   make a stronger claim (application-level latency including CPU
   stalls) but only by introducing CPU model parameters that are
   themselves modeled, not measured. We chose to bound the claim
   honestly rather than maximize the number of cited microseconds.

3. **Reproducibility.** The SystemC simulator builds in 30 seconds and
   runs the 576-config sweep in under 10 minutes on a laptop. gem5 SE
   mode adds 10–20× per-run cost. For artifact evaluation by reviewers
   the SystemC path is the better choice; gem5 remains the substrate
   for follow-on work that wants real CPU + OS in the loop.
