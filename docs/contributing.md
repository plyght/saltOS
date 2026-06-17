# Contributing to saltOS

saltOS is a small, curated, personal-first distribution. Contributions are
welcome, but they flow through an explicit trust model and a set of CI gates
rather than through drive-by merges. This document explains the workflow.

## Read first

Before contributing, read:

- **[CONVENTIONS.md](CONVENTIONS.md)** — the authoritative contract for formats,
  paths, and command surfaces. If anything here disagrees with CONVENTIONS, the
  conventions win.
- **[trust-model.md](trust-model.md)** — how contributor trust levels and
  package admission work.

See also: [recipes.md](recipes.md), [repository.md](repository.md).

## Hard project rules

These are non-negotiable and apply to every change:

- **No new code comments, banners, file headers, license text, or attribution
  prose** in code or scripts. (Markdown documentation is prose and is fine.)
  YAML workflow files carry only the keys they need.
- **Native tooling only.** Build with CMake + Ninja; use each language's native
  tools. No npm/yarn-style package managers.
- **Permissive licenses only** for dependencies, and **no telemetry**.
- **No secrets or credentials** committed anywhere. Signing secret keys are
  git-ignored; only public keys are committed.
- **Both `x86_64` and `aarch64` must be supported.** Recipes declare both in
  `arch` unless a package is genuinely arch-specific, and code must build and
  pass tests on both.

## Proposing a package

The official repository is curated and intentionally small; there is no
untrusted user repository. To propose a new package, open a **package proposal**
issue (use the package proposal issue template). Your proposal should satisfy the
admission checklist, which mirrors the admission rules in
[trust-model.md](trust-model.md):

- source URL is pinned;
- source hash is pinned;
- license is declared;
- build dependencies are declared;
- runtime dependencies are declared;
- the package contents are inspectable;
- the build succeeds in a clean environment;
- a maintainer or trusted reviewer approves it.

A maintainer will review the proposal before any recipe is added. See
[recipes.md](recipes.md) for the recipe schema.

## Local workflow

Build and test the tooling:

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

For recipe work, lint, scan, and build before opening a PR:

```sh
salt lint recipes/<name>
salt trust scan recipes/<name>
salt build recipes/<name>
```

`salt lint` checks that the recipe is well-formed; `salt trust scan` runs the
supply-chain risk checks; `salt build` confirms the package actually builds.

## CI gates

Every pull request must pass the relevant CI workflows. aarch64 jobs run on the
`ubuntu-24.04-arm` runner; x86_64 jobs run on `ubuntu-latest`.

| Workflow | Trigger | What it verifies |
| --- | --- | --- |
| **ci** | push / PR | Matrix build + `ctest` over **`x86_64`** and **`aarch64`** — the code configures, builds, and passes tests on both arches. |
| **lint** | PRs touching `recipes/**` | `salt lint` + supply-chain scan on the changed recipes. |
| **packages** | pushes touching recipes | Builds packages and a signed repository index. |
| **iso** | `workflow_dispatch` and tags | Builds the live ISOs for both arches (image steps may be best-effort on free runners). |

A change that does not pass its applicable gates will not be merged.

## Review workflow and trust

Review is tied to contributor trust levels (`unknown`, `vouched`, `maintainer`,
`denounced`; see [trust-model.md](trust-model.md)):

- **Unknown** contributors may open issues and proposals.
- **Recipe and code changes require review and approval from a trusted
  maintainer.** Automated checks can warn or block, but they are never the sole
  basis for trust.
- There is **no drive-by package ownership takeover** — a change that alters the
  maintainer or ownership of an existing package is flagged for explicit
  maintainer review.
- **Decisions are logged.** Trust changes record who made them and why.

## The pull request checklist

The PR template walks you through the gates. In short:

- **Recipe PRs** confirm pinned source URL and `sha256`, declared license and
  build/runtime deps, both `x86_64` and `aarch64` in `arch` (unless genuinely
  arch-specific), a set `reproducibility.status`, no install-time network access
  or arbitrary maintainer scripts, and that `salt lint` and `salt trust scan`
  pass locally.
- **Code PRs** confirm a clean CMake configure, a warning-free build, passing
  `ctest`, and **no new code comments, banners, headers, license text, or
  attribution prose**.

Each checklist item maps to one of the CI gates above, so a complete checklist
should mean a green pipeline.
