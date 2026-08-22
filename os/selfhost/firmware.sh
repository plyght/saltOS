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
# (iwlwifi + ibt bluetooth), NVIDIA Quadro M500M (nouveau), Realtek HDA.
# All iwlwifi generations are kept rather than just 8000C, because the machine
# has no Ethernet port and guessing the exact card wrong would leave it with no
# network at all.
PATTERNS=(
  "/regulatory.db"
  "/regulatory.db.p7s"
  "/iwlwifi-*"
  "/i915/"
  "/nvidia/"
  "/intel/ibt-*"
  "/intel-ucode/"
  "/edid/"
)

if [ -n "$FWCACHE" ] && [ -d "$FWCACHE/lib/firmware" ]; then
  echo "using cached firmware tree from $FWCACHE"
  mkdir -p "$ROOTFS/lib"
  cp -a "$FWCACHE/lib/firmware" "$ROOTFS/lib/"
else
  rm -rf "$SRC"
  git clone --depth 1 --filter=blob:none --sparse --branch "$FWREF" "$FWREPO" "$SRC" \
    || git clone --depth 1 --filter=blob:none --sparse "$FWREPO" "$SRC"
  ( cd "$SRC"
    git sparse-checkout init --no-cone
    printf '%s\n' "${PATTERNS[@]}" | git sparse-checkout set --stdin
    git checkout )

  mkdir -p "$DEST"
  # move rather than copy: the kernel build tree is still on disk at this point
  # and a second full copy of linux-firmware is enough to fill the runner.
  ( cd "$SRC"
    find . -mindepth 1 -maxdepth 1 -name '.git*' -prune -o -print0 \
      | xargs -0 -I{} mv {} "$DEST/" )

  rm -rf "$DEST/.git" "$DEST/.gitignore" "$DEST/.gitattributes"
  rm -rf "$SRC"

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
for f in regulatory.db.xz i915 nvidia intel-ucode; do
  if [ -e "$DEST/$f" ]; then echo "  present: $f"; else echo "  MISSING: $f"; fi
done
echo "  iwlwifi blobs: $(find "$DEST" -maxdepth 1 -name 'iwlwifi-*' | wc -l)"
echo "  iwlwifi-8000C (AC 8260): $(find "$DEST" -maxdepth 1 -name 'iwlwifi-8000C-*' | wc -l) files"
find "$DEST" -maxdepth 1 -name 'iwlwifi-8000C-*' | grep -q . \
  || echo "  WARNING: no iwlwifi-8000C firmware; an AC 8260 will not associate"
[ -e "$DEST/regulatory.db.xz" ] || { echo "FATAL: regulatory.db missing, wifi regdomain will fail"; exit 1; }
