#!/bin/bash

OMAKASE_SHARE="${OMAKASE_SHARE:-/usr/share/saltos-omakase}"
OMAKASE_RUN="${OMAKASE_RUN:-/run/saltos-install}"
export OMAKASE_CONFIG="$OMAKASE_RUN/install.toml"
export OMAKASE_CREDENTIALS="$OMAKASE_RUN/credentials"
export OMAKASE_DEFERRED="$OMAKASE_RUN/defer-provisioning"
export OMAKASE_STATE="$OMAKASE_RUN/state.json"
export OMAKASE_LOG="$OMAKASE_RUN/install.log"
OMAKASE_TARGET="${OMAKASE_TARGET:-/mnt/saltos-target}"

toml_get() {
  local file="$1" section="$2" key="$3"
  awk -v want="$section" -v key="$key" '
    /^[[:space:]]*\[\[?[^]]+\]\]?[[:space:]]*$/ {
      s=$0; gsub(/[][[:space:]]/, "", s); cur=s; next
    }
    cur == want {
      line=$0; sub(/^[[:space:]]+/, "", line)
      if (index(line, key) == 1) {
        rest=substr(line, length(key)+1); sub(/^[[:space:]]+/, "", rest)
        if (substr(rest,1,1) == "=") {
          v=substr(rest,2); sub(/^[[:space:]]+/, "", v); sub(/[[:space:]]+$/, "", v)
          if (substr(v,1,1) == "\"") { v=substr(v,2); sub(/"$/, "", v) }
          print v; exit
        }
      }
    }' "$file"
}

toml_quote() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '"%s"' "$s"
}

credential_get() {
  local key="$1"
  [ -r "$OMAKASE_CREDENTIALS" ] || return 1
  awk -F= -v k="$key" '$1 == k { sub(/^[^=]*=/, ""); print; exit }' "$OMAKASE_CREDENTIALS"
}

pkgmap_column() {
  head -1 "$OMAKASE_SHARE/packages/packages.tsv" | tr '\t' '\n' | grep -nx "$1" | cut -d: -f1
}

pkgmap_packages() {
  local distro="$1" col
  col="$(pkgmap_column "$distro")"
  [ -n "$col" ] || return 1
  tail -n +2 "$OMAKASE_SHARE/packages/packages.tsv" | cut -f"$col" | tr ' ' '\n' \
    | grep -v '^-$' | grep -v '^$' | awk '!seen[$0]++'
}

pkgmap_role() {
  local distro="$1" role="$2" col
  col="$(pkgmap_column "$distro")"
  [ -n "$col" ] || return 1
  awk -F'\t' -v r="$role" -v c="$col" '$1 == r { print $c; exit }' \
    "$OMAKASE_SHARE/packages/packages.tsv" | tr ' ' '\n' | grep -v '^-$' | grep -v '^$'
}

stratum_family() {
  local recipe="/etc/salt/strata/$1.toml"
  [ -f "$recipe" ] || recipe="/usr/share/salt/strata/$1.toml"
  toml_get "$recipe" "" family
}

stratum_bootstrap_method() {
  local recipe="/etc/salt/strata/$1.toml"
  [ -f "$recipe" ] || recipe="/usr/share/salt/strata/$1.toml"
  toml_get "$recipe" bootstrap method
}

serial_tty() {
  case "$(uname -m)" in
    aarch64) echo ttyAMA0 ;;
    *) echo ttyS0 ;;
  esac
}

live_medium() {
  local d
  for d in /run/initramfs/live /run/live/medium /lib/live/mount/medium; do
    if [ -d "$d/omakase" ]; then
      echo "$d"
      return 0
    fi
  done
  return 1
}

available_strata() {
  local f name method
  for f in /etc/salt/strata/*.toml; do
    name="$(basename "$f" .toml)"
    case "$name" in *-x86_64|*-aarch64) continue ;; esac
    method="$(toml_get "$f" bootstrap method)"
    case "$method" in
      rootfs|debootstrap) echo "$name" ;;
      oci) command -v docker >/dev/null 2>&1 || command -v podman >/dev/null 2>&1 && echo "$name" ;;
    esac
  done
}

primary_stratum() {
  local root="${1:-}"
  toml_get "$root/etc/salt/system.toml" stratum name
}

current_theme() {
  local f="${XDG_CONFIG_HOME:-$HOME/.config}/saltos/current-theme"
  [ -s "$f" ] && cat "$f" || echo tokyo-night
}

list_themes() {
  ls -1 "$OMAKASE_SHARE/themes"
}

as_user() {
  local user="$1"; shift
  if [ "$(id -un)" = "$user" ]; then
    "$@"
  else
    setpriv --reuid="$user" --regid="$(id -g "$user")" --init-groups \
      env HOME="$(getent passwd "$user" | cut -d: -f6)" USER="$user" LOGNAME="$user" "$@"
  fi
}
