#!/bin/sh
set -eu

ARCH="${ARCH:-x86_64}"
OUT="${OUT:-/var/tmp/saltos-build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SALT="${SALT:-salt}"

case "$ARCH" in
  x86_64|aarch64) ;;
  arm64) ARCH=aarch64 ;;
  amd64) ARCH=x86_64 ;;
  *) echo "unsupported ARCH: $ARCH" >&2; exit 2 ;;
esac

ORDER="$REPO_ROOT/os/bootstrap/build-order.toml"
WORK="$OUT/$ARCH"
PKGDIR="$WORK/packages"
SYSROOT="$WORK/sysroot"
LOGDIR="$WORK/logs"

mkdir -p "$PKGDIR" "$SYSROOT" "$LOGDIR"

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
  out="$PKGDIR/$name-$ARCH.grain"
  if [ ! -d "$recipe" ]; then
    echo "missing recipe: $name" >&2
    return 1
  fi
  log "building $name"
  SALT_ARCH="$ARCH" SALT_JOBS="$JOBS" \
    "$SALT" build "$recipe" \
      --arch "$ARCH" \
      --jobs "$JOBS" \
      --out "$PKGDIR" \
      --sysroot "$SYSROOT" \
      >"$LOGDIR/$name.log" 2>&1
  log "installing $name into sysroot"
  "$SALT" install "$out" --root "$SYSROOT" --yes \
    >>"$LOGDIR/$name.log" 2>&1 || \
  "$SALT" install "$name" --root "$SYSROOT" --repo "$PKGDIR" --yes \
    >>"$LOGDIR/$name.log" 2>&1
}

run_stage() {
  stage="$1"
  log "=== stage: $stage ==="
  stage_packages "$stage" | while IFS= read -r pkg; do
    [ -n "$pkg" ] || continue
    build_one "$pkg"
  done
}

log "repo root: $REPO_ROOT"
log "work dir:  $WORK"
log "jobs:      $JOBS"

SALT_NO_NETWORK=0 run_stage cross-toolchain
SALT_NO_NETWORK=0 run_stage temp-tools

export SALT_NO_NETWORK=1
run_stage base
run_stage desktop

log "base rootfs assembled under $SYSROOT"
log "package outputs under $PKGDIR"
