# Building the native base

saltOS builds its own minimal base from `recipes/` (the native plane) rather than
seeding it from another distribution. The big userland comes from a stratum chosen
at install time; the base — toolchain, core userland, kernel, bootloader, and the
installer tools — is self-built and reused.

## Pipeline

1. `os/bootstrap/bootstrap.sh` builds the recipe graph in topological order
   (`os/bootstrap/build-order.lua`, read with `salt eval`) via `salt build`, installing each resulting
   `.grain` into a sysroot with `salt install --root`. Stages: `cross-toolchain`,
   `temp-tools`, `base`, `desktop`. Select stages with the `STAGES` env, e.g.
   `STAGES="cross-toolchain temp-tools base"` for the installer base (no desktop).
2. `os/bootstrap/stage-rootfs.sh` assembles a bootable rootfs from the built
   grains (also honors `STAGES`).
3. `os/iso/build-iso.sh --installer salt-setup` produces the ISO, wiring the
   native `salt-setup` installer to autostart on the console.

Grains are content-addressed and pinned by recipe (`source.sha256` +
reproducibility status), so once a package builds it is deterministic and reused;
only changed recipes rebuild.

## CI (fully automated, free runners)

The `native-base` workflow builds the base on free GitHub runners with no manual
priming. A single push runs it end to end, split into sequential jobs so each
job stays within one runner's time/disk budget:

1. `toolchain` — builds the `cross-toolchain` + `temp-tools` stages.
2. `base` — builds the `base` stage.
3. `iso` — assembles the rootfs and the native ISO (`salt-setup` installer) and
   boot-tests it in QEMU, gating on the `SALTOS_BOOT_OK` marker.

Each job hands its accumulated grain + sysroot output forward to the next via an
uploaded artifact, so the pipeline populates itself within the run. Both build
jobs also restore and save an `actions/cache` of the grain directories (never the
sysroot), per run with a prefix restore. `bootstrap.sh` writes a `.stamp` next to
every grain (a hash of the recipe directory and of the builder sources) and
reuses a grain only while its stamp matches, so a restored cache can never hand
back a package built from an older recipe, and unchanged packages are not rebuilt.

## Desktop stage

The `[desktop]` stage (Xorg, Mesa, Qt 6, KF6 bits, LXQt 2.1, SDDM, ~100 recipes)
runs in `native-desktop`, triggered by every green `native-base` run on `main`
(or by hand with a `base_run_id`). It is far longer than one 6 h job, so it runs
as up to five chained slices (`native-desktop-chunk.yml`): each slice resumes from
the previous slice's sysroot and grains, builds until `BOOTSTRAP_DEADLINE`
(`bootstrap.sh` then stops before starting another package and exits 3), and
hands everything on. Desktop grains are cached the same way as the base.

The final `iso` job stages `base desktop` into a rootfs, runs
`os/desktop/live-session.sh --check` (a passwordless `live` user with SDDM
autologin into LXQt, plus a runit check service), builds the ISO and boots it
under QEMU with `-vga std`. It passes on `SALTOS_DESKTOP_OK`, printed once Xorg,
`lxqt-session`, `lxqt-panel` and `openbox` are all running; a screenshot of the
session is uploaded with the serial log.

Mesa is built with the Gallium `swrast` (llvmpipe/softpipe), `virgl`, `zink`,
AMD (`radeonsi`), `nouveau` and, on x86_64, `crocus` drivers; aarch64 adds the
common ARM SoC drivers. `iris` (Intel gen8 and newer) is left out for now: it
needs `intel-clc`, i.e. clang, libclc and SPIRV-LLVM-Translator, which the native
stack does not build yet, so those GPUs fall back to llvmpipe.

This mirrors the working `selfhost-iso` workflow, which already builds a
from-source native ISO and passes a QEMU boot test on a free runner; splitting the
larger recipe graph across staged jobs keeps each within the same limits.
