# saltOS Build Conventions

This file is the contract shared by every component and contributor (human or agent).
It is authoritative for formats, paths, and command surfaces.

## Architectures

saltOS targets two architectures:

- `x86_64`
- `aarch64`

Every recipe must declare both in `arch` unless a package is genuinely arch-specific.
The repository has one tree per arch: `repo/x86_64/` and `repo/aarch64/`.
CI builds a matrix over both. `salt` detects the host arch at runtime (uname machine,
normalizing `arm64` -> `aarch64`, `amd64` -> `x86_64`).

## Source layout

```
src/libsalt/       C core library (salt_core), headers in include/salt/
src/salt/          C++23 CLI (salt)
recipes/<name>/    package recipes (recipe.toml [+ patches/, files/])
repo/<arch>/       built repository (index.toml, index.toml.sig, packages/)
os/runit/          runit service definitions and the svc wrapper
os/btrfs/          subvolume layout + snapshot/rollback helpers
os/installer/      Calamares branding, settings, and custom modules
os/iso/            ISO build system (rootfs bootstrap + image assembly)
os/bootstrap/      base-system build orchestration (toolchain -> base)
keys/              signing keys (public committed, secret git-ignored)
tests/             ctest suite
docs/              documentation
.github/workflows/ CI/CD
```

## Build & quality gates

- C: C11. C++: C++23. Build with CMake + Ninja.
- Configure: `cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Test: `ctest --test-dir build --output-on-failure`
- No new code comments, banners, headers, license text, or attribution prose anywhere.
- Native tooling only. No npm/yarn. Permissive-licensed deps only, no telemetry.

## Recipe format (`recipes/<name>/recipe.toml`)

```toml
name = "zlib"
version = "1.3.1"
release = 1
summary = "Compression library"
license = "Zlib"
arch = ["x86_64", "aarch64"]

[source]
url = "https://zlib.net/zlib-1.3.1.tar.gz"
sha256 = "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"

[build]
system = "make"            # one of: make, autotools, cmake, meson, kernel, custom
deps = ["gcc", "make"]
script = """               # optional; required when system = "custom"
./configure --prefix=/usr
make
make DESTDIR="$SALT_DEST" install
"""

[package]
deps = ["glibc"]

[reproducibility]
status = "verified"        # verified | unverified
# reason = "..."           # required when status = "unverified"
```

Build environment variables provided to recipe scripts:

- `SALT_SRC`   extracted source directory (cwd)
- `SALT_DEST`  staging DESTDIR; installed tree is packaged from here
- `SALT_ARCH`  target arch
- `SALT_JOBS`  parallelism

Network access is permitted only during source fetch, then denied for the build.

## Package format (`.saltpkg`)

A `.saltpkg` is an uncompressed POSIX ustar archive containing, in order:

```
metadata.toml      package identity + deps + reproducibility
manifest.toml      every installed file: path, mode, size, sha256, type, linkname
files.tar.zst      zstd-compressed ustar of the payload, paths relative to /
scripts/           optional, discouraged; post-install hooks
```

Naming: `<name>-<version>-<release>-<arch>.saltpkg`

## Repository layout

```
repo/<arch>/
  index.toml          signed list of packages with name/version/release/sha256/size/deps
  index.toml.sig      ed25519 signature (hex) of index.toml
  packages/<name>-<version>-<release>-<arch>.saltpkg
```

Trust order: verify `index.toml.sig` against the trusted public key, then verify each
package's sha256 against the signed index before install.

## salt CLI surface

```
salt sync                 refresh repo index from configured source
salt search <term>
salt install <pkg>...
salt remove <pkg>...
salt update
salt rollback
salt deployments
salt verify
salt query <pkg>
salt files <pkg>
salt owner <path>
salt build <recipe-dir>
salt lint <recipe-dir>
salt sign <pkg>
salt repo publish <out-dir>
salt trust <subcommand>
```

Global flags: `--root <dir>` (operate on an alternate root, default `/`),
`--repo <url-or-path>`, `--key <pubkey-hex-or-file>`, `--yes`.

## Filesystem & rollback model

Btrfs subvolumes: `@ @home @var @log @snapshots`.
Every transaction snapshots `@` before mutating, records a deployment row, and on
failure restores automatically. `salt rollback` restores the previous deployment.
User data (`@home`) is never rolled back.

State paths (under `--root`):

```
/var/lib/salt/db.sqlite     package database + transaction log + deployments
/var/lib/salt/state/        per-transaction saved file state (non-btrfs fallback)
/.snapshots or /@snapshots  btrfs snapshots
/etc/salt/repo.conf         repo source + trusted key
```

## Init system

runit. Service dirs live in `/etc/runit/sv/<name>` (run + optional finish/check),
enabled via symlink into `/etc/runit/runsvdir/current/`. The `svc` wrapper exposes
`svc enable|disable|start|stop|restart|status <name>`.

## Installer

Calamares (Qt). Branding under `os/installer/branding/saltos`, settings in
`os/installer/settings.conf`, custom modules under `os/installer/modules/`. A custom
module configures Btrfs subvolumes, installs the base via `salt`, writes the bootloader
deployment entries, and seeds `/var/lib/salt/db.sqlite`.
