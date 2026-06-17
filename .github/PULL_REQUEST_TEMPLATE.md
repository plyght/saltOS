# Pull request

## Summary

Describe what this change does and why.

## Type of change

- [ ] Package recipe (add or update `recipes/<name>/recipe.toml`)
- [ ] salt / libsalt code
- [ ] OS base, runit, btrfs, installer, or ISO
- [ ] Documentation only

## Trust and contribution model

saltOS uses an explicit trust model (see `docs/trust-model.md`). Recipe and code
changes are merged only after review by a trusted maintainer. There is no
drive-by package ownership takeover.

- [ ] I have read `docs/contributing.md` and `docs/trust-model.md`.
- [ ] I am not changing the maintainer or ownership of an existing package, or I
      have flagged that change explicitly for maintainer review.

## Recipe changes (if applicable)

- [ ] Source URL is pinned
- [ ] Source `sha256` is pinned
- [ ] `license` is declared
- [ ] Build dependencies are declared
- [ ] Runtime dependencies are declared
- [ ] Both `x86_64` and `aarch64` are declared in `arch` (unless genuinely arch-specific)
- [ ] `reproducibility.status` is set (`verified`, or `unverified` with a `reason`)
- [ ] No install-time network access or arbitrary maintainer scripts
- [ ] `salt lint <recipe-dir>` passes locally
- [ ] `salt trust scan <recipe-dir>` reports no blocking findings

## Code changes (if applicable)

- [ ] `cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release` configures cleanly
- [ ] `cmake --build build` builds with no new warnings
- [ ] `ctest --test-dir build --output-on-failure` passes
- [ ] No new code comments, banners, headers, license text, or attribution prose

## CI

- [ ] `ci`, `lint`, and (for recipe changes) `packages` workflows are expected to pass
