#!/bin/sh
set -eu

prog=${0##*/}
FW=${SALTOS_FW_DIR:-/boot/firmware}
ROOTDEV_LABEL=${SALTOS_ROOT_LABEL:-saltos-root}
MNT=${SALTOS_AB_MNT:-/run/saltos-ab}
LIVE_ROOT=${SALTOS_LIVE_ROOT:-/}
STAGE_ROOT=${SALTOS_STAGE_ROOT:-$LIVE_ROOT}
PENDING="$FW/saltos-pending"

die() { printf '%s: %s\n' "$prog" "$1" >&2; exit 1; }
need_root() { [ "$(id -u)" -eq 0 ] || die "must run as root"; }

active_subvol() {
	[ -z "${SALTOS_ACTIVE_SUBVOL:-}" ] || { echo "$SALTOS_ACTIVE_SUBVOL"; return 0; }
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

slot_of() {
	case "$1" in
		@) echo a ;;
		@b) echo b ;;
		*) die "unknown root subvolume $1" ;;
	esac
}

mount_root() {
	mkdir -p "$MNT"
	mountpoint -q "$MNT" || mount -L "$ROOTDEV_LABEL" -o subvolid=5 "$MNT"
}

umount_root() {
	mountpoint -q "$MNT" && umount "$MNT" || true
}

newest_kernel() {
	root=$1
	for k in "$root"/boot/vmlinuz-* "$root"/boot/vmlinux-* "$root"/boot/Image-*; do
		[ -f "$k" ] && echo "${k##*/}"
	done | sort -t- -k2 -V | tail -n1
}

kernel_release() { echo "${1#*-}"; }

initrd_for() {
	root=$1; rel=$2
	for c in "initramfs-$rel.img" "initrd.img-$rel" "initrd-$rel" "initramfs-$rel"; do
		[ -f "$root/boot/$c" ] && { echo "$c"; return 0; }
	done
	return 1
}

running_release() { uname -r; }

pending_get() {
	[ -f "$PENDING" ] || return 1
	sed -n "s/^$1=//p" "$PENDING" | head -n1
}

pending_set() {
	{
		printf 'subvol=%s\n' "$1"
		printf 'kernel=%s\n' "$2"
		printf 'release=%s\n' "$3"
	} > "$PENDING"
}

clear_pending() {
	rm -f "$FW/tryboot.txt" "$PENDING"
	rm -f "$FW"/vmlinuz_*.next "$FW"/initramfs_*.next
}

copy_kernel() {
	root=$1; slot=$2; suffix=$3
	k=$(newest_kernel "$root")
	[ -n "$k" ] || die "no kernel found in $root/boot"
	rel=$(kernel_release "$k")
	cp "$root/boot/$k" "$FW/vmlinuz_$slot$suffix.tmp"
	mv -f "$FW/vmlinuz_$slot$suffix.tmp" "$FW/vmlinuz_$slot$suffix"
	if ird=$(initrd_for "$root" "$rel"); then
		cp "$root/boot/$ird" "$FW/initramfs_$slot$suffix.tmp"
		mv -f "$FW/initramfs_$slot$suffix.tmp" "$FW/initramfs_$slot$suffix"
	else
		rm -f "$FW/initramfs_$slot$suffix"
	fi
	echo "$rel" > "$FW/kernel_$slot$suffix.release"
	echo "$rel"
}

write_boot_cfg() {
	out=$1; slot=$2; suffix=$3; cmdline=$4
	src="$FW/config.txt"
	[ -f "$src" ] || die "$src missing"
	{
		grep -v -e '^kernel=' -e '^initramfs ' -e '^cmdline=' "$src"
		printf 'kernel=vmlinuz_%s%s\n' "$slot" "$suffix"
		[ -f "$FW/initramfs_$slot$suffix" ] && printf 'initramfs initramfs_%s%s followkernel\n' "$slot" "$suffix"
		printf 'cmdline=%s\n' "$cmdline"
	} > "$out.tmp"
	mv -f "$out.tmp" "$out"
}

