# Handoff: native-base + selfhost-desktop track

Owned: `.github/workflows/native-base.yml`, `.github/workflows/selfhost-desktop.yml`,
`os/bootstrap/*`, `os/selfhost/desktop.sh`, `recipes/*/recipe.toml`, and the recipe
builder path in `src/salt/cmd_build.cpp`.

## Done (all on main)

- `7a5fcb6` and ancestors (44 commits): glibc 2.39 -> 2.41 (real upstream sha256),
  libxcrypt added for `libcrypt.so.1`, gperf added for eudev, every remote recipe has
  a real 64-hex sha256 and the builder rejects anything else, bootstrap separates the
  toolchain/temp-tools repo (`$OUT/tools`) from the base repo (`$OUT`), stages packages
  in `os/bootstrap/build-order.toml` order with publish -> sync -> `install --nodeps`
  per package, runs base builds chrooted in the sysroot (`SALT_BUILD_ROOT`), prunes
  build trees / old generations / cache after each package and stage, fixes for
  binutils (MAKEINFO=true), gcc (/usr/bin/cc, libdir /usr/lib), libffi, python, perl,
  cmake, meson, dbus/usbutils (autotools), util-linux, kmod, iputils, runit
  (`/usr/sbin/runit`, `/usr/sbin/init`), ncurses, linux (modules in /usr/lib/modules),
  busybox; `stage-rootfs.sh` creates merged-usr links before installing; `build-iso.sh`
  + `live-init` boot the native rootfs and print `SALTOS_BOOT_OK`.
- `a816a95`: builder ignores `source.sha256` for `file://` local source dirs (tests use
  `TODO-sha256` fixtures) while still rejecting bad checksums for remote sources.
- `59f80f5`: clang-format.
- `fe1ce68`: builder deletes `usr/share/info/dir` from `$SALT_DEST` before packaging
  (CI has texinfo, so gmp and mpfr both shipped it -> `file conflict` from the resolver).
- `2f7fd10`: native-base grain cache key now also hashes `os/bootstrap/**`,
  `src/halite/**`, `src/salt/**` so builder changes invalidate cached grains.

- gcc recipe (release 5): dropped `--with-system-zlib`; the cross-stage gcc is built on
  the CI host and linked against the host libz, so `cc1` failed to load inside the base
  chroot (`libz.so.1: cannot open shared object file`, surfaced as glibc configure
  "cannot compute suffix of object files"). In-tree zlib is linked statically now.

- build-order: base-stage glibc now builds after python (its configure needs bison and
  python3, which the temp-tools sysroot lacks; everything before it links against the
  cross-stage glibc 2.41, same version/compiler).

- build-order: popt and rsync now precede linux (kernel `headers_install` needs rsync).

## Current CI state

- native-base green on main for `e9bdbbd` (toolchain, base, ISO assembly + QEMU boot):
  https://github.com/plyght/saltOS/actions/runs/35559388485
- selfhost-desktop green for `7a5fcb6`: https://github.com/plyght/saltOS/actions/runs/35547211980
  (workflow only triggers on `os/selfhost/desktop.sh` / its yml; re-run via
  `gh workflow run selfhost-desktop --repo plyght/saltOS` if needed).
- To reproduce a base-job failure locally: `gh run download <id> -n saltos-build-x86_64`,
  untar into `saltos-build`, run bootstrap.sh with `STAGES=base OUT=$PWD/saltos-build`.

## Verified locally

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && \
  ctest --test-dir build --output-on-failure           # 5/5 pass
export PATH="$PWD/build/src/salt:$PATH" SALT_ARCH=x86_64 OUT=$HOME/nb-out/saltos-build
sudo --preserve-env=PATH,SALT_ARCH,OUT STAGES="cross-toolchain temp-tools base" \
  os/bootstrap/bootstrap.sh                            # full from-source build, passes
sudo --preserve-env=PATH ROOTFS=$OUT/x86_64/rootfs OUT=$OUT ARCH=x86_64 STAGES=base \
  os/bootstrap/stage-rootfs.sh
sudo os/iso/build-iso.sh --arch x86_64 --rootfs $OUT/x86_64/rootfs --installer salt-setup \
  --salt-setup $PWD/build/src/setup/salt-setup --out $HOME/nb-out/out/saltos-native-base-x86_64.iso
qemu-system-x86_64 -m 2048 -cdrom $HOME/nb-out/out/saltos-native-base-x86_64.iso \
  -nographic -serial mon:stdio  # prints SALTOS_BOOT_OK
```

Selfhost desktop ISO (`os/selfhost/desktop.sh`) boots under QEMU `-vga std` and the
serial log prints `SALTOS_X_OK xorg + twm + xterm running, all from source, no Debian`.

## Left / known gaps

- Local host has no `makeinfo`, so info-page related conflicts only appear in CI.
- `salt build` now strips ELF payloads and turns hardlinks into symlinks (the rootfs
  was ~6 GB unstripped, git-core alone 2.6 GB from hardlinked builtins).
- selfhost-desktop proves Xorg + twm + xterm, not the LXQt + SDDM target stack.
- Lint's clang-tidy needs clang >= 17 (`-std=gnu++23`); Ubuntu 24.04 default clang-tidy
  cannot run it locally, rely on the lint workflow.
