#!/bin/sh
# Build the saltOS BASE as signed .grain packages so OTA (`salt update`) can
# replace the base in place -- not just stratum apps. Today this packages:
#   - salt            : the package manager / stratum runtime binary
#   - saltos-base     : the runit services + console/login config (the layer
#                       build-arm64.sh bakes into the rootfs)
#   - linux-saltos    : the kernel Image (optional; pass KERNEL=)
#
# It produces a signed repo at $OUT/$ARCH (index.toml + index.toml.sig +
# packages/), ready to serve over HTTP for OTA. The installed image registers
# these as installed at build time (see register-base-grains.sh) so the first
# `salt update` knows the current versions.
#
# Required: SALT (path to the salt binary), SEC_KEY (hex secret key for signing).
# Optional: ARCH, VERSION, OUT, BASE_SRC (dir tree for saltos-base), KERNEL.
set -eu

SALT="${SALT:?set SALT to the salt binary}"
SEC_KEY="${SEC_KEY:?set SEC_KEY to the signing secret key (hex)}"
ARCH="${ARCH:-$("$SALT" --version | sed -E 's/.*\((.*)\)/\1/')}"
VERSION="${VERSION:-0.1.0}"
OUT="${OUT:-$PWD/base-grains}"
WORK="${WORK:-$PWD/base-grains-work}"

rm -rf "$WORK"; mkdir -p "$WORK" "$OUT"
export SALT_OUT="$WORK/out"
export SALT_WORK="$WORK/build"
mkdir -p "$SALT_OUT" "$SALT_WORK"

# --- salt grain ------------------------------------------------------------
stage="$WORK/stage-salt"; mkdir -p "$stage"
cp "$SALT" "$stage/salt"
cat > "$WORK/salt.recipe.toml" <<EOF
name = "salt"
version = "$VERSION"
release = 1
summary = "saltOS package manager and stratum runtime"
license = "MIT"
arch = ["x86_64", "aarch64"]
[source]
url = "file://$stage"
sha256 = ""
[build]
system = "custom"
script = """
mkdir -p "\$SALT_DEST/usr/bin"
cp "\$SALT_SRC/salt" "\$SALT_DEST/usr/bin/salt"
chmod 0755 "\$SALT_DEST/usr/bin/salt"
"""
[package]
EOF
"$SALT" build "$WORK/salt.recipe.toml"

# --- saltos-base grain (config layer) -------------------------------------
if [ -n "${BASE_SRC:-}" ] && [ -d "$BASE_SRC" ]; then
  cat > "$WORK/saltos-base.recipe.toml" <<EOF
name = "saltos-base"
version = "$VERSION"
release = 1
summary = "saltOS base config: runit services, console login, defaults"
license = "MIT"
arch = ["x86_64", "aarch64"]
[source]
url = "file://$BASE_SRC"
sha256 = ""
[build]
system = "custom"
script = """
cp -a "\$SALT_SRC/." "\$SALT_DEST/"
"""
[package]
EOF
  "$SALT" build "$WORK/saltos-base.recipe.toml"
fi

# --- kernel grain (optional) ----------------------------------------------
if [ -n "${KERNEL:-}" ] && [ -f "$KERNEL" ]; then
  kstage="$WORK/stage-kernel"; mkdir -p "$kstage/boot"
  cp "$KERNEL" "$kstage/boot/$(basename "$KERNEL")"
  cat > "$WORK/linux-saltos.recipe.toml" <<EOF
name = "linux-saltos"
version = "$VERSION"
release = 1
summary = "saltOS Linux kernel image"
license = "GPL-2.0"
arch = ["x86_64", "aarch64"]
[source]
url = "file://$kstage"
sha256 = ""
[build]
system = "custom"
script = """
mkdir -p "\$SALT_DEST/boot"
cp -a "\$SALT_SRC/boot/." "\$SALT_DEST/boot/"
"""
[package]
EOF
  "$SALT" build "$WORK/linux-saltos.recipe.toml"
fi

# --- publish a signed repo -------------------------------------------------
mkdir -p "$OUT/$ARCH/packages"
cp "$SALT_OUT/$ARCH/packages"/*.grain "$OUT/$ARCH/packages/" 2>/dev/null || \
  cp "$SALT_OUT"/*.grain "$OUT/$ARCH/packages/" 2>/dev/null || true
"$SALT" --key "$SEC_KEY" repo publish "$OUT/$ARCH"
echo "wrote signed base-grain repo: $OUT/$ARCH"
ls -la "$OUT/$ARCH" "$OUT/$ARCH/packages"
