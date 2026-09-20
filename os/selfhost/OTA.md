# saltOS base as grains + OTA

The saltOS base — the `salt` binary, the `saltos-base` config layer (runit
services, console login, salt config, strata recipes), and the kernel — can be
packaged as signed `.grain` packages so `salt update` replaces the **base** in
place, not just stratum apps. No full reflash for a userland/salt/kernel fix.

## Build + publish the base grains

```sh
# Sign with YOUR persistent OTA key (keep the .sec private; it signs every push).
salt keygen ./ota-keys saltos-ota
SEC=$(cat ./ota-keys/*.sec)

SALT=/path/to/salt SEC_KEY=$SEC ARCH=aarch64 VERSION=0.1.1 \
  OUT=./base-repo ROOTFS=/path/to/built/rootfs KERNEL=/path/to/Image \
  sh os/selfhost/build-base-grains.sh
# -> signed repo at ./base-repo/aarch64 (index.toml + index.toml.sig + packages/)
```

Upload `./base-repo` to any static host (S3, a plain web server). The client
only fetches `<source>/<arch>/index.toml`, its `.sig`, and the `.grain` files —
no server-side logic. Pass `URL_BASE=<url>` to record an absolute
`url = <url>/<filename>` per package when the grains live somewhere other than
`<source>/<arch>/packages/`.

The official channel does exactly that on GitHub for free: the
`ota-publish` workflow (`.github/workflows/ota-publish.yml`, tag `v<version>` or
`workflow_dispatch`) builds and signs the grains with the `OTA_SECRET_KEY`
secret, uploads them as assets of the `v<version>` and rolling `ota-stable`
Releases, and deploys the signed index to GitHub Pages at
`https://plyght.github.io/saltOS/<arch>/index.toml`. Images bake that URL and
`keys/ota.pub`. Setup and the owner's three commands: [docs/ota.md](../../docs/ota.md).

## Make an image ship a grain-tracked base

Build the installed image with registration enabled so its package DB tracks the
base at its built version and `repo.conf` points at your OTA host:

```sh
REGISTER_BASE_GRAINS=1 \
SALT_BIN=/path/to/salt \
OTA_URL=https://your-ota-host/base-repo \
OTA_KEY=$SEC \
... (normal build-installed-image.sh env) ...
```

If `OTA_KEY` is omitted a keypair is generated and saved next to the image —
keep the `.sec` to sign future pushes. With registration off (default), the base
is baked into the image as before; you can still migrate a running system to
grain-tracking later by pointing `repo.conf` at the repo and
`salt install salt saltos-base linux-saltos`.

To package a complete kernel (vmlinuz + initramfs + modules) from an installed
tree instead of a bare `Image`, pass `KERNEL_TREE=<root> KERNEL_RELEASE=<uname -r>`;
this is what `os/build/vm-x86.sh` does to register the factory kernel and what
`os/ota/ship.sh` uses to publish a kernel upgrade.

## Update a running system

```sh
salt-ota check    # sync + report what would be upgraded
salt-ota run      # sync, upgrade everything in one snapshotted transaction,
                  # arm a trial boot if the kernel changed (exit 3 = reboot)
salt-ota confirm  # after the reboot: health checks, make the new kernel default
salt deployments  # generations with date, kernel and packages changed
salt rollback [N] # previous generation becomes the root again; reboot
```

`salt update` selects the **newest** version in the index (natural version
compare), verifies every grain's hash and signature against the key in
`repo.conf`, and rejects a tampered/unsigned index or grain leaving the system
unchanged. Verified end-to-end in QEMU by `os/ota/test-qemu.sh` (workflow
`ota`): factory image with `salt` + `linux-saltos` registered as grains, a
newer `salt` and kernel published with `ship.sh`, `salt-ota run`, trial boot of
the new kernel, automatic fallback when it is not confirmed, confirmation,
`salt rollback` to the factory generation, and refusal of a bad-hash and a
bad-signature repository. Commands and exit codes: [docs/ota.md](../../docs/ota.md);
the deployment/rollback model: [docs/rollback.md](../../docs/rollback.md).
