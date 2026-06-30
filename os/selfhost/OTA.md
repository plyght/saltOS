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

Upload `./base-repo` to any static host (GitHub Releases, S3, a plain web
server). The client only fetches `<source>/<arch>/index.toml`, its `.sig`, and
the `.grain` files — no server-side logic.

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

## Update a running system

```sh
salt sync       # fetch + verify the signed index against the trusted key
salt update     # install any base/app grain whose version is newer
salt rollback   # btrfs/A-B snapshot taken before the txn, if needed
```

`salt update` selects the **newest** version in the index (natural version
compare), verifies every grain's signature against the key in `repo.conf`, and
rejects a tampered/unsigned index. Verified end-to-end: install salt as a grain,
publish a newer one, `salt update` swaps the on-disk binary; same for the
`saltos-base` config grain (and `repo.conf` is preserved across the update).
