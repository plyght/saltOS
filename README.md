# saltOS

saltOS is an independent, Bedrock-like Linux distribution: a daily-drivable OS
that owns its own boot, base system, init, package manager, and rollback model —
while being able to run software from *any* major Linux ecosystem through
managed, rollbackable environments called **strata**.

It has two package planes:

- **Native plane** — saltOS-owned packages built by `salt`, in the `.grain`
  format, from a curated and signed binary repository. This is the curated base:
  kernel, libc, init, bootloader integration, system tools, and salt's own glue.
- **Stratum plane** — managed foreign-distro roots (Arch, Debian, Void, Fedora,
  openSUSE, Alpine, …) that provide package depth through their own package
  managers (`pacman`, `apt`, `xbps`, `dnf`, `zypper`, `apk`), isolated by default
  and selectively exposed on the host when you choose.

```txt
saltOS is the host OS and control plane.
Native salt packages provide the curated base and first-party system.
Foreign distro strata provide package depth and interchangeable components.
salt decides what is integrated, exposed, adopted, or isolated.
```

> Status: experimental and under active construction. The model is real and
> proven in CI; it is not yet a polished daily driver.

## What's proven in CI

- **Every major package manager, end-to-end.** The `strata` workflow runs a
  matrix over **Alpine (apk), Void (xbps), Arch (pacman), Debian (apt),
  Fedora (dnf), and openSUSE (zypper)**. Each job: bootstrap the stratum →
  run a command inside it (chroot + Linux namespaces) → install a package with
  the foreign package manager → take a snapshot → roll back → expose a command →
  tear down. All six are green.
- **It boots.** The `live-iso` workflow builds a console ISO that boots in QEMU,
  runs `salt`, and initializes the stratum plane (`SALTOS_BOOT_OK` +
  `SALTOS_STRATUM_OK` on the serial console).
- **The native plane works.** The `ci` workflow builds `salt`, passes the unit
  suite and an end-to-end CLI smoke test (build → sign → publish →
  signature-verified sync → install → verify → remove → rollback), and proves
  the maintainer pipeline on real upstream source (`zlib` → signed `.grain`).
- **A fully self-hosted, from-source image** (`selfhost-iso`) compiles the Linux
  kernel, glibc, GNU bash/coreutils, BusyBox, runit, and a static `salt` entirely
  from upstream source and boots under runit as PID 1 — no Debian, no `apt`, no
  prebuilt distro packages.

## The stratum plane

A stratum is a managed foreign userspace rooted under `/strata/<name>`. saltOS
owns the bootstrap, snapshots, command routing, and rollback; the foreign package
manager only owns its own root.

```sh
salt stratum add arch              # bootstrap an Arch userspace
salt stratum list                  # show managed strata
salt run arch firefox              # run foreign software (Wayland/X11/dbus/audio/GPU pass through)
salt pkg arch install ripgrep      # foreign package-manager passthrough (pre-op snapshot taken)
salt expose arch rg                # expose a command as a host shim on PATH
salt expose-desktop arch firefox   # expose a desktop app
salt provider set coreutils debian/coreutils   # adopt a component from a stratum
salt stratum snapshot arch         # per-stratum snapshot
salt stratum rollback arch         # roll a stratum back
```

Foreign software is **explicit by default** — you run it with `salt run` or opt
in with `salt expose`; it does not pollute the host namespace. Exposed commands
are saltOS-owned shims in `/usr/local/salt/shims` (on `PATH`). Trust boundaries
stay visible: a native `.grain`, a package from a foreign distro's official repo,
and a lower-trust third-party package are never treated as equivalent.

Strata are defined by small TOML recipes in `strata/` and bootstrap by one of
three methods: a rootfs tarball (`rootfs`), `debootstrap`, or an OCI image export
(`oci`, via docker/podman). See [`docs/strata.md`](docs/strata.md).

## The native plane

- `salt` package manager: native, transactional, rollback-aware,
  signature-checking, source-hash-aware, daemonless.
- `.grain` format (a *grain* of salt): a ustar container of `metadata.toml`,
  `manifest.toml`, a zstd-compressed payload, and optional (discouraged) scripts.
- Curated binary repository with an ed25519-signed `index.toml`.
- Explicit contributor trust model (unknown / vouched / maintainer / denounced)
  and supply-chain risk scanning.
- TOML package recipes with pinned source URLs and hashes. All 111 recipes pass
  `salt lint`; the rest pin real upstream URLs pending verified hashes.

