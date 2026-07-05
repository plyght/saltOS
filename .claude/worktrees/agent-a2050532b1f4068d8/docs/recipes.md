# Writing Recipes

A saltOS package is built from a **recipe**: a single TOML file that pins where
the source comes from, how to build it, and what the resulting package depends
on. Recipes are deliberately small, declarative, and easy to audit. TOML is used
because it is smaller, clearer, and easier to parse safely than YAML — not
because of any language affinity.

This document is the schema reference and authoring guide. For how recipes are
turned into packages and installed, see [package-manager.md](package-manager.md);
for the admission and supply-chain rules recipes must satisfy, see
[trust-model.md](trust-model.md); for how built packages reach users, see
[repository.md](repository.md).

## Recipe directory layout

Each package lives in its own directory under `recipes/`:

```
recipes/<name>/
  recipe.toml      required: the recipe itself
  patches/         optional: patches applied to the source before building
  files/           optional: extra files referenced by the build
```

The directory name should match the package `name`.

## Schema reference

A recipe is a TOML document with a small set of top-level keys and four tables:
`[source]`, `[build]`, `[package]`, and `[reproducibility]`.

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

### `[source]`

| Key | Type | Meaning |
| --- | --- | --- |
| `url` | string | Pinned source URL. Must point at a specific, immutable artifact. |
| `sha256` | string | SHA-256 of the downloaded source. Verified before the build runs. |

Both are mandatory. The source URL must be pinned and the hash must be pinned;
unpinned or hashless sources are rejected.

### `[build]`

| Key | Type | Meaning |
| --- | --- | --- |
| `system` | string | One of `make`, `autotools`, `cmake`, `meson`, `kernel`, `custom`. |
| `deps` | array of strings | Build-time dependencies. |
| `script` | string | Optional build script. **Required when `system = "custom"`.** |

For the well-known build systems, `salt build` knows the standard
configure/build/install incantation, so `script` can be omitted. For
`system = "custom"`, the `script` is the build, and it must be provided. A
`script` may also be supplied alongside a known system when a package needs an
out-of-the-ordinary sequence (as `glibc` does below).

### `[package]`

| Key | Type | Meaning |
| --- | --- | --- |
| `deps` | array of strings | Runtime dependencies of the installed package. |

Runtime dependencies must be declared explicitly; they are recorded in the
package metadata and the local database and are used to keep installs and
removals consistent.

### `[reproducibility]`

| Key | Type | Meaning |
| --- | --- | --- |
| `status` | string | `verified` or `unverified`. |
| `reason` | string | Required when `status = "unverified"`; explains why. |

See [Reproducibility status](#reproducibility-status) below.

## Build environment variables

During the build phase, `salt build` provides these environment variables to the
recipe script (and to the standard build systems):

| Variable | Meaning |
| --- | --- |
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
system = "make"
deps = ["gcc", "make"]

[package]
deps = ["glibc"]

[reproducibility]
status = "verified"
```

### Example: a `cmake` recipe

```toml
name = "example-tool"
version = "2.4.0"
release = 1
summary = "Example CMake-built utility"
license = "MIT"
arch = ["x86_64", "aarch64"]

[source]
url = "https://example.org/releases/example-tool-2.4.0.tar.xz"
sha256 = "0000000000000000000000000000000000000000000000000000000000000000"

[build]
system = "cmake"
deps = ["gcc", "cmake", "ninja"]

[package]
deps = ["glibc"]

[reproducibility]
status = "verified"
```

### Worked example: `glibc` (autotools with a custom script)

The real `recipes/glibc/recipe.toml` uses `system = "autotools"` together with an
explicit `script`, because glibc needs an out-of-tree build and specific
configure flags. It shows how `SALT_JOBS` and `SALT_DEST` are used:

```toml
name = "glibc"
version = "2.39"
release = 1
summary = "GNU C Library"
license = "LGPL-2.1-or-later"
arch = ["x86_64", "aarch64"]

[source]
url = "https://ftp.gnu.org/gnu/glibc/glibc-2.39.tar.xz"
sha256 = "f77bd47cf8170c57365ae7f8e575d6a1bdde2da767bcb0c34a0e9d8bde5cc41a"

[build]
system = "autotools"
deps = ["gcc", "make", "binutils", "python", "bison"]
script = """
#!/bin/sh
mkdir -p build
cd build
../configure --prefix=/usr \\
    --disable-werror \\
    --enable-kernel=4.19 \\
    --enable-stack-protector=strong \\
    libc_cv_slibdir=/usr/lib
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
"""

[package]
deps = []

[reproducibility]
status = "verified"
```

Note how the script starts in `SALT_SRC` (the extracted source), creates an
out-of-tree `build/` directory, builds with `make -j"$SALT_JOBS"`, and installs
into `DESTDIR="$SALT_DEST"`.

## Requirements

Every recipe must, at minimum:

- pin the source URL,
- pin the source `sha256`,
- declare the `license`,
- declare build dependencies (`[build].deps`),
- declare runtime dependencies (`[package].deps`),
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

  ```toml
  [reproducibility]
  status = "verified"
  ```

- `status = "unverified"` — the package is not yet reproducible. A `reason` is
  required so the gap is explicit and reviewable.

  ```toml
  [reproducibility]
  status = "unverified"
  reason = "Chromium-derived browser build currently not bit-for-bit reproducible"
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
