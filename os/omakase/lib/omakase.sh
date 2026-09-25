#!/bin/bash

OMAKASE_SHARE="${OMAKASE_SHARE:-/usr/share/saltos-omakase}"
OMAKASE_RUN="${OMAKASE_RUN:-/run/saltos-install}"
export OMAKASE_CONFIG="$OMAKASE_RUN/install.lua"
export OMAKASE_CREDENTIALS="$OMAKASE_RUN/credentials"
export OMAKASE_DEFERRED="$OMAKASE_RUN/defer-provisioning"
export OMAKASE_STATE="$OMAKASE_RUN/state.json"
export OMAKASE_LOG="$OMAKASE_RUN/install.log"
OMAKASE_TARGET="${OMAKASE_TARGET:-/mnt/saltos-target}"

# Config files (install.lua, strata/<n>.lua, system.lua) are Lua, evaluated by salt.
OMAKASE_SALT="${OMAKASE_SALT:-salt}"

# conf_get FILE KEY: print the value of KEY in the Lua config FILE (nothing if
# the file or key is missing). KEY is dotted (`install.disk`); an element of a
# list of tables is addressed as in `salt eval FILE` output (`stratum[1].name`).
conf_get() {
  local file="$1" key="$2" line
  [ -f "$file" ] || return 0
  case "$key" in
    *\[*)
      while IFS= read -r line; do
        if [ "${line%% = *}" = "$key" ]; then
          printf '%s\n' "${line#* = }"
          return 0
        fi
      done < <("$OMAKASE_SALT" eval "$file" 2>/dev/null)
      ;;
    *) "$OMAKASE_SALT" eval "$file" "$key" 2>/dev/null ;;
  esac
  return 0
}

# lua_quote STRING: STRING as a Lua string literal.
lua_quote() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  s="${s//$'\n'/\\n}"
  s="${s//$'\r'/\\r}"
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

stratum_recipe() {
  local recipe="/etc/salt/strata/$1.lua"
  [ -f "$recipe" ] || recipe="/usr/share/salt/strata/$1.lua"
  printf '%s\n' "$recipe"
}

stratum_family() {
  conf_get "$(stratum_recipe "$1")" family
}

stratum_bootstrap_method() {
  conf_get "$(stratum_recipe "$1")" bootstrap.method
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
  for f in /etc/salt/strata/*.lua; do
    [ -f "$f" ] || continue
    name="$(basename "$f" .lua)"
    case "$name" in *-x86_64|*-aarch64) continue ;; esac
    method="$(conf_get "$f" bootstrap.method)"
    case "$method" in
      rootfs|debootstrap) echo "$name" ;;
      oci) command -v docker >/dev/null 2>&1 || command -v podman >/dev/null 2>&1 && echo "$name" ;;
    esac
  done
}

primary_stratum() {
  local root="${1:-}" f
  f="$root/etc/salt/system.lua"
  # installs from before the Lua switch still have system.toml
  [ -f "$f" ] || f="$root/etc/salt/system.toml"
  conf_get "$f" "stratum[1].name"
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
