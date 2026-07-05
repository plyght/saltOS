#!/bin/sh
set -eu

prog=${0##*/}

DEVICE=""
MOUNTPOINT="/mnt"
FSTAB=""
COMPRESS="zstd:3"
DO_FORMAT=0
LABEL="saltos"

usage() {
	cat <<EOF
usage: $prog -d <device> [-m <mountpoint>] [options]

Create the saltOS Btrfs subvolume layout and emit matching fstab entries.

required:
  -d <device>      block device or existing btrfs (e.g. /dev/nvme0n1p2)

options:
  -m <mountpoint>  where to mount during setup (default: $MOUNTPOINT)
  -f               mkfs.btrfs the device first (DESTROYS DATA)
  -L <label>       filesystem label when formatting (default: $LABEL)
  -c <compress>    compression algorithm (default: $COMPRESS)
  -o <file>        also write generated fstab entries to <file>
  -h               show this help

Subvolumes created: @ @home @var @log @snapshots @strata
EOF
}

die() {
	printf '%s: %s\n' "$prog" "$1" >&2
	exit 1
}

while getopts d:m:fL:c:o:h opt; do
	case "$opt" in
		d) DEVICE=$OPTARG ;;
		m) MOUNTPOINT=$OPTARG ;;
		f) DO_FORMAT=1 ;;
		L) LABEL=$OPTARG ;;
		c) COMPRESS=$OPTARG ;;
		o) FSTAB=$OPTARG ;;
		h) usage; exit 0 ;;
		*) usage >&2; exit 1 ;;
	esac
done

[ -n "$DEVICE" ] || { usage >&2; exit 1; }
[ -b "$DEVICE" ] || die "not a block device: $DEVICE"
command -v btrfs >/dev/null 2>&1 || die "btrfs-progs not found"
[ "$(id -u)" -eq 0 ] || die "must run as root"

if [ "$DO_FORMAT" -eq 1 ]; then
	command -v mkfs.btrfs >/dev/null 2>&1 || die "mkfs.btrfs not found"
	mkfs.btrfs -f -L "$LABEL" "$DEVICE"
fi

TOPLEVEL="$MOUNTPOINT/.btrfs-top"
mkdir -p "$TOPLEVEL"
mount -o subvolid=5 "$DEVICE" "$TOPLEVEL"

cleanup() {
	umount "$TOPLEVEL" 2>/dev/null || true
	rmdir "$TOPLEVEL" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for sub in @ @home @var @log @snapshots @strata; do
	if [ -d "$TOPLEVEL/$sub" ]; then
		printf '%s: subvolume already exists: %s\n' "$prog" "$sub"
	else
		btrfs subvolume create "$TOPLEVEL/$sub"
	fi
done

umount "$TOPLEVEL"
trap - EXIT INT TERM
rmdir "$TOPLEVEL" 2>/dev/null || true

COMMON="rw,noatime,compress=$COMPRESS,space_cache=v2"

mkdir -p "$MOUNTPOINT"
mount -o "$COMMON,subvol=@" "$DEVICE" "$MOUNTPOINT"
mkdir -p "$MOUNTPOINT/home" "$MOUNTPOINT/var" "$MOUNTPOINT/var/log" "$MOUNTPOINT/.snapshots" "$MOUNTPOINT/strata"
mount -o "$COMMON,subvol=@home" "$DEVICE" "$MOUNTPOINT/home"
mount -o "$COMMON,subvol=@var" "$DEVICE" "$MOUNTPOINT/var"
mkdir -p "$MOUNTPOINT/var/log"
mount -o "$COMMON,subvol=@log" "$DEVICE" "$MOUNTPOINT/var/log"
mount -o "$COMMON,subvol=@snapshots" "$DEVICE" "$MOUNTPOINT/.snapshots"
mount -o "$COMMON,subvol=@strata" "$DEVICE" "$MOUNTPOINT/strata"

UUID=$(blkid -s UUID -o value "$DEVICE" 2>/dev/null || true)
if [ -n "$UUID" ]; then
	SRC="UUID=$UUID"
else
	SRC="$DEVICE"
fi

emit_fstab() {
	cat <<EOF
$SRC  /            btrfs  $COMMON,subvol=@           0 0
$SRC  /home        btrfs  $COMMON,subvol=@home       0 0
$SRC  /var         btrfs  $COMMON,subvol=@var        0 0
$SRC  /var/log     btrfs  $COMMON,subvol=@log        0 0
$SRC  /.snapshots  btrfs  $COMMON,subvol=@snapshots  0 0
$SRC  /strata      btrfs  $COMMON,subvol=@strata     0 0
EOF
}

emit_fstab

if [ -n "$FSTAB" ]; then
	emit_fstab > "$FSTAB"
	printf '%s: wrote fstab entries to %s\n' "$prog" "$FSTAB" >&2
fi