## Rollback everywhere

```sh
salt update                 # snapshots @ and records a deployment, then upgrades
salt rollback               # restore the previous known-good host deployment
salt stratum rollback arch  # roll back a single stratum after a bad foreign update
```

Every host transaction snapshots `@` before mutating the system and rolls back
automatically on failure; the bootloader can boot a previous deployment. Each
stratum is snapshot-capable independently (Btrfs subvolume, or a tarball
fallback). User data in `@home` is never rolled back. See
[`docs/rollback.md`](docs/rollback.md).

## Architecture at a glance

| Layer | Choice |
| --- | --- |
| Kernel | upstream Linux, latest stable, minimal patching |
| C library | glibc (foreign strata may use their own, e.g. musl in Alpine) |
| Init | runit (`svc` wrapper); strata never take PID 1 |
| Filesystem | Btrfs (`@ @home @var @log @snapshots @strata`) |
| Bootloader | GRUB (snapshot/deployment entries) |
| Native package manager | `salt` (C core `halite` + C++23 CLI) |
| Native package format | `.grain` |
| Stratum manager | `salt stratum` / `salt run` / `salt pkg` / `salt expose` / `salt provider` |
| Isolation | private mount namespace + bind mounts + chroot + privilege drop |
| Databases | SQLite: `db.sqlite` (native) and `strata.sqlite` (strata/exposed/providers) |
| Repository | curated, signed `index.toml` per arch |
| Desktop | LXQt (in progress); apps can be native or stratum-backed |
| Installer | Calamares |

See [`docs/architecture.md`](docs/architecture.md) for the full picture.

## Supported architectures

- `x86_64` (primary)
- `aarch64`

`salt` detects the host arch at runtime (normalizing `arm64`→`aarch64`,
`amd64`→`x86_64`). The repository keeps one tree per arch.

## Building from source

saltOS uses CMake + Ninja. C is C11, C++ is C++23. The build host is Linux.

Dependencies (Debian/Ubuntu names): `cmake ninja-build pkg-config libzstd-dev
libsodium-dev libsqlite3-dev build-essential`.

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The `salt` binary is produced under `build/src/salt/salt`.

## salt command cheat-sheet

```
native packages
  salt sync                 refresh the repo index from the configured source
  salt search <term>        search the repository
  salt install <pkg>...     install one or more native packages
  salt remove <pkg>...      remove packages
  salt update               upgrade the system (snapshot + transaction)
  salt rollback             restore the previous deployment
  salt deployments          list deployments / rollback points
  salt verify               verify installed files against recorded hashes
  salt query|files|owner    inspect packages and file ownership

strata + integration
  salt stratum add <name>           bootstrap a foreign distro stratum
  salt stratum list|status|remove   manage strata
  salt stratum snapshot|rollback    per-stratum rollback
  salt run <stratum> <cmd> [args]   run a command inside a stratum
  salt pkg <stratum> <op> [pkg]     foreign package-manager passthrough
  salt expose <stratum> <cmd>       expose a command as a host shim
  salt expose-desktop <stratum> <a> expose a desktop app
  salt provider set|rollback        adopt/revert component providers
  salt service import|enable|start  integrate stratum daemons under runit

maintainer
  salt build <recipe-dir>   build a .grain from a recipe
  salt lint <recipe-dir>    lint a recipe
  salt stratum lint <file>  lint a stratum recipe
  salt sign <pkg>           sign a .grain / index
  salt repo publish <dir>   build and sign a repository index
  salt trust <subcommand>   manage contributor trust and supply-chain scans
```

Global flags: `--root <dir>`, `--repo <url-or-path>`, `--key <pubkey-hex-or-file>`,
`--yes`.

## Documentation

- [Architecture](docs/architecture.md)
- [Package manager](docs/package-manager.md)
- [Strata (the stratum plane)](docs/strata.md)
- [Writing recipes](docs/recipes.md)
- [Repository model](docs/repository.md)
- [Trust model](docs/trust-model.md)
- [Rollback](docs/rollback.md)
- [Installation](docs/installation.md)
- [Contributing](docs/contributing.md)
- [Build conventions](docs/CONVENTIONS.md) (authoritative contract)

## License and policy

saltOS prefers permissive and copyleft open-source software. Every native package
must declare its license. The project avoids telemetry by default, unclear
licenses, repackaged binaries of unknown provenance, and packages that download
code at install time. Foreign strata follow their upstream distributions'
policies — saltOS surfaces that boundary rather than hiding it.
