#!/bin/bash
# Boot a saltOS Apple-Silicon disk image on macOS without UTM.
#
# UTM's "Apple Virtualization" backend is just a GUI over Apple's
# Virtualization.framework. vfkit (https://github.com/crc-org/vfkit) is a small,
# signed CLI over the *same* framework -- same VZEFIBootLoader, same virtio-blk /
# virtio-gpu / virtio console -- so booting here reproduces a UTM Apple-Virt boot
# exactly, but scriptably and over SSH. This is the no-UTM way to build, run, and
# smoke-test the image vm-apple.sh produces.
#
# Modes:
#   --check        headless boot; exit 0 once the saltOS login banner appears on
#                  the serial console (hvc0), 1 on timeout. For CI / "does it boot".
#   (default)      headless boot; stream the serial console live. Ctrl-C stops.
#   --gui          open a graphical window with keyboard/mouse (virtio-gpu),
#                  exactly like UTM's Apple-Virt display. The interactive path.
#
# The image is booted on an APFS copy-on-write clone so the source stays pristine
# (override with --in-place). vfkit is fetched to a cache dir if not on PATH.
#
# Usage:
#   os/build/vm-run.sh [--check|--gui|--in-place] [image.img]
#   CPUS=4 MEM_MIB=4096 TIMEOUT=120 os/build/vm-run.sh --check out-vm/saltos-0.1.0-apple-aarch64.img
set -euo pipefail

VFKIT_VERSION="${VFKIT_VERSION:-v0.6.3}"
CPUS="${CPUS:-2}"
MEM_MIB="${MEM_MIB:-2048}"
TIMEOUT="${TIMEOUT:-90}"
CACHE="${SALTOS_CACHE:-$HOME/.cache/saltos}"

MODE=stream
INPLACE=0
IMG=""
for a in "$@"; do
  case "$a" in
    --check|--smoke) MODE=check ;;
    --gui)           MODE=gui ;;
    --in-place)      INPLACE=1 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    -*) echo "unknown flag: $a" >&2; exit 2 ;;
    *) IMG="$a" ;;
  esac
done

[ "$(uname -s)" = "Darwin" ] || { echo "vm-run.sh needs macOS (Apple Virtualization.framework)" >&2; exit 1; }

# Locate an image if none was given: newest apple .img under ./ , out-vm/, ~/.
if [ -z "$IMG" ]; then
  IMG="$(ls -t ./*apple*.img out-vm/*apple*.img "$HOME"/*apple*.img 2>/dev/null | head -n1 || true)"
fi
[ -n "$IMG" ] && [ -f "$IMG" ] || { echo "no image found; pass one: vm-run.sh path/to/saltos-*-apple-aarch64.img" >&2; exit 1; }
IMG="$(cd "$(dirname "$IMG")" && pwd)/$(basename "$IMG")"

# Resolve (or fetch) vfkit.
VFKIT="${VFKIT:-$(command -v vfkit || true)}"
if [ -z "$VFKIT" ] || [ ! -x "$VFKIT" ]; then
  VFKIT="$CACHE/vfkit-$VFKIT_VERSION"
  if [ ! -x "$VFKIT" ]; then
    echo "==> fetching vfkit $VFKIT_VERSION -> $VFKIT"
    mkdir -p "$CACHE"
    curl -fsSL -o "$VFKIT" \
      "https://github.com/crc-org/vfkit/releases/download/$VFKIT_VERSION/vfkit"
    chmod +x "$VFKIT"
    xattr -dr com.apple.quarantine "$VFKIT" 2>/dev/null || true
  fi
fi

# Boot a pristine clone unless asked to use the image in place. cp -c is an APFS
# copy-on-write clone (instant, no extra space until written).
WORK="$(mktemp -d "${TMPDIR:-/tmp}/saltos-vm-run.XXXXXX")"
NVRAM="$WORK/efi-vars.nvram"
CONSOLE="$WORK/console.log"
if [ "$INPLACE" = 1 ]; then
  DISK="$IMG"
else
  DISK="$WORK/disk.img"
  echo "==> cloning $IMG -> $DISK (copy-on-write)"
  cp -c "$IMG" "$DISK" 2>/dev/null || cp "$IMG" "$DISK"
fi

VM_PID=""
cleanup() { [ -n "$VM_PID" ] && kill "$VM_PID" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

common_args=(
  --cpus "$CPUS" --memory "$MEM_MIB"
  --bootloader "efi,variable-store=$NVRAM,create"
  --device "virtio-blk,path=$DISK"
  --device virtio-rng
  --device "virtio-net,nat"
)

echo "==> booting $(basename "$DISK") via Apple Virtualization.framework (vfkit $VFKIT_VERSION)"
case "$MODE" in
  gui)
    # Graphical, interactive -- the UTM-equivalent display.
    exec "$VFKIT" "${common_args[@]}" \
      --device virtio-gpu \
      --device virtio-input,keyboard \
      --device virtio-input,pointing \
      --device "virtio-serial,logFilePath=$CONSOLE" \
      --gui
    ;;
  check|stream)
    "$VFKIT" "${common_args[@]}" \
      --device "virtio-serial,logFilePath=$CONSOLE" >/dev/null 2>&1 &
    VM_PID=$!
    : > "$CONSOLE"
    if [ "$MODE" = stream ]; then
      echo "==> serial console (Ctrl-C to stop):"
      tail -f "$CONSOLE" &
      TAIL_PID=$!
      wait "$VM_PID" 2>/dev/null || true
      kill "$TAIL_PID" 2>/dev/null || true
      exit 0
    fi
    # check: wait for the saltOS login banner (userspace reached = boot OK).
    echo "==> waiting up to ${TIMEOUT}s for the saltOS login banner..."
    deadline=$(( $(date +%s) + TIMEOUT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
      if ! kill -0 "$VM_PID" 2>/dev/null; then
        echo "FAIL: vfkit exited before the system booted" >&2
        tail -n 40 "$CONSOLE" >&2 || true
        exit 1
      fi
      if grep -aqiE 'saltos.* login:|\bsalt@saltos' "$CONSOLE" 2>/dev/null; then
        echo "PASS: saltOS reached userspace (login banner on hvc0)"
        exit 0
      fi
      sleep 2
    done
    echo "FAIL: no login banner within ${TIMEOUT}s" >&2
    tail -n 40 "$CONSOLE" >&2 || true
    exit 1
    ;;
esac
