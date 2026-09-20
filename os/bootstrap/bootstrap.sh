#!/bin/sh
set -eu

ARCH="${ARCH:-x86_64}"
OUT="${OUT:-/var/tmp/saltos-build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SALT="${SALT:-salt}"
SALT="$(command -v "$SALT")" || { echo "salt not found in PATH" >&2; exit 2; }

case "$ARCH" in
  x86_64|aarch64) ;;
  arm64) ARCH=aarch64 ;;
  amd64) ARCH=x86_64 ;;
  *) echo "unsupported ARCH: $ARCH" >&2; exit 2 ;;
esac

ORDER="$REPO_ROOT/os/bootstrap/build-order.toml"
WORK="$OUT/$ARCH"
TOOLS="$OUT/tools"
SYSROOT="$WORK/sysroot"
LOGDIR="$WORK/logs"

mkdir -p "$SYSROOT" "$LOGDIR"

stage_repo() {
  case "$1" in
    cross-toolchain|temp-tools) REPO_OUT="$TOOLS" ;;
    *) REPO_OUT="$OUT" ;;
  esac
  REPO_DIR="$REPO_OUT/$ARCH"
  PKGDIR="$REPO_DIR/packages"
  mkdir -p "$PKGDIR"
}

log() { printf '[bootstrap %s] %s\n' "$ARCH" "$*"; }

