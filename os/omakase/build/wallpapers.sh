#!/bin/bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$PWD/out-omakase/wallpapers}"
CACHE="${WALLPAPER_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/saltos-omakase/wallpapers}"

mkdir -p "$OUT" "$CACHE"
rm -rf "${OUT:?}"/*

fetch() {
  local url="$1" file="$2" sha="$3"
  if [ -f "$CACHE/$file" ] && printf '%s  %s\n' "$sha" "$CACHE/$file" | sha256sum -c --quiet - 2>/dev/null; then
    return 0
  fi
  echo "==> fetching $file"
  curl -fL --retry 3 -o "$CACHE/$file.part" "$url"
  if ! printf '%s  %s\n' "$sha" "$CACHE/$file.part" | sha256sum -c --quiet -; then
    echo "wallpapers: sha256 mismatch for $file ($url)" >&2
    rm -f "$CACHE/$file.part"
    exit 1
  fi
  mv "$CACHE/$file.part" "$CACHE/$file"
}

toml_value() {
  sed -n "s/^$1 = \"\(.*\)\"\$/\1/p" | head -1
}

credits="$OUT/CREDITS"
{
  echo "saltOS omakase wallpapers"
  echo
  echo "All photos are from Unsplash and distributed under the Unsplash License"
  echo "(https://unsplash.com/license): free to use, redistribute and modify,"
  echo "including commercially; attribution appreciated."
  echo
} >"$credits"

count=0
for manifest in "$HERE"/wallpapers/*.toml; do
  theme="$(toml_value theme <"$manifest")"
  [ -n "$theme" ] || { echo "wallpapers: $manifest has no theme" >&2; exit 1; }
  mkdir -p "$OUT/$theme"
  echo "[$theme]" >>"$credits"
  n=0
  while IFS= read -r block; do
    [ -n "$block" ] || continue
    id="$(printf '%b' "$block" | toml_value id)"
    title="$(printf '%b' "$block" | toml_value title)"
    photographer="$(printf '%b' "$block" | toml_value photographer)"
    profile="$(printf '%b' "$block" | toml_value profile)"
    page="$(printf '%b' "$block" | toml_value page)"
    url="$(printf '%b' "$block" | toml_value url)"
    sha="$(printf '%b' "$block" | toml_value sha256)"
    for v in id title photographer profile page url sha; do
      [ -n "${!v}" ] || { echo "wallpapers: $manifest: wallpaper $((n + 1)) missing $v" >&2; exit 1; }
    done
    fetch "$url" "$id.jpg" "$sha"
    n=$((n + 1))
    file="$(printf '%02d-%s.jpg' "$n" "$id")"
    install -m 0644 "$CACHE/$id.jpg" "$OUT/$theme/$file"
    printf '%s\n  "%s" by %s (%s)\n  %s\n' "$file" "$title" "$photographer" "$profile" "$page" >>"$credits"
    count=$((count + 1))
  done < <(awk '
    /^\[\[wallpaper\]\]/ { if (block != "") print block; block = ""; inblock = 1; next }
    inblock && NF { block = block $0 "\\n" }
    END { if (block != "") print block }' "$manifest")
  [ "$n" -gt 0 ] || { echo "wallpapers: $manifest lists no wallpapers" >&2; exit 1; }
  echo >>"$credits"
done

echo "==> $count wallpapers staged in $OUT"
