# Two-node SystemC evaluation

This directory holds the cycle-level **two-node** simulator that wires two
NICs across a link and measures the end-to-end path UB claims to shorten,
each stack against a matched OpenRoCE baseline. It is the source of every
`paper/figures/twonode_*.pdf`.

## The simulator

`build/twonode_sim` (built by `build_twonode.sh`) models both endpoints of
the wire. Key knobs:

```
--stack    ub_loadstore | ub_urma | roce_bf | roce_dma | infiniswap | fastswap
--workload ptr_chase | bulk_read | bulk_write | send_recv | hash_probe |
           cas_lock | dist_barrier | graph_bfs | seq_scan | zipf_read
--verb     load | store | read | write | send | recv | faa | cas | swap
--n-ops N --link-delay-ns L --concurrency C --payload-bytes B
--jitter-factor F        # link-delay jitter (per-op latency dump)
--out-csv FILE           # append one summary row
--dump-latencies FILE    # write one raw per-op latency per line
```

All RoCE per-op timing defaults (PCIe MMIO/DMA, PSN serialization, etc.)
live in `include/twonode/components.hpp` and are documented inline with the
published ConnectX-7-class sources they track.

## Reproducing everything

```sh
# build the three sims, run every experiment, regenerate every figure
bash reproduce_figures.sh

# assert the paper's headline numbers (500 ns / 2236 ns / 4.47x / 4855x state)
bash verify_claims.sh
```

`reproduce_figures.sh` is the orchestrator: it builds `twonode_sim`,
`multinode_sim`, and `conn_setup_sim`, runs every `run_*.sh` and `sim_*.py`,
then every `plot_*.py`. Each step is independent; failures are reported but
do not abort the run.

## Experiment → data → figure map

| Driver | CSV (`results/`) | Plot | Figure(s) | Paper § |
|--------|------------------|------|-----------|---------|
| `run_sweep.sh` | `sweep.csv` | `plot_figs.py` | latency breakdown, e2e CDF, oprate-vs-conc, link-delay, payload scaling, verb coverage, cache policy | §lat |
| `run_sweep.sh` | `sweep.csv` | `plot_verb_asymmetry.py` | `twonode_verb_asymmetry` | §lat |
| `run_lat_tput.sh` | `lat_tput.csv` | `plot_lat_tput.py` | `twonode_lat_tput_envelope` | §lat |
| `run_sram_spill.sh` | `sram_spill.csv` | `plot_sram_spill.py` | `twonode_sram_spill` | §state |
| `run_mixed_order.sh` | `mixed_order.csv` | `plot_mixed_order.py` | `twonode_mixed_order` | §ordering |
| `run_rol_compare.sh` | `rol_compare.csv` | `plot_rol_compare.py` | `twonode_rol_fused_ack` | §ordering |
| `run_cas_contention.sh` | `cas_contention.csv` | `plot_cas_contention.py` | `twonode_cas_contention` | §contention |
| `run_tp_sharing.sh` | `tp_sharing.csv` | `plot_tp_sharing.py` | `twonode_tp_sharing` | §state |
| `run_m2n.sh` | `m2n.csv` | `plot_m2n.py` | `twonode_m2n` | §fanout |
| `run_loss_recovery.sh` | `loss_recovery.csv` | `plot_loss_recovery.py` | `twonode_loss_goodput` | §transport |
| `run_infiniswap_compare.sh` | `infiniswap.csv` | `plot_infiniswap.py` | `twonode_infiniswap` | §pageswap |
| `run_ycsb.sh` | `ycsb.csv` | `plot_ycsb.py` | `twonode_ycsb` | §kvstore |
| `run_c1_validation.sh` | `c1_validation.csv` | `plot_validation.py` | `twonode_validation` | §c1-validation |
| `run_jitter.sh` | `jitter/*.txt` | `plot_jitter_cdf.py` | `twonode_jitter_cdf` | §transport |
| `run_multinode.sh` | `multinode_sc.csv` | `plot_multinode.py` | `twonode_multinode_scaling` | §scaleout |
| `build_conn_setup.sh` + `sim_conn_setup.py` | `conn_setup.csv` | `plot_conn_setup.py` | `twonode_conn_setup` | §setup |
| `sim_cong_control.py` | `caqm_trace.csv` (self) | `plot_cong_control.py` | `twonode_cong_control` | §congestion |

Self-generating plots (no driver — the plot writes its own CSV from an
inline analytical model): `plot_directory_cliff.py`,
`plot_invalidation_broadcast.py`, and the `*_gem5.py` CHI variants, which
read the gem5 CHI experiment under `gem5_scaffold/chi_experiment/`.

## Determinism

All randomized experiments (Poisson arrivals in `lat_tput`, link jitter in
`run_jitter.sh`) seed their RNG, so repeated runs are bit-identical.
Note that matplotlib embeds a timestamp in every PDF, so re-rendering a
figure changes its bytes even when the underlying data is unchanged.
