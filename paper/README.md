# OpenURMA — arXiv tech report sources

LaTeX sources for the tech report. Build with:

```sh
make
```

This runs `pdflatex` + `bibtex` + 2x `pdflatex` (standard LaTeX
build dance) and produces `main.pdf`.

The paper covers:
- The QP-scaling problem and UB's two architectural pillars
  (transaction/transport split, graded ordering)
- OpenURMA's 35-element implementation on OpenClickNP
- The OpenRoCE RoCEv2 RC baseline (in-tree, 21 elements)
- Per-connection state, cycle-accurate latency / throughput,
  Vivado P&R area — side-by-side at 1024×1024 endpoints
- Limitations (no silicon yet, out-of-context P&R, no kernel-module
  driver)

Numbers in the paper come directly from the eval scripts in the
parent repo:
- `scripts/run_all_tests.sh` → SW-emu correctness
- `scripts/build_systemc.sh tests/systemc/test_sc_latency.cpp` → SystemC
- `scripts/synth_hls.sh` + `scripts/vivado_all.sh` → HLS + P&R sweep

The PDF target is arXiv (self-contained, two-column, CC-BY-4.0).
