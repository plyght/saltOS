#!/bin/sh
set -u

CONSOLE=/dev/console
STATE=/run/saltos-autoinstall
CFG_DEST=$STATE/system.lua
FWCFG_DIR=/sys/firmware/qemu_fw_cfg/by_name/opt/saltos
FWCFG=$FWCFG_DIR/autoinstall/raw

say() { echo "SALTOS_AUTOINSTALL $*" > "$CONSOLE"; }

mkdir -p "$STATE"
chmod 0755 "$STATE"

mode=""
source=""
then_action="poweroff"
for w in $(cat /proc/cmdline); do
  case "$w" in
    saltos.autoinstall=*) mode="${w#saltos.autoinstall=}" ;;
    saltos.autoinstall.config=*) source="${w#saltos.autoinstall.config=}" ;;
    saltos.autoinstall.then=*) then_action="${w#saltos.autoinstall.then=}" ;;
  esac
done
if [ -z "$mode" ]; then
  modprobe qemu_fw_cfg 2> /dev/null
  n=0
  while [ "$n" -lt 5 ] && [ ! -d /sys/firmware/qemu_fw_cfg/by_name ]; do sleep 1; n=$((n + 1)); done
  [ -r "$FWCFG_DIR/autoinstall-mode/raw" ] && mode="$(tr -d '\n' < "$FWCFG_DIR/autoinstall-mode/raw")"
  [ -r "$FWCFG_DIR/autoinstall-then/raw" ] && then_action="$(tr -d '\n' < "$FWCFG_DIR/autoinstall-then/raw")"
fi

echo "${mode:-none}" > "$STATE/mode"
chmod 0644 "$STATE/mode"
[ -n "$mode" ] || exec sleep infinity

wait_net() {
  n=0
  while [ "$n" -lt 90 ]; do
    getent hosts deb.debian.org > /dev/null 2>&1 && return 0
    sleep 2
    n=$((n + 1))
  done
  return 1
}

fetch_config() {
  case "$source" in
    "")
      modprobe qemu_fw_cfg 2> /dev/null
      n=0
      while [ "$n" -lt 15 ] && [ ! -r "$FWCFG" ]; do sleep 1; n=$((n + 1)); done
      [ -r "$FWCFG" ] || { say "FAIL no saltos.autoinstall.config= and no QEMU fw_cfg opt/saltos/autoinstall"; return 1; }
      cp "$FWCFG" "$CFG_DEST"
      ;;
    http://*|https://*)
      wait_net || { say "FAIL network never came up to fetch $source"; return 1; }
      curl -fsSL "$source" -o "$CFG_DEST" || { say "FAIL could not download $source"; return 1; }
      ;;
    LABEL=*|UUID=*)
      case "$source" in
        LABEL=*) dev="/dev/disk/by-label/${source#LABEL=}" ;;
        *) dev="/dev/disk/by-uuid/${source#UUID=}" ;;
      esac
      n=0
      while [ "$n" -lt 30 ] && [ ! -e "$dev" ]; do sleep 1; n=$((n + 1)); done
      [ -e "$dev" ] || { say "FAIL device $source never appeared"; return 1; }
      mkdir -p "$STATE/media"
      mount -o ro "$dev" "$STATE/media" || { say "FAIL cannot mount $source"; return 1; }
      for f in system.lua saltos.lua system.toml saltos.toml; do
        [ -f "$STATE/media/$f" ] && cp "$STATE/media/$f" "$CFG_DEST" && break
      done
      umount "$STATE/media"
      ;;
    /*)
      cp "$source" "$CFG_DEST" || { say "FAIL cannot read $source"; return 1; }
      ;;
    *)
      say "FAIL unsupported saltos.autoinstall.config=$source"
      return 1
      ;;
  esac
  [ -s "$CFG_DEST" ] || { say "FAIL empty configuration from ${source:-fw_cfg}"; return 1; }
  # Configurations are Lua (they return a table). An older TOML one is kept
  # under a .toml name so salt-setup reads it as TOML.
  if ! grep -q '^[[:space:]]*return[[:space:]{]' "$CFG_DEST"; then
    mv "$CFG_DEST" "$STATE/system.toml"
    CFG_DEST=$STATE/system.toml
  fi
  chmod 0600 "$CFG_DEST"
  return 0
}

finish() {
  status="$1"
  if [ "$status" = OK ]; then
    say "OK $mode install finished"
    sync
    case "$then_action" in
      poweroff) say "powering off"; sleep 2; poweroff || runit-init 0 ;;
      reboot) say "rebooting"; sleep 2; reboot || runit-init 6 ;;
      *) ;;
    esac
  fi
  exec sleep infinity
}

case "$mode" in
  text)
    fetch_config || exec sleep infinity
    say "text install starting from ${source:-fw_cfg}"
    wait_net || say "WARN network is not up; stratum bootstrap may fail"
    if salt-setup --from "$CFG_DEST" --yes > "$CONSOLE" 2>&1; then
      rm -f "$CFG_DEST"
      finish OK
    fi
    say "FAIL salt-setup exited with an error"
    exec sleep infinity
    ;;
  calamares)
    fetch_config || exec sleep infinity
    chmod 0644 "$CFG_DEST"
    say "waiting for the Calamares session driver"
    wait_net || say "WARN network is not up; stratum bootstrap may fail"
    while [ ! -f "$STATE/result" ]; do sleep 5; done
    if [ "$(cat "$STATE/result")" = OK ]; then finish OK; fi
    say "FAIL Calamares session driver reported failure"
    exec sleep infinity
    ;;
  *)
    say "FAIL unknown saltos.autoinstall=$mode"
    exec sleep infinity
    ;;
esac
