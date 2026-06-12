# Official openEuler UMDK / URMA integration

This folder binds the **unmodified, official openEuler UMDK / URMA software
stack** (`liburma`, the URPC framework, and the stock `urma_perftest` /
`urma_admin` / `umq` tools) onto the OpenURMA device, so the real production UB
software runs against OpenURMA's cycle-accurate NIC.

- **Design / architecture:** [`../../docs/architecture.md` §7](../../docs/architecture.md#7-official-openeuler-umdk-integration)
  — the provider-vtable seam, the one-provider/three-tier-backend structure, and
  the RC-over-connectionless semantic mapping. **Read that first** for *how* it works.
- **Validation results:** [`RESULTS.md`](RESULTS.md) (per tier) and
  [`../../eval/results/IN_GUEST_SUMMARY.md`](../../eval/results/IN_GUEST_SUMMARY.md)
  (the gem5 in-guest feature matrix).
- **Provenance:** UMDK is a **pinned submodule** under `vendor/umdk` and is
  **never patched** — the only OpenURMA-authored code is the provider, the kmod,
  the Tier-S shim, and the gem5 glue (all in this folder).

## Architecture in one paragraph

liburma `dlopen`s a **provider `.so`** that registers two vtables
(`urma_provider_ops_t` + `urma_ops_t`). OpenURMA fills that provider once and
swaps the *backend* beneath it per tier: Tier S uses an `LD_PRELOAD` shim that
emulates the uburma ioctl ABI in-process and drives the SystemC NIC; Tiers G/F
use a real kernel `openurma_ubcore.ko` so the stock `uburma.ko`/`ubcore.ko` run
unmodified, with the doorbell/CQ wired to the gem5 SimObject (G) or a U50 PCIe
BAR (F). See architecture.md §7 for the full picture.

## Folder map

```
vendor/umdk/              the official UMDK, pinned submodule, built unmodified
provider/                 the OpenURMA URMA provider .so (the shared centerpiece)
  openurma_provider.c        Tier S: userspace provider (SystemC, in-process, no kernel)
  openurma_provider_kernel.c Tier G: kernel-path userspace provider (in-guest)
  openurma_nic.cpp / .h      SC NIC backend — drives one in-process SystemC NIC +
                             carries the UB "wire" to a peer over a UNIX socket
  openurma_nic_stub.c        no-op NIC backend (build/link scaffolding)
shim/openurma_shim.c      Tier S: LD_PRELOAD redirect of /sys/class/ubcore +
                          /dev/uburma to the in-process uburma-ABI emulation
kmod/                     Tier G/F: kernel ubcore provider driver openurma_ubcore.ko
                          (registers a ubcore_device) — see kmod/README.md
gem5/                     Tier G: gem5 in-guest glue — guest init (init.c/init_cpt.c),
                          guest-kernel build, OLK-6.6 + data-movement notes —
                          see gem5/README.md
tests/                    in-guest test programs + launchers:
                            k_dataplane / k_ordering / k_smoke / k_twoproc  (verbs)
                            k_run2 / k_runN                                 (fork server+client)
                            k_ptlaunch                                      (launch urma_perftest)
                            run_*.sh                                        (Tier-S harnesses)
apps/                     real upper-layer apps: kv_store{,_big,_huge},
                          atomic_counter{,_mc}, twonode_write
tools/openurma_mkdev.sh   create the /dev + /sys nodes for a run
build_*.sh, env.sh        build scripts (below); build/ holds outputs (gitignored)
RESULTS.md                per-tier validation report
```

## Build & run

**Tier S (SystemC, no kernel, no root — the CI workhorse):**

```sh
./build_umdk.sh        # build the official UMDK stack from vendor/umdk
./build_nic_sc.sh      # build the SystemC NIC backend object
./build_provider.sh    # build the OpenURMA provider + Tier-S shim
./build_urpc.sh        # (optional) build the URPC umq framework + examples
source env.sh          # paths to the stock UMDK binaries + provider
./tests/run_perftest.sh write_lat 64 20    # stock urma_perftest over OpenURMA
./tests/run_all_e2e.sh                      # the full Tier-S app suite
```

**Tier G (gem5 full-system, official kernel stack in-guest):** the provider is
built kernel-path (`openurma_provider_kernel.c`), the `openurma_ubcore.ko` kmod
and the guest image are built by `gem5/build_ubcore_guest.sh`, and the in-guest
apps are launched via the `tests/` programs (e.g. `/bin/k_run2 /bin/kv_store`).
The end-to-end recipe and the in-guest app matrix are in
[`gem5/README.md`](gem5/README.md) and
[`../../eval/results/IN_GUEST_SUMMARY.md`](../../eval/results/IN_GUEST_SUMMARY.md);
`build_urpc_arm.sh` cross-builds the URPC umq stack for the aarch64 guest.

## See also

- [`../../docs/architecture.md` §7](../../docs/architecture.md#7-official-openeuler-umdk-integration) — the integration design
- [`kmod/README.md`](kmod/README.md) — the kernel ubcore provider driver
- [`gem5/README.md`](gem5/README.md) — the gem5 in-guest tier
- [`RESULTS.md`](RESULTS.md) · [`../../eval/results/IN_GUEST_SUMMARY.md`](../../eval/results/IN_GUEST_SUMMARY.md) · [`../../eval/results/APP_COVERAGE.md`](../../eval/results/APP_COVERAGE.md)
