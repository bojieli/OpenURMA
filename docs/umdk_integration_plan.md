# Binding the official openEuler UMDK / URMA stack to OpenURMA

> **This was an implementation plan; the work is done and the design has moved.**
>
> The integration **architecture** now lives in
> [`architecture.md` §7 "Official openEuler UMDK integration"](architecture.md#7-official-openeuler-umdk-integration)
> — the provider seam, the one-provider/three-tier-backend structure, and the
> RC-over-connectionless semantic mapping, reflecting what was actually built
> (an `LD_PRELOAD` shim for Tier S rather than the originally-planned CUSE
> daemon; the SC CQE/data path closed; the gem5 in-guest pipeline-data mode).
>
> The **current results / status** live in:
> - [`../integration/umdk/RESULTS.md`](../integration/umdk/RESULTS.md) — per-tier validation report
> - [`../eval/results/IN_GUEST_SUMMARY.md`](../eval/results/IN_GUEST_SUMMARY.md) — gem5 in-guest feature matrix
> - [`../eval/results/APP_COVERAGE.md`](../eval/results/APP_COVERAGE.md) — official-app × tier coverage

## What was achieved (vs the original acceptance criteria)

The plan's definition of done — (1) discovery via stock `urma_admin`, (2) a
write+read+send smoke round trip, (3) the canonical `urma_perftest` matrix in
RC, (4) the real URPC `umq` echo, (5) provenance (the vendored UMDK submodule is
pinned and **never patched** — only new provider/driver files are added), (6)
across all three tiers — is met. See the reports above for the evidence.

Provenance guard still holds: UMDK is a pinned submodule under
`integration/umdk/vendor/umdk`; the build recompiles it from source; no UMDK
file is patched. The only OpenURMA-authored code is the provider `.so`, the
`openurma_ubcore.ko` kmod, the Tier-S `LD_PRELOAD` shim, and the gem5 glue.

*(The original 11-section plan — milestones, CI gates, risk table, first steps —
is preserved in git history if needed; this file is kept as a stable redirect
because other docs link to it.)*