stage_packages() {
  stage="$1"
  awk -v stage="[$stage]" '
    $0 == stage { inblock=1; next }
    /^\[/ { inblock=0 }
    inblock && /"/ {
      line=$0
      gsub(/[",]/, "", line)
      gsub(/^[ \t]+|[ \t]+$/, "", line)
      if (line != "packages = [" && line != "" && line != "]")
        print line
    }
  ' "$ORDER"
}

build_one() {
  name="$1"
  recipe="$REPO_ROOT/recipes/$name"
  if [ ! -d "$recipe" ]; then
    echo "missing recipe: $name" >&2
    return 1
  fi
  version=$(sed -n 's/^version = "\(.*\)"$/\1/p' "$recipe/recipe.toml" | head -n1)
  release=$(sed -n 's/^release = \([0-9][0-9]*\)$/\1/p' "$recipe/recipe.toml" | head -n1)
  grain="$PKGDIR/$name-$version-${release:-1}-$ARCH.grain"
  if [ -f "$grain" ]; then
    log "reusing $name ($grain)"
  else
    log "building $name"
    rm -f "$PKGDIR/$name"-*-"$ARCH".grain
    if ! SALT_ARCH="$ARCH" SALT_JOBS="$JOBS" SALT_OUT="$REPO_OUT" FORCE_UNSAFE_CONFIGURE=1 \
        "$SALT" build "$recipe" >"$LOGDIR/$name.log" 2>&1; then
      echo "build failed for $name; log follows:" >&2
      cat "$LOGDIR/$name.log" >&2 || true
      return 1
    fi
  fi
  if [ ! -f "$grain" ]; then
    echo "no grain produced for $name; build log follows:" >&2
    cat "$LOGDIR/$name.log" >&2 2>/dev/null || true
    return 1
  fi
  log "publishing local index"
  if ! "$SALT" repo publish "$REPO_DIR" >>"$LOGDIR/$name.log" 2>&1; then
    echo "repo publish failed for $name; log follows:" >&2
    cat "$LOGDIR/$name.log" >&2 || true
    return 1
  fi
  if ! "$SALT" --root "$SYSROOT" --repo "$REPO_OUT" --yes sync >>"$LOGDIR/$name.log" 2>&1; then
    echo "sync failed for $name; log follows:" >&2
    cat "$LOGDIR/$name.log" >&2 || true
    return 1
  fi
  log "installing $name into sysroot"
  if ! "$SALT" --root "$SYSROOT" --repo "$REPO_OUT" --yes install --nodeps "$name" >>"$LOGDIR/$name.log" 2>&1; then
    echo "install failed for $name; log follows:" >&2
    cat "$LOGDIR/$name.log" >&2 || true
    return 1
  fi
}

sysroot_toolchain_env() {
  for tool in gcc g++ ar ranlib strip nm objcopy objdump; do
    if [ ! -x "$SYSROOT/usr/bin/$tool" ]; then
      echo "sysroot toolchain incomplete: missing $SYSROOT/usr/bin/$tool (run the cross-toolchain stage first)" >&2
      return 1
    fi
  done
  export CC="$SYSROOT/usr/bin/gcc --sysroot=$SYSROOT"
  export CXX="$SYSROOT/usr/bin/g++ --sysroot=$SYSROOT"
  export CPP="$SYSROOT/usr/bin/gcc --sysroot=$SYSROOT -E"
  export AR="$SYSROOT/usr/bin/ar"
  export RANLIB="$SYSROOT/usr/bin/ranlib"
  export STRIP="$SYSROOT/usr/bin/strip"
  export NM="$SYSROOT/usr/bin/nm"
  export OBJCOPY="$SYSROOT/usr/bin/objcopy"
  export OBJDUMP="$SYSROOT/usr/bin/objdump"
  export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
  export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
  export ACLOCAL_PATH="$SYSROOT/usr/share/aclocal"
}

CHROOT_MOUNTS=""

sysroot_chroot_umount() {
  for m in $CHROOT_MOUNTS; do
    umount "$m" 2>/dev/null || umount -l "$m" 2>/dev/null || true
  done
  CHROOT_MOUNTS=""
}

sysroot_chroot_env() {
  for tool in gcc g++ make bash sh; do
    if [ ! -x "$SYSROOT/usr/bin/$tool" ]; then
      echo "sysroot incomplete: missing $SYSROOT/usr/bin/$tool (run the temp-tools stage first)" >&2
      return 1
    fi
  done
  mkdir -p "$SYSROOT/usr/sbin" "$SYSROOT/dev" "$SYSROOT/proc" "$SYSROOT/sys" "$SYSROOT/run" \
    "$SYSROOT/tmp" "$SYSROOT/var/tmp" "$SYSROOT/root" "$SYSROOT/etc"
  chmod 1777 "$SYSROOT/tmp" "$SYSROOT/var/tmp"
  for d in bin sbin lib; do
    [ -e "$SYSROOT/$d" ] || ln -s "usr/$d" "$SYSROOT/$d"
  done
  [ -f "$SYSROOT/etc/passwd" ] || printf 'root:x:0:0:root:/root:/bin/bash\n' > "$SYSROOT/etc/passwd"
  [ -f "$SYSROOT/etc/group" ] || printf 'root:x:0:\n' > "$SYSROOT/etc/group"
  if [ -z "$CHROOT_MOUNTS" ]; then
    mount --bind /dev "$SYSROOT/dev"
    CHROOT_MOUNTS="$SYSROOT/dev"
    mount -t proc proc "$SYSROOT/proc"
    CHROOT_MOUNTS="$SYSROOT/proc $CHROOT_MOUNTS"
    mount -t sysfs sysfs "$SYSROOT/sys"
    CHROOT_MOUNTS="$SYSROOT/sys $CHROOT_MOUNTS"
    mount -t tmpfs tmpfs "$SYSROOT/run"
    CHROOT_MOUNTS="$SYSROOT/run $CHROOT_MOUNTS"
  fi
  unset CC CXX CPP AR RANLIB STRIP NM OBJCOPY OBJDUMP
  unset PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_LIBDIR PKG_CONFIG_PATH ACLOCAL_PATH
  unset CFLAGS CXXFLAGS LDFLAGS CPPFLAGS
  export PATH=/usr/bin:/usr/sbin:/bin:/sbin
  export HOME=/root
  export SALT_WORK="$SYSROOT/var/tmp/salt-build"
  export SALT_BUILD_ROOT="$SYSROOT"
}

trap sysroot_chroot_umount EXIT INT TERM

run_stage() {
  stage="$1"
  log "=== stage: $stage ==="
  stage_repo "$stage"
  case "$stage" in
    cross-toolchain) ;;
    temp-tools) sysroot_toolchain_env ;;
    *) sysroot_chroot_env ;;
  esac
  stage_packages "$stage" | while IFS= read -r pkg; do
    [ -n "$pkg" ] || continue
    build_one "$pkg"
  done
}

STAGES="${STAGES:-cross-toolchain temp-tools base desktop}"

log "repo root: $REPO_ROOT"
log "work dir:  $WORK"
log "jobs:      $JOBS"
log "stages:    $STAGES"

for stage in $STAGES; do
  case "$stage" in
    cross-toolchain|temp-tools) SALT_NO_NETWORK=0 run_stage "$stage" ;;
    *) SALT_NO_NETWORK=1 run_stage "$stage" ;;
  esac
done

log "base rootfs assembled under $SYSROOT"
log "package outputs under $WORK/packages"
log "bootstrap tool packages under $TOOLS/$ARCH/packages"