cmd_status() {
	cur=$(active_subvol)
	printf 'active subvol: %s\n' "$cur"
	printf 'standby subvol: %s\n' "$(other_subvol "$cur")"
	if [ -f "$FW/tryboot.txt" ]; then
		printf 'tryboot: pending (%s, kernel %s, uncommitted)\n' \
			"$(pending_get subvol || echo '?')" "$(pending_get release || echo '?')"
	else
		printf 'tryboot: none\n'
	fi
	for s in a b; do
		[ -f "$FW/kernel_$s.release" ] && printf 'slot %s kernel: %s\n' "$s" "$(cat "$FW/kernel_$s.release")"
	done
	if [ "$(id -u)" -eq 0 ] && command -v btrfs >/dev/null 2>&1; then
		mount_root
		btrfs subvolume list "$MNT" 2>/dev/null | awk '{print "  subvol "$NF}' || true
		umount_root
	fi
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
	stage=${1:-/run/saltos-ab-target}
	cur=$(active_subvol)
	tgt=$(other_subvol "$cur")
	slot=$(slot_of "$tgt")
	cur_cmdline="cmdline_$(slot_of "$cur").txt"
	[ -f "$FW/$cur_cmdline" ] || cur_cmdline="cmdline.txt"

	rel=$(copy_kernel "$stage" "$slot" "")
	sed "s/subvol=[^ ,]*/subvol=$tgt/" "$FW/$cur_cmdline" > "$FW/cmdline_$slot.txt.tmp"
	mv -f "$FW/cmdline_$slot.txt.tmp" "$FW/cmdline_$slot.txt"
	write_boot_cfg "$FW/tryboot.txt" "$slot" "" "cmdline_$slot.txt"
	pending_set "$tgt" "vmlinuz_$slot" "$rel"

	mountpoint -q "$stage" 2>/dev/null && umount "$stage" || true
	sync
	printf '%s: prepared tryboot into %s (kernel %s); reboot with: reboot "0 tryboot"\n' "$prog" "$tgt" "$rel" >&2
}

cmd_commit() {
	[ -f "$FW/tryboot.txt" ] || die "no pending tryboot to commit"
	cur=$(active_subvol)
	want=$(pending_get subvol || echo "$cur")
	rel=$(pending_get release || echo "")
	if [ "$cur" != "$want" ] || { [ -n "$rel" ] && [ "$(running_release)" != "$rel" ]; }; then
		clear_pending
		sync
		die "trial boot of $want (kernel ${rel:-?}) fell back to $cur (kernel $(running_release)); pending tryboot discarded"
	fi
	slot=$(slot_of "$cur")
	if [ -f "$FW/vmlinuz_$slot.next" ]; then
		mv -f "$FW/vmlinuz_$slot.next" "$FW/vmlinuz_$slot"
		if [ -f "$FW/initramfs_$slot.next" ]; then
			mv -f "$FW/initramfs_$slot.next" "$FW/initramfs_$slot"
		else
			rm -f "$FW/initramfs_$slot"
		fi
		mv -f "$FW/kernel_$slot.next.release" "$FW/kernel_$slot.release"
		write_boot_cfg "$FW/config.txt" "$slot" "" "cmdline_$slot.txt"
	else
		cp "$FW/tryboot.txt" "$FW/config.txt.tmp"
		mv -f "$FW/config.txt.tmp" "$FW/config.txt"
	fi
	clear_pending
	sync
	printf '%s: committed %s (kernel %s) as the new default boot\n' "$prog" "$cur" "$(running_release)" >&2
}

cmd_abort() {
	clear_pending
	sync
	printf '%s: cleared pending tryboot; next reboot uses committed default\n' "$prog" >&2
}

cmd_kernel_update() {
	mode=${1:-}
	cur=$(active_subvol)
	slot=$(slot_of "$cur")
	if [ "$STAGE_ROOT" != "$LIVE_ROOT" ] && [ "$mode" != rollback ]; then
		k=$(newest_kernel "$STAGE_ROOT")
		[ -n "$k" ] || die "no kernel found in $STAGE_ROOT/boot"
		printf 'boot: staged root provides kernel %s (activated by finalize)\n' "$(kernel_release "$k")"
		return 0
	fi
	if [ "$mode" = rollback ]; then
		rel=$(copy_kernel "$STAGE_ROOT" "$slot" "")
		[ -f "$FW/cmdline_$slot.txt" ] || cp "$FW/cmdline.txt" "$FW/cmdline_$slot.txt"
		write_boot_cfg "$FW/config.txt" "$slot" "" "cmdline_$slot.txt"
		clear_pending
		sync
		printf 'boot: default kernel %s (rollback)\n' "$rel"
		return 0
	fi
	k=$(newest_kernel "$STAGE_ROOT")
	[ -n "$k" ] || die "no kernel found in $STAGE_ROOT/boot"
	rel=$(kernel_release "$k")
	committed=$(cat "$FW/kernel_$slot.release" 2>/dev/null || echo "")
	if [ "$rel" = "$committed" ]; then
		clear_pending
		printf 'boot: default kernel %s\n' "$rel"
		return 0
	fi
	cmd_kernel_try
}

