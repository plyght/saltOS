# saltOS

saltOS is a personal-first, Unix-like Linux distribution focused on being fast,
current, rollbackable, understandable, and resistant to common open-source
supply-chain failure modes.

It is not trying to be another general-purpose community distro. It is a small,
coherent operating system with its own package manager (`salt`), its own package
format (`.saltpkg`), a curated and signed binary repository, TOML package
recipes, and a simple full-system rollback model built on Btrfs snapshots.

> Status: experimental. The model and tooling are under active construction.
> saltOS does not yet need thousands of packages; it needs a coherent base
> system that proves the model works.

## What saltOS is

- A real Unix-like desktop OS with a traditional `/usr`, `/etc`, `/var`, `/opt`
  filesystem layout, not a content-addressed store.
- Current but curated: recent software flows through a reviewed, signed
  repository instead of an untrusted user repo. There is no AUR equivalent.
- Rollback as a first-class feature: every system transaction takes a Btrfs
  snapshot, and a bad update is undone with one command.
- Reproducible where it matters: pinned source hashes, signed metadata,
  declared dependencies, with reproducibility tracked and improved over time.
- Understandable by one maintainer: simple native C/C++ tools over clever
  frameworks.

## Feature list

- `salt` package manager: native, transactional, rollback-aware,
  signature-checking, source-hash-aware, daemonless.
- `.saltpkg` format: ustar container of `metadata.toml`, `manifest.toml`, a
  zstd-compressed payload, and optional (discouraged) scripts.
- Curated binary repository with an ed25519-signed `index.toml`.
- Explicit contributor trust model (unknown / vouched / maintainer / denounced)
  and supply-chain risk scanning.
- Full-system rollback via Btrfs subvolumes and per-transaction deployments.
- runit init with a friendly `svc` wrapper.
- LXQt desktop edition with Helium as the default browser.
- Both `x86_64` and `aarch64`.

## Architecture at a glance

| Layer | Choice |
| --- | --- |
| Kernel | upstream Linux, latest stable, minimal patching |
| C library | glibc |
| Init | runit (`svc` wrapper) |
| Filesystem | Btrfs (`@ @home @var @log @snapshots`) |
| Bootloader | GRUB (snapshot/deployment entries) |
| Package manager | `salt` (C core `salt_core` + C++23 CLI) |
| Package DB | SQLite at `/var/lib/salt/db.sqlite` |
| Repository | curated, signed `index.toml` per arch |
| Desktop | LXQt, Wayland-first where practical |
| Installer | Calamares |

See [`docs/architecture.md`](docs/architecture.md) for the full picture and the
control flow of an install and an update + rollback.

## Supported architectures

- `x86_64`
- `aarch64`

`salt` detects the host arch at runtime (normalizing `arm64` to `aarch64` and
`amd64` to `x86_64`). The repository keeps one tree per arch and CI builds a
matrix over both.

## Building from source

saltOS uses CMake + Ninja. C is C11, C++ is C++23. The build host is Linux; CI
(GitHub Actions on `ubuntu-latest` and `ubuntu-24.04-arm`) is where the project
is compiled and tested.

Dependencies (Debian/Ubuntu names): `cmake ninja-build pkg-config libzstd-dev
libsodium-dev libsqlite3-dev build-essential`.

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The `salt` binary is produced under `build/src/salt/salt`.

## Building a package

Recipes live in `recipes/<name>/recipe.toml`. To lint, scan, and build one into
a `.saltpkg`:

```sh
salt lint recipes/zlib
salt trust scan recipes/zlib
salt build recipes/zlib
```

Then sign the artifact and publish a signed repository index:

```sh
salt sign out/zlib-1.3.1-1-x86_64.saltpkg
salt repo publish out/
```

See [`docs/recipes.md`](docs/recipes.md) for the full recipe schema and
[`docs/repository.md`](docs/repository.md) for the repository model.

## The rollback story

```sh
salt update      # snapshots @ and records a new deployment first
# a bad update happens
salt rollback    # restore the previous known-good deployment
reboot
```

Every transaction snapshots `@` before mutating the system, records a deployment
row, and on failure rolls back automatically. The bootloader can also boot the
previous deployment. User data in `@home` is never rolled back. Inspect
deployments with `salt deployments` and check integrity with `salt verify`. See
[`docs/rollback.md`](docs/rollback.md).

## salt command cheat-sheet

```
salt sync                 refresh the repo index from the configured source
salt search <term>        search the repository
salt install <pkg>...     install one or more packages
salt remove <pkg>...      remove packages
salt update               upgrade the system (snapshot + transaction)
salt rollback             restore the previous deployment
salt deployments          list deployments / rollback points
salt verify               verify installed files against recorded hashes
salt query <pkg>          show package details
salt files <pkg>          list files owned by a package
salt owner <path>         show which package owns a path
salt build <recipe-dir>   build a .saltpkg from a recipe
salt lint <recipe-dir>    lint a recipe
salt sign <pkg>           sign a .saltpkg / index
salt repo publish <dir>   build and sign a repository index
salt trust <subcommand>   manage contributor trust and supply-chain scans
```

Global flags: `--root <dir>`, `--repo <url-or-path>`, `--key <pubkey-hex-or-file>`,
`--yes`.

## Documentation

- [Architecture](docs/architecture.md)
- [Package manager](docs/package-manager.md)
- [Writing recipes](docs/recipes.md)
- [Repository model](docs/repository.md)
- [Trust model](docs/trust-model.md)
- [Rollback](docs/rollback.md)
- [Installation](docs/installation.md)
- [Contributing](docs/contributing.md)
- [Build conventions](docs/CONVENTIONS.md) (authoritative contract)

## License and policy

saltOS prefers permissive and copyleft open-source software. Every package must
declare its license. The project avoids telemetry by default, unclear licenses,
repackaged binaries of unknown provenance, and packages that download code at
install time.
