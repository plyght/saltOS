#!/bin/sh
# Prepare a staged desktop rootfs for a live session: a passwordless `live`
# user that SDDM logs straight into LXQt. With --check, also a runit service
# that reports on the console once the whole session is up (CI boot test).
#
#   os/desktop/live-session.sh <rootfs> [--check]
set -eu

ROOTFS=${1:?usage: live-session.sh <rootfs> [--check]}
CHECK=${2:-}
[ -d "$ROOTFS/etc" ] || { echo "not a rootfs: $ROOTFS" >&2; exit 2; }

in_root() { chroot "$ROOTFS" /bin/sh -c "$1"; }

in_root 'getent passwd live >/dev/null || useradd -m -U -G wheel,audio,video,input -s /bin/bash -c "Live session" live'
in_root 'passwd -d live >/dev/null'

mkdir -p "$ROOTFS/etc/sddm.conf.d"
cat > "$ROOTFS/etc/sddm.conf.d/50-live-autologin.conf" <<'CONF'
[Autologin]
User=live
Session=lxqt
Relogin=false
CONF

[ "$CHECK" = "--check" ] || exit 0

mkdir -p "$ROOTFS/etc/runit/sv/desktop-check"
cat > "$ROOTFS/etc/runit/sv/desktop-check/run" <<'RUN'
#!/bin/sh
exec 2>&1
say() {
	echo "$1" > /dev/console
	[ -c /dev/ttyS0 ] && echo "$1" > /dev/ttyS0
}
i=0
while [ "$i" -lt 900 ]; do
	if pgrep -x Xorg >/dev/null && pgrep -x lxqt-session >/dev/null &&
		pgrep -x lxqt-panel >/dev/null && pgrep -x openbox >/dev/null; then
		say "SALTOS_DESKTOP_OK sddm autologin -> lxqt-session + lxqt-panel + openbox on Xorg"
		exec sleep infinity
	fi
	i=$((i + 1))
	sleep 1
done
say "SALTOS_DESKTOP_FAIL session did not come up within 900s"
for f in /var/log/sddm.log /var/log/Xorg.0.log /home/live/.local/share/sddm/xorg-session.log; do
	[ -f "$f" ] || continue
	say "---- $f"
	tail -n 60 "$f" | while IFS= read -r l; do say "$l"; done
done
ps -ef | while IFS= read -r l; do say "$l"; done
exec sleep infinity
RUN
chmod 0755 "$ROOTFS/etc/runit/sv/desktop-check/run"
ln -sf /etc/runit/sv/desktop-check "$ROOTFS/etc/runit/runsvdir/current/desktop-check"
