# Building the native base

saltOS builds its own minimal base from `recipes/` (the native plane) rather than
seeding it from another distribution. The big userland comes from a stratum chosen
at install time; the base — toolchain, core userland, kernel, bootloader, and the
installer tools — is self-built and reused.

## Pipeline

1. `os/bootstrap/bootstrap.sh` builds the recipe graph in topological order
   (`os/bootstrap/build-order.toml`) via `salt build`, installing each resulting
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
uploaded artifact, so the pipeline populates itself within the run. The
`toolchain` job additionally restores/saves an `actions/cache` keyed on a hash of
`recipes/**` + `build-order.toml`; combined with `bootstrap.sh` reusing any grain
already present, unchanged packages are not rebuilt on later runs.

This mirrors the working `selfhost-iso` workflow, which already builds a
from-source native ISO and passes a QEMU boot test on a free runner; splitting the
larger recipe graph across staged jobs keeps each within the same limits.
