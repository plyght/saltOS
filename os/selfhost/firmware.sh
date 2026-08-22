#!/bin/bash
set -euo pipefail

FWREPO="${FWREPO:-https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git}"
FWREF="${FWREF:-20250211}"
WORK="${WORK:?set WORK}"
ROOTFS="${ROOTFS:?set ROOTFS}"
FWCACHE="${FWCACHE:-}"

SRC="$WORK/linux-firmware"
DEST="$ROOTFS/lib/firmware"

PATTERNS=(
  "/regulatory.db"
  "/regulatory.db.p7s"
  "/iwlwifi-*"
  "/i915/"
  "/amdgpu/"
  "/ath9k_htc/"
  "/ath10k/"
  "/ath11k/"
  "/ath12k/"
  "/rtw88/"
  "/rtw89/"
  "/rtlwifi/"
  "/rtl_bt/"
  "/mediatek/"
  "/brcm/"
  "/qca/"
  "/intel/"
  "/amd-ucode/"
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
  ( cd "$SRC"
    find . -mindepth 1 -maxdepth 1 -name '.git*' -prune -o -print0 \
      | xargs -0 -I{} cp -a {} "$DEST/" )

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
for f in regulatory.db.xz iwlwifi-cc-a0-77.ucode.xz i915 amdgpu; do
  if [ -e "$DEST/$f" ]; then echo "  present: $f"; else echo "  MISSING: $f"; fi
done
[ -e "$DEST/regulatory.db.xz" ] || { echo "FATAL: regulatory.db missing, wifi regdomain will fail"; exit 1; }
