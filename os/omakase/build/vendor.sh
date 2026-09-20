#!/bin/bash
set -euo pipefail

ARCH="${1:-x86_64}"
OUT="${OUT:-$PWD/out-omakase/vendor}"
CACHE="${VENDOR_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/saltos-omakase/vendor}"
HERE="$(cd "$(dirname "$0")" && pwd)"
HELIUM_KEY="$HERE/helium-signing-key.asc"
HELIUM_FPR=BE677C1989D35EAB2C5F26C9351601AD01D6378E

VICINAE_VERSION=0.29.0
HELIUM_VERSION=0.17.0.1
GUM_VERSION=0.16.2

case "$ARCH" in
  x86_64)
    vicinae_file="Vicinae-x86_64.AppImage"
    vicinae_sha=4d32f7589f857ee6229da95269c6a3109fc3093152c2d6f29042f7d924fe86bc
    helium_file="helium-$HELIUM_VERSION-x86_64_linux.tar.xz"
    helium_sha=50238835e8896253d4af142a3032354119c1e6f7e777926c4530195c714ff3f5
    gum_file="gum_${GUM_VERSION}_Linux_x86_64.tar.gz"
    gum_sha=b7a9db6cee95a3475f6f18fb860dc3d3f812bd0b9e12071e448a942eebb1457a
    ;;
  aarch64)
    vicinae_file="Vicinae-aarch64.AppImage"
    vicinae_sha=13c524217f9fc9bb3b0b512074c3ab5ac2e42f78639ca891d1debd40d52a2f89
    helium_file="helium-$HELIUM_VERSION-arm64_linux.tar.xz"
    helium_sha=4c41e22a5cbe2854bc3bd45aa7a74bce16087f727f8f65ad651c7ba1ab84b46f
    gum_file="gum_${GUM_VERSION}_Linux_arm64.tar.gz"
    gum_sha=05870f6f7b86ce64d27ed79555dcb7ad50c17ef4fe29f396cd7a4c010cde5a4b
    ;;
  *) echo "vendor: unsupported arch $ARCH" >&2; exit 1 ;;
esac

for t in curl sha256sum gpg gpgv readelf unsquashfs; do
  command -v "$t" >/dev/null || { echo "vendor: missing tool $t" >&2; exit 1; }
done
mkdir -p "$OUT" "$CACHE"

fetch() {
  local url="$1" file="$2" sha="$3"
  if [ -f "$CACHE/$file" ] && printf '%s  %s\n' "$sha" "$CACHE/$file" | sha256sum -c --quiet - 2>/dev/null; then
    return 0
  fi
  echo "==> fetching $file"
  curl -fL --retry 3 -o "$CACHE/$file.part" "$url"
  printf '%s  %s\n' "$sha" "$CACHE/$file.part" | sha256sum -c --quiet -
  mv "$CACHE/$file.part" "$CACHE/$file"
}

fetch_sig() {
  local url="$1" file="$2"
  [ -f "$CACHE/$file" ] && return 0
  echo "==> fetching $file"
  curl -fL --retry 3 -o "$CACHE/$file.part" "$url"
  mv "$CACHE/$file.part" "$CACHE/$file"
}

verify_helium_signature() {
  local keyring
  keyring="$(mktemp -d)"
  gpg --batch --quiet --dearmor -o "$keyring/helium.gpg" "$HELIUM_KEY"
  gpg --batch --quiet --no-default-keyring --keyring "$keyring/helium.gpg" --list-keys --with-colons \
    | grep -q "^fpr:*:$HELIUM_FPR:" || { echo "vendor: helium signing key fingerprint mismatch" >&2; rm -rf "$keyring"; exit 1; }
  gpgv --keyring "$keyring/helium.gpg" "$CACHE/$helium_file.asc" "$CACHE/$helium_file" \
    || { echo "vendor: helium signature verification failed" >&2; rm -rf "$keyring"; exit 1; }
  rm -rf "$keyring"
}

appimage_extract() {
  local image="$1" dest="$2" offset
  offset="$(readelf -h "$image" | awk '
    /Start of section headers/ { start = $5 }
    /Size of section headers/ { size = $5 }
    /Number of section headers/ { count = $5 }
    END { print start + size * count }')"
  [ "${offset:-0}" -gt 0 ] || { echo "vendor: cannot locate the squashfs payload in $image" >&2; exit 1; }
  unsquashfs -q -n -d "$dest" -o "$offset" "$image" >/dev/null
}

fetch "https://github.com/vicinaehq/vicinae/releases/download/v$VICINAE_VERSION/$vicinae_file" "$vicinae_file" "$vicinae_sha"
fetch "https://github.com/imputnet/helium-linux/releases/download/$HELIUM_VERSION/$helium_file" "$helium_file" "$helium_sha"
fetch_sig "https://github.com/imputnet/helium-linux/releases/download/$HELIUM_VERSION/$helium_file.asc" "$helium_file.asc"
fetch "https://github.com/charmbracelet/gum/releases/download/v$GUM_VERSION/$gum_file" "$gum_file" "$gum_sha"

echo "==> verifying helium signature"
verify_helium_signature

echo "==> staging vicinae"
rm -rf "$OUT/vicinae-$ARCH"
appimage_extract "$CACHE/$vicinae_file" "$OUT/vicinae-$ARCH"
[ -x "$OUT/vicinae-$ARCH/AppRun" ] || { echo "vendor: vicinae AppRun missing" >&2; exit 1; }
printf '%s\n' "$VICINAE_VERSION" >"$OUT/vicinae-$ARCH/VERSION"

echo "==> staging helium"
rm -rf "$OUT/helium-$ARCH"
mkdir -p "$OUT/helium-$ARCH"
tar -xJf "$CACHE/$helium_file" --strip-components=1 -C "$OUT/helium-$ARCH"
[ -x "$OUT/helium-$ARCH/helium" ] || { echo "vendor: helium binary missing" >&2; exit 1; }
printf '%s\n' "$HELIUM_VERSION" >"$OUT/helium-$ARCH/VERSION"

echo "==> staging gum"
tar -xzf "$CACHE/$gum_file" --strip-components=1 -C "$OUT" "${gum_file%.tar.gz}/gum"
mv "$OUT/gum" "$OUT/gum-$ARCH"
chmod 0755 "$OUT/gum-$ARCH"

du -sh "$OUT"
