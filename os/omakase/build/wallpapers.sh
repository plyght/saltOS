#!/bin/bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$PWD/out-omakase/wallpapers}"
CACHE="${WALLPAPER_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/saltos-omakase/wallpapers}"
# The manifests are Lua (os/omakase/wallpapers/<theme>.lua), evaluated by salt.
SALT_BIN="${SALT_BIN:-salt}"
command -v "$SALT_BIN" >/dev/null 2>&1 || { echo "wallpapers: salt binary '$SALT_BIN' not found (set SALT_BIN)" >&2; exit 1; }

mkdir -p "$OUT" "$CACHE"
rm -rf "${OUT:?}"/*

# The photos are committed under wallpapers/images/ (Unsplash re-encodes its
# images over time, so the pinned bytes are ours); the Unsplash url is only a
# fallback for a checkout without them.
fetch() {
  local url="$1" file="$2" sha="$3"
  if [ -f "$HERE/wallpapers/images/$file" ]; then
    if printf '%s  %s\n' "$sha" "$HERE/wallpapers/images/$file" | sha256sum -c --quiet -; then
      cp "$HERE/wallpapers/images/$file" "$CACHE/$file"
      return 0
    fi
    echo "wallpapers: sha256 mismatch for committed wallpapers/images/$file" >&2
    exit 1
  fi
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

# wallpaper_field N KEY: KEY of the Nth wallpaper from the flattened
# `salt eval` dump held in $dump ("wallpaper[N].KEY = value" lines).
wallpaper_field() {
  local prefix="wallpaper[$1].$2 = "
  local line
  while IFS= read -r line; do
    if [ "${line:0:${#prefix}}" = "$prefix" ]; then
      printf '%s\n' "${line:${#prefix}}"
      return 0
    fi
  done <<<"$dump"
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
for manifest in "$HERE"/wallpapers/*.lua; do
  dump="$("$SALT_BIN" eval "$manifest")" || { echo "wallpapers: cannot evaluate $manifest" >&2; exit 1; }
  theme="$("$SALT_BIN" eval "$manifest" theme 2>/dev/null || true)"
  [ -n "$theme" ] || { echo "wallpapers: $manifest has no theme" >&2; exit 1; }
  mkdir -p "$OUT/$theme"
  echo "[$theme]" >>"$credits"
  n=0
  total="$(printf '%s\n' "$dump" | sed -n 's/^wallpaper\[\([0-9][0-9]*\)\]\..*/\1/p' | sort -n | tail -1)"
  for ((i = 1; i <= ${total:-0}; i++)); do
    id="$(wallpaper_field "$i" id)"
    title="$(wallpaper_field "$i" title)"
    photographer="$(wallpaper_field "$i" photographer)"
    profile="$(wallpaper_field "$i" profile)"
    page="$(wallpaper_field "$i" page)"
    url="$(wallpaper_field "$i" url)"
    sha="$(wallpaper_field "$i" sha256)"
    for v in id title photographer profile page url sha; do
      [ -n "${!v}" ] || { echo "wallpapers: $manifest: wallpaper $i missing $v" >&2; exit 1; }
    done
    fetch "$url" "$id.jpg" "$sha"
    n=$((n + 1))
    file="$(printf '%02d-%s.jpg' "$n" "$id")"
    install -m 0644 "$CACHE/$id.jpg" "$OUT/$theme/$file"
    printf '%s\n  "%s" by %s (%s)\n  %s\n' "$file" "$title" "$photographer" "$profile" "$page" >>"$credits"
    count=$((count + 1))
  done
  [ "$n" -gt 0 ] || { echo "wallpapers: $manifest lists no wallpapers" >&2; exit 1; }
  echo >>"$credits"
done

echo "==> $count wallpapers staged in $OUT"
