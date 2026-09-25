#!/bin/sh
set -u

STATE=/run/saltos-autoinstall
RESULT=$STATE/result
LOG=$STATE/calamares-driver.log
SHOTS=$STATE/shots
CFG=$HOME/.cache/saltos-autoinstall.lua

say() { echo "SALTOS_GUI_AUTOINSTALL $*" | sudo tee -a "$LOG" | sudo tee /dev/console > /dev/null; }
result() { echo "$1" | sudo tee "$RESULT" > /dev/null; }

sudo mkdir -p "$STATE" "$SHOTS"
sudo chmod 0755 "$STATE" "$SHOTS"

n=0
while [ "$n" -lt 60 ] && ! sudo test -s "$STATE/system.lua"; do sleep 2; n=$((n + 1)); done
mkdir -p "$HOME/.cache"
(umask 077; sudo cat "$STATE/system.lua" > "$CFG" 2> /dev/null)
[ -s "$CFG" ] || { say "FAIL no configuration for the GUI driver"; result FAIL; exit 1; }

# conf_get KEY: KEY (dotted, or `stratum[1].name` as `salt eval` prints it) from the Lua config.
conf_get() {
  case "$1" in
    *\[*) salt eval "$CFG" 2> /dev/null | while IFS= read -r line; do
        [ "${line%% = *}" = "$1" ] && { printf '%s\n' "${line#* = }"; break; }
      done ;;
    *) salt eval "$CFG" "$1" 2> /dev/null ;;
  esac
  return 0
}
HOSTNAME_="$(conf_get system.hostname)"
USERNAME_="$(conf_get user.name)"
PASSWORD_="$(conf_get user.password)"
STRATUM_="$(conf_get 'stratum[1].name')"
DESKTOP_="$(conf_get install.desktop)"
[ -n "$HOSTNAME_" ] || HOSTNAME_=saltos
[ -n "$USERNAME_" ] || USERNAME_=salt
[ -n "$PASSWORD_" ] || PASSWORD_=salt

shot() {
  command -v import > /dev/null 2>&1 && import -window root "$SHOTS/$1.png" 2> /dev/null && return
  xwd -root -silent 2> /dev/null > "$SHOTS/$1.xwd" || true
}

win() { xdotool search --onlyvisible --classname calamares 2> /dev/null | head -n1; }

wait_win() {
  n=0
  while [ "$n" -lt 120 ]; do
    w="$(win)"
    [ -n "$w" ] && { echo "$w"; return 0; }
    sleep 2
    n=$((n + 1))
  done
  return 1
}

geom() {
  eval "$(xdotool getwindowgeometry --shell "$1")"
}

key() {
  xdotool windowactivate --sync "$WIN" 2> /dev/null
  xdotool key --clearmodifiers "$1"
  sleep "${2:-1}"
}

next() {
  key alt+n "${1:-3}"
}

type_field() {
  xdotool type --delay 40 -- "$1"
  sleep 0.5
}

say "starting Calamares"
/usr/local/bin/saltos-installer > /tmp/calamares.out 2>&1 &
CAL_PID=$!

dump_calamares_log() {
  sudo cp /tmp/calamares.out "$LOG.calamares" 2> /dev/null
  tail -n 60 /tmp/calamares.out 2> /dev/null | sudo tee -a "$LOG" | sudo tee /dev/console > /dev/null
}

WIN="$(wait_win)" || { say "FAIL Calamares window never appeared"; dump_calamares_log; result FAIL; exit 1; }
sleep 6
xdotool windowactivate --sync "$WIN"
geom "$WIN"
say "window $WIN at ${X},${Y} size ${WIDTH}x${HEIGHT}"
shot 01-welcome

say "welcome -> next"
next 3
shot 02-location
say "location -> next"
next 3
shot 03-keyboard
say "keyboard -> next"
next 4
shot 04-partition
say "partition (erase disk, btrfs) -> next"
next 4
shot 05-stratum
say "primary stratum (${STRATUM_:-default}) -> next"
next 3
shot 06-desktop
say "desktop (${DESKTOP_:-default}) -> next"
next 3
shot 07-users

say "filling users page for $USERNAME_@$HOSTNAME_"
xdotool windowactivate --sync "$WIN"
sleep 0.5
xdotool key --clearmodifiers ctrl+a
type_field "$USERNAME_"
xdotool key Tab
sleep 0.5
xdotool key --clearmodifiers ctrl+a
type_field "$USERNAME_"
xdotool key Tab
sleep 0.5
xdotool key --clearmodifiers ctrl+a
type_field "$HOSTNAME_"
xdotool key Tab
sleep 0.5
type_field "$PASSWORD_"
xdotool key Tab
sleep 0.5
type_field "$PASSWORD_"
sleep 1
shot 08-users-filled
say "users -> next"
next 3
shot 09-summary
say "summary -> install"
key alt+i 3
shot 10-confirm
say "confirm -> install now"
xdotool key --clearmodifiers alt+i
sleep 3
shot 11-installing

n=0
while kill -0 "$CAL_PID" 2> /dev/null && [ "$n" -lt 720 ]; do
  if sudo grep -q "saltOS installed into" /run/saltos-installer/salt-setup.log 2> /dev/null; then break; fi
  [ $((n % 12)) -eq 0 ] && shot "12-progress-$n"
  sleep 5
  n=$((n + 1))
done
shot 13-finished

if sudo grep -q "saltOS installed into" /run/saltos-installer/salt-setup.log 2> /dev/null; then
  n=0
  while grep -q " /tmp/calamares-root-" /proc/mounts && [ "$n" -lt 60 ]; do sleep 2; n=$((n + 1)); done
  sleep 5
  shot 14-done
  say "OK Calamares drove salt-setup to completion"
  result OK
  exit 0
fi
say "FAIL install did not complete"
dump_calamares_log
sudo tail -n 40 /run/saltos-installer/salt-setup.log 2> /dev/null | sudo tee -a "$LOG" | sudo tee /dev/console > /dev/null
result FAIL
exit 1
