#!/bin/sh
set -eu

prog=${0##*/}
FW=${SALTOS_FW_DIR:-/boot/firmware}
ROOTDEV_LABEL=${SALTOS_ROOT_LABEL:-saltos-root}
MNT=${SALTOS_AB_MNT:-/run/saltos-ab}

die() { printf '%s: %s\n' "$prog" "$1" >&2; exit 1; }
need_root() { [ "$(id -u)" -eq 0 ] || die "must run as root"; }

active_subvol() {
	for tok in $(cat /proc/cmdline); do
		case "$tok" in
			rootflags=*)
				val=${tok#rootflags=}
				for kv in $(echo "$val" | tr ',' ' '); do
					case "$kv" in subvol=*) echo "${kv#subvol=}"; return 0 ;; esac
				done
			;;
		esac
	done
	echo "@"
}

other_subvol() {
	case "$1" in
		@) echo "@b" ;;
		@b) echo "@" ;;
		*) echo "@b" ;;
	esac
}

mount_root() {
	mkdir -p "$MNT"
	mountpoint -q "$MNT" || mount -L "$ROOTDEV_LABEL" -o subvolid=5 "$MNT"
}

umount_root() {
	mountpoint -q "$MNT" && umount "$MNT" || true
}

cmd_status() {
	cur=$(active_subvol)
	printf 'active subvol: %s\n' "$cur"
	printf 'standby subvol: %s\n' "$(other_subvol "$cur")"
	if [ -f "$FW/tryboot.txt" ]; then
		printf 'tryboot: pending (uncommitted)\n'
	else
		printf 'tryboot: none\n'
	fi
	mount_root
	btrfs subvolume list "$MNT" 2>/dev/null | awk '{print "  subvol "$NF}' || true
	umount_root
}

cmd_prepare() {
	need_root
	command -v btrfs >/dev/null 2>&1 || die "btrfs-progs not found"
	cur=$(active_subvol)
	tgt=$(other_subvol "$cur")
	mount_root
	[ -e "$MNT/$tgt" ] && btrfs subvolume delete "$MNT/$tgt" >/dev/null 2>&1 || true
	btrfs subvolume snapshot "$MNT/$cur" "$MNT/$tgt" >&2
	umount_root

	stage="/run/saltos-ab-target"
	rm -rf "$stage"; mkdir -p "$stage"
	mount -L "$ROOTDEV_LABEL" -o "subvol=$tgt" "$stage"
	printf '%s\n' "$stage"
}

cmd_finalize() {
	need_root
	stage=${1:-/run/saltos-ab-target}
	cur=$(active_subvol)
	tgt=$(other_subvol "$cur")

	cmdline_cur="cmdline.txt"
	cmdline_new="cmdline_${tgt#@}.txt"
	[ "$cmdline_new" = "cmdline_.txt" ] && cmdline_new="cmdline_a.txt"

	sed "s/subvol=[^ ,]*/subvol=$tgt/" "$FW/$cmdline_cur" > "$FW/$cmdline_new"
	{
		grep -v '^cmdline=' "$FW/config.txt"
		printf 'cmdline=%s\n' "$cmdline_new"
	} > "$FW/tryboot.txt"

	mountpoint -q "$stage" && umount "$stage" || true
	sync
	printf '%s: prepared tryboot into %s; reboot with: reboot "0 tryboot"\n' "$prog" "$tgt" >&2
}

cmd_commit() {
	need_root
	[ -f "$FW/tryboot.txt" ] || die "no pending tryboot to commit"
	cur=$(active_subvol)
	cmdline_new=$(grep '^cmdline=' "$FW/tryboot.txt" | head -1 | cut -d= -f2)
	[ -n "$cmdline_new" ] && cp "$FW/$cmdline_new" "$FW/cmdline.txt"
	grep -v '^cmdline=' "$FW/tryboot.txt" > "$FW/config.txt"
	rm -f "$FW/tryboot.txt"
	sync
	printf '%s: committed %s as the new default boot\n' "$prog" "$cur" >&2
}

cmd_abort() {
	need_root
	rm -f "$FW/tryboot.txt"
	sync
	printf '%s: cleared pending tryboot; next reboot uses committed default\n' "$prog" >&2
}

cmd=${1:-status}
shift 2>/dev/null || true
case "$cmd" in
	status) cmd_status "$@" ;;
	prepare) cmd_prepare "$@" ;;
	finalize) cmd_finalize "$@" ;;
	commit) cmd_commit "$@" ;;
	abort) cmd_abort "$@" ;;
	active) active_subvol ;;
	-h|--help|help)
		cat <<EOF
usage: $prog <command>

  status     show active/standby subvol and tryboot state
  prepare    snapshot active root into standby subvol, print its mountpoint
  finalize   point a one-shot tryboot at the standby subvol
  commit     make the tried boot the permanent default
  abort      clear a pending tryboot
  active     print the currently booted subvol
EOF
	;;
	*) die "unknown command: $cmd (try '$prog --help')" ;;
esac
