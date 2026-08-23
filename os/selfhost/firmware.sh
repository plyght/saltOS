#!/bin/bash
set -euo pipefail

FWREPO="${FWREPO:-https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git}"
FWREF="${FWREF:-20250211}"
WORK="${WORK:?set WORK}"
ROOTFS="${ROOTFS:?set ROOTFS}"
FWCACHE="${FWCACHE:-}"

SRC="$WORK/linux-firmware"
DEST="$ROOTFS/lib/firmware"

# Scoped to the ThinkPad P40 Yoga: Skylake i915, Intel Wireless-AC 8260
# (iwlwifi + ibt bluetooth), NVIDIA Quadro M500M (nouveau).
# The iwlwifi blobs are top-level files and arrive via cone mode. regulatory.db
# is NOT part of linux-firmware; it ships in wireless-regdb and is fetched
# separately below. Intel CPU microcode is not in linux-firmware either (only
# amd-ucode is), so this image relies on the BIOS-supplied microcode.
FW_DIRS=(
  i915
  nvidia
  intel
)

REGDB_VER="${REGDB_VER:-2024.10.07}"

if [ -n "$FWCACHE" ] && [ -d "$FWCACHE/lib/firmware" ]; then
  echo "using cached firmware tree from $FWCACHE"
  mkdir -p "$ROOTFS/lib"
  cp -a "$FWCACHE/lib/firmware" "$ROOTFS/lib/"
else
  rm -rf "$SRC"
  git clone --depth 1 --filter=blob:none --sparse --branch "$FWREF" "$FWREPO" "$SRC" \
    || git clone --depth 1 --filter=blob:none --sparse "$FWREPO" "$SRC"
  # Cone mode: `git sparse-checkout set` uses it by default and overrides an
  # earlier `init --no-cone`, so only whole directories can be selected. Cone
  # mode also materialises every top-level file, which is where the iwlwifi
  # blobs live, so those arrive without needing a pattern.
  ( cd "$SRC"
    git sparse-checkout set --cone "${FW_DIRS[@]}"
    git checkout )

  # intel/ carries sof, ipu3, vsc and more that this machine cannot use. Keep
  # only the Bluetooth blobs for the AC 8260's companion radio.
  if [ -d "$SRC/intel" ]; then
    find "$SRC/intel" -mindepth 1 -maxdepth 1 ! -name 'ibt-*' -exec rm -rf {} + 2>/dev/null || true
  fi

  mkdir -p "$DEST"
  # move rather than copy: the kernel build tree is still on disk at this point
  # and a second full copy of linux-firmware is enough to fill the runner.
  ( cd "$SRC"
    find . -mindepth 1 -maxdepth 1 -name '.git*' -prune -o -print0 \
      | xargs -0 -I{} mv {} "$DEST/" )

  rm -rf "$DEST/.git" "$DEST/.gitignore" "$DEST/.gitattributes"
  rm -rf "$SRC"

  echo "===== wireless-regdb (regulatory.db) ====="
  regdb_tar="$WORK/wireless-regdb-${REGDB_VER}.tar.xz"
  n=0
  while [ "$n" -lt 5 ]; do
    curl -fsSL --http1.1 --retry 3 --retry-delay 5 --connect-timeout 30 \
      "https://www.kernel.org/pub/software/network/wireless-regdb/wireless-regdb-${REGDB_VER}.tar.xz" \
      -o "$regdb_tar" && break
    n=$((n + 1)); rm -f "$regdb_tar"; sleep 5
  done
  [ -f "$regdb_tar" ] || { echo "FATAL: could not fetch wireless-regdb"; exit 1; }
  ( cd "$WORK" && tar -xf "$regdb_tar" )
  install -Dm644 "$WORK/wireless-regdb-${REGDB_VER}/regulatory.db" "$DEST/regulatory.db"
  install -Dm644 "$WORK/wireless-regdb-${REGDB_VER}/regulatory.db.p7s" "$DEST/regulatory.db.p7s"
  rm -rf "$WORK/wireless-regdb-${REGDB_VER}" "$regdb_tar"

  echo "===== compressing firmware (xz, kernel-compatible dict) ====="
  find "$DEST" -type f ! -name '*.xz' -print0 \
    | xargs -0 -r -P "$(nproc)" -n 64 xz --quiet --check=crc32 --lzma2=dict=1MiB --force

  find "$DEST" -type l | while read -r link; do
    tgt="$(readlink "$link")"
    case "$tgt" in
      *.xz) continue ;;
    esac
    ln -sf "${tgt}.xz" "${link}.xz"
    rm -f "$link"
  done

  if [ -n "$FWCACHE" ]; then
    mkdir -p "$FWCACHE/lib"
    rm -rf "$FWCACHE/lib/firmware"
    cp -a "$DEST" "$FWCACHE/lib/firmware"
  fi
fi

mkdir -p "$ROOTFS/usr/lib"
[ -e "$ROOTFS/usr/lib/firmware" ] || ln -sf /lib/firmware "$ROOTFS/usr/lib/firmware"

echo "===== firmware installed ====="
du -sh "$DEST"
for f in regulatory.db.xz regulatory.db.p7s.xz i915 nvidia intel; do
  if [ -e "$DEST/$f" ]; then echo "  present: $f"; else echo "  MISSING: $f"; fi
done
echo "  intel bluetooth blobs: $(find "$DEST/intel" -maxdepth 1 -name 'ibt-*' 2>/dev/null | wc -l)"
echo "  iwlwifi blobs: $(find "$DEST" -maxdepth 1 -name 'iwlwifi-*' | wc -l)"
echo "  iwlwifi-8000C (AC 8260): $(find "$DEST" -maxdepth 1 -name 'iwlwifi-8000C-*' | wc -l) files"
find "$DEST" -maxdepth 1 -name 'iwlwifi-8000C-*' | grep -q . \
  || echo "  WARNING: no iwlwifi-8000C firmware; an AC 8260 will not associate"
[ -e "$DEST/regulatory.db.xz" ] || [ -e "$DEST/regulatory.db" ] \
  || { echo "FATAL: regulatory.db missing, wifi regdomain will fail"; exit 1; }
find "$DEST" -maxdepth 1 -name 'iwlwifi-8000C-*' | grep -q . \
  || { echo "FATAL: no iwlwifi-8000C firmware; the AC 8260 will not associate"; exit 1; }
