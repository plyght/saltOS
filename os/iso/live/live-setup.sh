#!/bin/sh
PATH=/usr/bin:/usr/sbin:/bin:/sbin
export PATH

LIVE_USER=salt
LIVE_HOME=/home/$LIVE_USER

if ! getent group "$LIVE_USER" >/dev/null 2>&1; then
	groupadd "$LIVE_USER"
fi

if ! getent passwd "$LIVE_USER" >/dev/null 2>&1; then
	useradd -m -g "$LIVE_USER" -G wheel,audio,video,input,seat -s /bin/bash "$LIVE_USER"
	passwd -d "$LIVE_USER"
fi

for grp in wheel audio video input seat; do
	getent group "$grp" >/dev/null 2>&1 && usermod -aG "$grp" "$LIVE_USER"
done

mkdir -p "$LIVE_HOME/Desktop"
if [ -f /usr/share/applications/Install-saltOS.desktop ]; then
	cp /usr/share/applications/Install-saltOS.desktop "$LIVE_HOME/Desktop/Install-saltOS.desktop"
	chmod 0755 "$LIVE_HOME/Desktop/Install-saltOS.desktop"
fi

cat > "$LIVE_HOME/.xprofile" <<'EOF'
export XDG_CURRENT_DESKTOP=LXQt
EOF

chown -R "$LIVE_USER:$LIVE_USER" "$LIVE_HOME"

if ! grep -q '^%wheel' /etc/sudoers 2>/dev/null; then
	echo '%wheel ALL=(ALL) NOPASSWD: ALL' >> /etc/sudoers
fi

mkdir -p /etc/sddm.conf.d
cat > /etc/sddm.conf.d/10-live-autologin.conf <<EOF
[Autologin]
User=$LIVE_USER
Session=lxqt
Relogin=false

[General]
HaltCommand=/usr/bin/loginctl poweroff
RebootCommand=/usr/bin/loginctl reboot
EOF

mkdir -p /run/salt
if [ -d /run/initramfs/live/repo ]; then
	mkdir -p /run/salt/repo
	mount --bind /run/initramfs/live/repo /run/salt/repo 2>/dev/null
elif [ -d /lib/live/mount/medium/repo ]; then
	mkdir -p /run/salt/repo
	mount --bind /lib/live/mount/medium/repo /run/salt/repo 2>/dev/null
fi
