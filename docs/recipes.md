# Writing Recipes

A saltOS package is built from a **recipe**: a single Lua file,
`recipe.lua`, that pins where the source comes from, how to build it, and what
the resulting package depends on. Recipes are deliberately small, declarative,
and easy to audit. A recipe is not a program that builds anything: it is a
sandboxed chunk that *returns a table* of plain data (see
[The configuration sandbox](#the-configuration-sandbox)). Lua is used so a
recipe can name a value once (`local version = "1.3.1"`) and build strings
from it, while still evaluating to nothing more than data; everything a
program writes and signs (a grain's `metadata.toml`, `index.toml`) stays TOML.

This document is the schema reference and authoring guide. For how recipes are
turned into packages and installed, see [package-manager.md](package-manager.md);
for the admission and supply-chain rules recipes must satisfy, see
[trust-model.md](trust-model.md); for how built packages reach users, see
[repository.md](repository.md).

## Recipe directory layout

Each package lives in its own directory under `recipes/`:

```
recipes/<name>/
  recipe.lua       required: the recipe itself
  patches/         optional: *.patch applied with `patch -p1`, in name order, before the build
  files/           optional: extra files, copied into the source tree as $SALT_FILES
```

A `scripts/` directory is not allowed; install-time code is declared in
[`hooks`](#hooks).

The directory name should match the package `name`.

## Schema reference

A recipe returns a table with a small set of top-level keys and four nested
tables: `source`, `build`, `package`, and `reproducibility`. (Below, `build.deps`
means the `deps` field of the `build` table.)

```lua
return {
  name = "zlib",
  version = "1.3.1",
  release = 1,
  summary = "Compression library",
  license = "Zlib",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://zlib.net/zlib-1.3.1.tar.gz",
    sha256 = "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
  },
  build = {
    system = "make",           -- one of: make, autotools, cmake, meson, kernel, custom
    deps = { "gcc", "make" },
    -- optional; required when system = "custom"
    script = [[
./configure --prefix=/usr
make
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc" },
  },
  reproducibility = {
    status = "verified",       -- verified | unverified
    -- reason = "...",         -- required when status = "unverified"
  },
}
```

### The configuration sandbox

`recipe.lua` and every other human-authored saltOS configuration file (strata
recipes, `/etc/salt/system.lua`, `salt.lua`, `repo.lua`, `boot.lua`) is
evaluated by the same restricted Lua 5.4 interpreter built into `salt`:

- The file is a text chunk that must `return` a table. Only plain data survives
  evaluation: strings, integers, booleans and tables. A table is either a list
  (`{ "a", "b" }`) or has only string keys (`{ url = "..." }`); floats,
  functions and mixed tables are rejected. Multi-line text uses long strings,
  `[[ ... ]]` (backslashes inside are literal, and a newline right after `[[`
  is dropped).
- `local` variables, functions, string concatenation (`..`) and the `string`,
  `table`, `math` and `utf8` libraries are available, plus `salt.arch` (the
  target architecture: `$SALT_ARCH`, else the host's) and `salt.host_arch`.
- There is no `io`, `os`, `require`, `load`, `dofile` or `debug`: a config file
  cannot read or write files, run commands, reach the network or load other
  code. `print` writes to stderr. Evaluation is capped at 64 MiB of memory and
  a fixed instruction budget, so a runaway loop fails instead of hanging.

Because the result is only data, a recipe evaluates to the same table every
time for a given `salt.arch`. To see exactly what `salt` sees, or to read one
value from a script, use `salt eval`:

```sh
salt eval recipes/zlib/recipe.lua                 # every leaf, as `path = value`
salt eval recipes/zlib/recipe.lua source.url      # one scalar
salt eval recipes/zlib/recipe.lua build.deps      # a list, one element per line
```

A missing key exits 1 with no output; a table key prints the table's keys.

### Top-level keys

| Key | Type | Meaning |
| --- | --- | --- |
| `name` | string | Package name; matches the directory name. |
| `version` | string | Upstream version. |
| `release` | integer | saltOS packaging revision for this version, starting at 1. |
| `summary` | string | One-line description. |
| `license` | string | SPDX-style license identifier. Required. |
| `arch` | array of strings | Target architectures. Must include both `"x86_64"` and `"aarch64"` unless the package is genuinely architecture-specific. |

saltOS targets two architectures, `x86_64` and `aarch64`, and the repository
keeps one tree per arch. Declaring both in `arch` is the default; only narrow it
when a package truly cannot exist on one of them.

### `source`

| Key | Type | Meaning |
| --- | --- | --- |
| `url` | string | Pinned source URL. Must point at a specific, immutable artifact. |
| `sha256` | string | SHA-256 of the downloaded source. Verified before the build runs. |

Both are mandatory. The source URL must be pinned and the hash must be pinned;
unpinned or hashless sources are rejected.

### `build`

| Key | Type | Meaning |
| --- | --- | --- |
| `system` | string | One of `make`, `autotools`, `cmake`, `meson`, `kernel`, `custom`. |
| `deps` | array of strings | Build-time dependencies. |
| `script` | string | Optional build script. **Required when `system = "custom"`.** |
| `strip` | boolean | Default `true`. Set `false` to ship ELF files with their debug info. |

After the build, `salt build` finalizes `$SALT_DEST` on the host before
packaging:

- **hardlinks** become relative symlinks to the first path of each inode (the
  grain format stores files and symlinks only, so extra links would otherwise be
  packaged as full copies);
- **ELF files are stripped** unless `strip = false`: executables and shared
  objects with `--strip-unneeded`; the dynamic loader, `libc`, `libpthread`,
  `libthread_db`, objects and static archives with `--strip-debug`. Kernel
  modules and firmware are untouched. `SALT_STRIP` names the strip binary
  (default `strip`); files it cannot handle are left as they are;
- `usr/share/info/dir` is removed (every texinfo package would ship it).

For the well-known build systems, `salt build` knows the standard
configure/build/install incantation, so `script` can be omitted. For
`system = "custom"`, the `script` is the build, and it must be provided. A
`script` may also be supplied alongside a known system when a package needs an
out-of-the-ordinary sequence (as `glibc` does below).

### `package`

| Key | Type | Meaning |
| --- | --- | --- |
| `deps` | array of strings | Runtime dependencies of the installed package. |

Runtime dependencies must be declared explicitly; they are recorded in the
package metadata and the local database and are used to keep installs and
removals consistent.

### `reproducibility`

| Key | Type | Meaning |
| --- | --- | --- |
| `status` | string | `verified` or `unverified`. |
| `reason` | string | Required when `status = "unverified"`; explains why. |

See [Reproducibility status](#reproducibility-status) below.

### `hooks`

Install hooks are the only install-time code a grain can carry. They are
optional, declared here, and restricted:

| Key | Runs |
| --- | --- |
| `post_install` | after a fresh install's files are in place |
| `post_upgrade` | after an upgrade's files are in place (instead of `post_install`) |
| `pre_remove` | before a package's files are removed |
| `post_remove` | after they are removed |

```lua
  hooks = {
    post_install = [[
fc-cache -s
]],
  },
```

- Each value is a `/bin/sh -e` body stored in the grain's `metadata.toml`,
  covered by the package hash and repository signature, and recorded in the
  local database (remove hooks run from there, so they belong to the installed
  version).
- Hooks run as root, `chroot`ed into the target root, in fresh network and
  mount namespaces (no network), with stdin from `/dev/null`, umask 022, a
  fixed environment (`PATH`, `HOME=/root`, `LC_ALL=C`, `SALT_PKG`,
  `SALT_VERSION`, `SALT_OLD_VERSION` for upgrades, `SALT_HOOK`) and a 300 s
  limit (`SALT_HOOK_TIMEOUT`). The target needs a `/bin/sh`.
- A failing `post_install`, `post_upgrade` or `pre_remove` fails the
  transaction, which is rolled back. A failing `post_remove` only warns (the
  files are already gone).
- Any other key under `hooks`, and a free-form `scripts/` directory in the
  recipe, is rejected by `salt build` and blocks `salt lint` / `salt trust
  scan`. Every declared hook is surfaced as an `install-hook` finding, and an
  added or changed hook as `hook-change`, so it is always reviewed.
- `SALT_SKIP_HOOKS=1` skips hooks with a warning (for example when assembling a
  root for another architecture, where its `/bin/sh` cannot execute).

## Build environment variables

During the build phase, `salt build` provides these environment variables to the
recipe script (and to the standard build systems):

| Variable | Meaning |
| --- | --- |
| `SALT_FILES` | The recipe's `files/` directory, copied to `$SALT_SRC/.salt-files` (so chrooted builds see it too). |
| `SALT_SRC` | The extracted source directory. This is the working directory (cwd) when the script starts. |
| `SALT_DEST` | The staging `DESTDIR`. Install into here; the package payload is built from this tree. |
| `SALT_ARCH` | The target architecture (`x86_64` or `aarch64`). |
| `SALT_JOBS` | Build parallelism; pass to `make -j"$SALT_JOBS"` and similar. |

Network access is permitted **only during the source fetch**. Once the source is
downloaded and its hash is verified, the network is denied for the rest of the
build. Builds must therefore be self-contained: everything they need must come
from the pinned source plus declared build dependencies.

Always install into `$SALT_DEST` (never directly into the live system), and use
`--prefix=/usr` so files land in the standard Unix-like layout when the package
is later installed.

The builder itself honours a few variables from its caller:

| Variable | Meaning |
| --- | --- |
| `SALT_WORK` | Directory holding the per-package `src/`, `dest/` and `dl/` trees (default `work/`). |
| `SALT_OUT` | Output directory; grains land in `$SALT_OUT/<arch>/packages/` (default `out/`). |
| `SALT_JOBS` | Parallelism handed to the recipe (default `4`). |
| `SALT_BUILD_ROOT` | If set, the recipe script runs inside `chroot "$SALT_BUILD_ROOT"`; `SALT_WORK` must then lie within that root so `SALT_SRC`/`SALT_DEST` can be expressed relative to it. Fetching, hash verification, extraction and packaging still happen outside the chroot. The bootstrap uses this to build the `base` and `desktop` stages natively inside the sysroot. |

Local `file://` sources that point at a git checkout are copied with
`git ls-files --cached --others --exclude-standard`, so ignored build outputs
never end up in `SALT_SRC`.

## Build systems

- **`make`** — a plain `Makefile`. `salt build` runs the build and a
  `DESTDIR`-aware install. Good for projects with no configure step.
- **`autotools`** — `./configure && make && make install`, with `--prefix=/usr`
  and `DESTDIR="$SALT_DEST"`. Provide a `script` when configure needs
  non-default flags.
- **`cmake`** — configures an out-of-tree build with a release configuration and
  `CMAKE_INSTALL_PREFIX=/usr`, then builds and installs into `SALT_DEST`.
- **`meson`** — sets up a build directory with `--prefix=/usr`, then
  `meson compile` and `meson install` into `SALT_DEST`.
- **`kernel`** — the Linux kernel's own build flow; used for the kernel package.
- **`custom`** — no assumptions; the `script` is the entire build and must be
  provided.

### Example: a `make` recipe

```lua
local version = "1.3.1"

return {
  name = "zlib",
  version = version,
  release = 1,
  summary = "Compression library",
  license = "Zlib",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://zlib.net/zlib-" .. version .. ".tar.gz",
    sha256 = "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
  },
  build = {
    system = "make",
    deps = { "gcc", "make" },
  },
  package = {
    deps = { "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
```

### Example: a `cmake` recipe

```lua
return {
  name = "example-tool",
  version = "2.4.0",
  release = 1,
  summary = "Example CMake-built utility",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://example.org/releases/example-tool-2.4.0.tar.xz",
    sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
  },
  build = {
    system = "cmake",
    deps = { "gcc", "cmake", "ninja" },
  },
  package = {
    deps = { "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
```

### Worked example: `glibc` (autotools with a custom script)

The real `recipes/glibc/recipe.lua` uses `system = "autotools"` together with an
explicit `script`, because glibc needs an out-of-tree build and specific
configure flags. It shows how `SALT_JOBS` and `SALT_DEST` are used:

```lua
local version = "2.41"

return {
  name = "glibc",
  version = version,
  release = 1,
  summary = "GNU C Library",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://ftp.gnu.org/gnu/glibc/glibc-" .. version .. ".tar.xz",
    sha256 = "a5a26b22f545d6b7d7b3dd828e11e428f24f4fac43c934fb071b6a7d0828e901",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "binutils", "python", "bison" },
    script = [[
#!/bin/sh
mkdir -p build
cd build
../configure --prefix=/usr \
    --disable-werror \
    --enable-kernel=4.19 \
    --enable-stack-protector=strong \
    libc_cv_slibdir=/usr/lib
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = {},
  },
  reproducibility = {
    status = "verified",
  },
}
```

Note how the script starts in `SALT_SRC` (the extracted source), creates an
out-of-tree `build/` directory, builds with `make -j"$SALT_JOBS"`, and installs
into `DESTDIR="$SALT_DEST"`.

## Requirements

Every recipe must, at minimum:

- pin the source URL,
- pin the source `sha256`,
- declare the `license`,
- declare build dependencies (`build.deps`),
- declare runtime dependencies (`package.deps`),
- declare a reproducibility `status`.

These are the same properties required for a package to be admitted to the
official repository: pinned source URL and hash, declared license, declared build
and runtime dependencies, a build that succeeds in a clean environment,
inspectable contents, and maintainer review. The build itself runs in a clean
environment with the network denied after fetch, so anything a build needs must
be declared. See [trust-model.md](trust-model.md) for the complete admission
rules.

## Reproducibility status

Reproducibility is enforced **progressively** — the system does not demand
bit-for-bit reproducibility of everything before it can exist, but every recipe
must declare where it stands.

- `status = "verified"` — the package is built reproducibly; rebuilding from the
  pinned source in a clean environment yields the same artifact.

  ```lua
  reproducibility = {
    status = "verified",
  },
  ```

- `status = "unverified"` — the package is not yet reproducible. A `reason` is
  required so the gap is explicit and reviewable.

  ```lua
  reproducibility = {
    status = "unverified",
    reason = "Chromium-derived browser build currently not bit-for-bit reproducible",
  },
  ```

Large, high-risk packages such as the Helium browser (Chromium-derived) are the
typical `unverified` cases and are held to stricter review in exchange.

## Linting and scanning a recipe

Before a recipe is built or proposed, run the recipe through the linter and the
supply-chain scanner:

```sh
salt lint recipes/zlib
salt trust scan recipes/zlib
```

- `salt lint` checks that the recipe is well-formed and policy-complete: pinned
  URL and hash, declared license, valid build system, declared build and runtime
  dependencies, and a present reproducibility status.
- `salt trust scan` looks for supply-chain risk signals — source URL changes, new
  install scripts, obfuscated scripts, new network access during build,
  unexpected binary blobs, embedded crypto wallet addresses, and similar — and
  reports findings at `info`, `warn`, or `block` severity.

Once a recipe lints cleanly and scans without blocking findings, build it with
`salt build recipes/<name>` (see [package-manager.md](package-manager.md)), then
sign and publish the result into the curated repository
([repository.md](repository.md)).