cmd_kernel_try() {
	cur=$(active_subvol)
	slot=$(slot_of "$cur")
	k=$(newest_kernel "$STAGE_ROOT")
	[ -n "$k" ] || die "no kernel found in $STAGE_ROOT/boot"
	rel=$(kernel_release "$k")
	committed=$(cat "$FW/kernel_$slot.release" 2>/dev/null || echo "")
	if [ "$rel" = "$committed" ]; then
		printf 'boot: no newer kernel to try (default %s)\n' "$rel"
		return 0
	fi
	[ -f "$FW/cmdline_$slot.txt" ] || cp "$FW/cmdline.txt" "$FW/cmdline_$slot.txt"
	copy_kernel "$STAGE_ROOT" "$slot" ".next" >/dev/null
	write_boot_cfg "$FW/tryboot.txt" "$slot" ".next" "cmdline_$slot.txt"
	pending_set "$cur" "vmlinuz_$slot.next" "$rel"
	sync
	printf 'boot: kernel %s will be tried once on next boot (fallback %s); reboot with `reboot "0 tryboot"`, then run `salt-ota confirm`\n' \
		"$rel" "${committed:-none}"
}

cmd_kernel_confirm() {
	if [ ! -f "$FW/tryboot.txt" ]; then
		slot=$(slot_of "$(active_subvol)")
		printf 'boot: nothing to confirm (default kernel %s)\n' "$(cat "$FW/kernel_$slot.release" 2>/dev/null || echo unknown)"
		return 0
	fi
	cmd_commit
}

cmd_kernel_status() {
	cur=$(active_subvol)
	slot=$(slot_of "$cur")
	printf 'loader:   tryboot\n'
	printf 'running:  %s (root %s)\n' "$(running_release)" "$cur"
	printf 'default:  %s\n' "$(cat "$FW/kernel_$slot.release" 2>/dev/null || echo '(unset)')"
	if [ -f "$FW/tryboot.txt" ]; then
		rel=$(pending_get release || echo '?')
		want=$(pending_get subvol || echo "$cur")
		if [ "$want" = "$cur" ] && [ "$(running_release)" = "$rel" ]; then
			printf 'pending:  %s (booted, awaiting confirm)\n' "$rel"
		else
			printf 'pending:  %s in %s (armed for next boot)\n' "$rel" "$want"
		fi
		return 3
	fi
	printf 'pending:  none\n'
	return 0
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
	kernel-update) cmd_kernel_update "$@" ;;
	kernel-try) cmd_kernel_try "$@" ;;
	kernel-confirm) cmd_kernel_confirm "$@" ;;
	kernel-status) cmd_kernel_status "$@" ;;
	-h|--help|help)
		cat <<EOF
usage: $prog <command>

  status          show active/standby subvol, slot kernels and tryboot state
  prepare         snapshot active root into standby subvol, print its mountpoint
  finalize        copy the standby root's kernel to its slot and arm a one-shot tryboot
  commit          make the tried boot (root and kernel) the permanent default
  abort           clear a pending tryboot
  active          print the currently booted subvol
  kernel-update   after a transaction touched /boot: stage the new kernel for one trial boot
  kernel-try      re-arm a one-shot tryboot of the newest installed kernel
  kernel-confirm  confirm the tried kernel/root (fails and clears it if the boot fell back)
  kernel-status   loader/running/default/pending summary (exit 3 while a trial is pending)
EOF
	;;
	*) die "unknown command: $cmd (try '$prog --help')" ;;
esac
