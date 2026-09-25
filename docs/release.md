# Releases

A release is one commit built into every user-facing edition, published as a
GitHub pre-release tagged `v<version>`.

```sh
git tag v0.1.0 && git push origin v0.1.0      # or run the `release` workflow by hand with a version
```

`.github/workflows/release.yml` calls each edition workflow as a reusable
workflow on that commit, so an image is released only if it passed its own
QEMU gates there:

| Workflow | Release file(s) |
| --- | --- |
| `installer-base` | `saltos-<v>-base-x86_64.iso` |
| `installer-iso`, `installer-iso-arm64` | `saltos-<v>-installer-{x86_64,aarch64}.iso` |
| `live-iso` (desktop leg), `live-iso-arm64` (console leg) | `saltos-<v>-desktop-live-x86_64.iso`, `saltos-<v>-console-live-aarch64.iso` |
| `omakase-iso` | `saltos-<v>-omakase-{x86_64,aarch64}.iso` |
| `native-base` | `saltos-<v>-native-base-{x86_64,aarch64}.iso` |
| `pi5-image` | `saltos-<v>-raspberry-pi-5-aarch64.img.zst` |
| `thinkpad-image` | `saltos-<v>-thinkpad-x86_64.iso` |
| `vm-image-x86`, `vm-image-arm64` | `saltos-<v>-vm-desktop-x86_64.img.zst`, `saltos-<v>-vm-desktop-apple-aarch64.img.zst` |

The publish job renames each image, splits anything over GitHub's 2 GiB asset
limit into `.partNN` pieces, writes `SHA256SUMS`, and creates (or updates) the
pre-release with notes that mark it experimental.

## Signing

When the `OTA_SECRET_KEY` repository secret is set, the release also runs
`ota-publish` (signed base grains, the per-arch index on GitHub Pages and the
rolling `ota-stable` channel; see [ota.md](ota.md)).

Without it the release is **unsigned**: the notes say so, no OTA channel is
published, and installed systems do not receive updates until one exists.
Images still carry the committed `keys/ota.pub`, so a signed channel published
later with the matching secret key reaches them without reinstalling. If that
key is lost, generate a new pair (`salt keygen ./keys ota`), commit the new
`keys/ota.pub`, set the secret, and cut a new release; images from before the
change need a reinstall or a manual key update (`/etc/salt/keys/ota.pub`).

The native desktop ISO (`native-desktop`) is built separately after each green
`native-base` run and is not part of a release until it passes its boot test.
