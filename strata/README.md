# Stratum Recipes

A **stratum** is a managed foreign-distro userspace that the native `salt` tool
bootstraps, runs apps from, exposes commands from, and rolls back. A stratum
recipe is the TOML definition that tells `salt` how to build and integrate one.

This directory holds the official stratum definitions saltOS ships so users do
not have to invent them from scratch:

```
void.toml      Void Linux   (xbps)
arch.toml      Arch Linux   (pacman)
debian.toml    Debian       (apt)
```

For the wider model — running foreign software, exposing commands, component
adoption, and per-stratum rollback — see [../docs/strata.md](../docs/strata.md)
and sections 8 and 11 of [../DISTRO.md](../DISTRO.md).

## Schema

A recipe is a single TOML file. The top-level keys identify the stratum:

```toml
name = "arch"               # stratum name, used on the salt command line
family = "arch"             # distro family (void | arch | debian | ...)
arch = "x86_64"             # CPU architecture of the root
package_manager = "pacman"  # native package manager inside the stratum
root = "/strata/arch"       # where the stratum root filesystem lives
trust = "official"          # trust level of this definition
```

### `[bootstrap]`

How the stratum root is first populated.

```toml
[bootstrap]
method = "rootfs"   # "rootfs" | "debootstrap"
url = "https://.../archlinux-bootstrap-x86_64.tar.zst"
sha256 = ""         # pinned hash of the downloaded tarball
strip = 1           # leading path components to strip on extract
```

- `method` — the bootstrap method (see below).
- `url` — the rootfs/bootstrap tarball to download (empty for `debootstrap`).
- `sha256` — the pinned hash of that tarball, verified before extraction.
- `strip` — number of leading path components to strip when extracting, the
  way `tar --strip-components` does. The Arch bootstrap tarball extracts into a
  top-level `root.x86_64/` directory, so it uses `strip = 1`; the Void rootfs
  unpacks directly at the root, so it uses `strip = 0`.

### `[integration]`

Which host integration permissions the stratum is granted.

```toml
[integration]
graphics = true   # Wayland/X11 sockets and GPU device access
audio = true      # PulseAudio/PipeWire sockets
dbus = true       # D-Bus session access
```

### `[[repository]]`

One or more upstream repositories the stratum's package manager uses. Each is an
array-of-tables entry with a name and URL:

```toml
[[repository]]
name = "core"
url = "https://geo.mirror.pkgbuild.com/$repo/os/$arch"

[[repository]]
name = "extra"
url = "https://geo.mirror.pkgbuild.com/$repo/os/$arch"
```

URLs may contain package-manager substitution variables (for example pacman's
`$repo` and `$arch`); these are passed through to the stratum's native
configuration unchanged.

## Bootstrap methods

- **`rootfs`** — download the tarball at `url`, verify it against `sha256`, and
  extract it into `root` (applying `strip`). Used by Void and Arch, which both
  publish a ready-made rootfs/bootstrap tarball.
- **`debootstrap`** — use the host's `debootstrap` tool to build the root from
  the configured repository instead of downloading a prebuilt tarball. Used by
  Debian. `url`/`sha256` are left empty for this method.

## Keeping URLs and hashes fresh

The bootstrap `url` and `sha256` are pinned on purpose, but the upstream
artifacts they point at **rotate over time**, so maintainers must keep them
current:

- **Void** publishes a *dated* rootfs (for example
  `void-x86_64-ROOTFS-20240314.tar.xz`). Old dates are eventually removed
  upstream, so both the date in the `url` and the `sha256` need periodic
  refreshing.
- **Arch** publishes its bootstrap tarball under `iso/latest/`, which always
  points at the newest monthly release. The URL stays stable but its contents —
  and therefore the `sha256` — change every month.

When `sha256` is empty it means the hash has not yet been pinned for the current
artifact and must be filled in before the definition is considered complete.

## Working with recipes

```sh
salt stratum lint <recipe.toml>      # validate a recipe against this schema
salt stratum add <name|recipe.toml>  # bootstrap a stratum from a recipe
```

`salt stratum lint` checks a recipe for correctness and policy before it is
used. `salt stratum add` takes either the name of a shipped stratum (for
example `salt stratum add arch`) or a path to a `recipe.toml`, then bootstraps
the stratum root according to its `[bootstrap]` section.
