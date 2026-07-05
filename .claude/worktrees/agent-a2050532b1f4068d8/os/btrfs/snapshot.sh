#!/bin/sh
set -eu

prog=${0##*/}

ROOT_SUBVOL=${SALT_ROOT_SUBVOL:-/}
SNAP_DIR=${SALT_SNAPSHOT_DIR:-/.snapshots}
STATE_DIR=${SALT_STATE_DIR:-/var/lib/salt}
PREFIX=root

usage() {
	cat <<EOF
usage: $prog <command> [args]

commands:
  create <id>        snapshot $ROOT_SUBVOL read-only into $SNAP_DIR as
                     ${PREFIX}-<id>-<timestamp>; prints the snapshot name
  list               list snapshots in $SNAP_DIR, newest first
  delete <name>      delete snapshot <name> from $SNAP_DIR
  rollback <name>    make snapshot <name> the active root for next boot

environment:
  SALT_ROOT_SUBVOL   active root subvolume mount (default: $ROOT_SUBVOL)
  SALT_SNAPSHOT_DIR  snapshot directory (default: $SNAP_DIR)
  SALT_STATE_DIR     salt state dir (default: $STATE_DIR)
EOF
}

die() {
	printf '%s: %s\n' "$prog" "$1" >&2
	exit 1
}

need_root() {
	[ "$(id -u)" -eq 0 ] || die "must run as root"
}

need_btrfs() {
	command -v btrfs >/dev/null 2>&1 || die "btrfs-progs not found"
}

is_subvol() {
	btrfs subvolume show "$1" >/dev/null 2>&1
}

timestamp() {
	date -u +%Y%m%dT%H%M%SZ
}

cmd_create() {
	id=${1:-}
	[ -n "$id" ] || die "create requires a transaction id"
	need_root
	need_btrfs
	is_subvol "$ROOT_SUBVOL" || die "$ROOT_SUBVOL is not a btrfs subvolume"
	mkdir -p "$SNAP_DIR"
	name="${PREFIX}-${id}-$(timestamp)"
	dest="$SNAP_DIR/$name"
	[ -e "$dest" ] && die "snapshot already exists: $name"
	btrfs subvolume snapshot -r "$ROOT_SUBVOL" "$dest" >&2
	if [ -d "$STATE_DIR" ]; then
		printf '%s\t%s\t%s\n' "$(timestamp)" "$id" "$name" >> "$STATE_DIR/snapshots.log" 2>/dev/null || true
	fi
	printf '%s\n' "$name"
}

cmd_list() {
	need_btrfs
	[ -d "$SNAP_DIR" ] || { printf '%s: no snapshot directory: %s\n' "$prog" "$SNAP_DIR" >&2; return 0; }
	found=0
	for s in "$SNAP_DIR/$PREFIX"-*; do
		[ -e "$s" ] || continue
		found=1
		printf '%s\n' "${s##*/}"
	done | sort -r
	[ "$found" -eq 1 ] || printf '%s: no snapshots found in %s\n' "$prog" "$SNAP_DIR" >&2
}

cmd_delete() {
	name=${1:-}
	[ -n "$name" ] || die "delete requires a snapshot name"
	need_root
	need_btrfs
	case "$name" in
		*/*) die "snapshot name must not contain '/'" ;;
	esac
	target="$SNAP_DIR/$name"
	[ -e "$target" ] || die "no such snapshot: $name"
	is_subvol "$target" || die "$target is not a btrfs subvolume"
	btrfs subvolume delete "$target" >&2
	printf '%s: deleted %s\n' "$prog" "$name" >&2
}

cmd_rollback() {
	name=${1:-}
	[ -n "$name" ] || die "rollback requires a snapshot name"
	need_root
	need_btrfs
	case "$name" in
		*/*) die "snapshot name must not contain '/'" ;;
	esac
	src="$SNAP_DIR/$name"
	[ -e "$src" ] || die "no such snapshot: $name"
	is_subvol "$src" || die "$src is not a btrfs subvolume"

	restore="${PREFIX}-rollback-from-$name-$(timestamp)"
	btrfs subvolume snapshot -r "$ROOT_SUBVOL" "$SNAP_DIR/$restore" >&2

	rw="$SNAP_DIR/${name}-active"
	[ -e "$rw" ] && die "rollback target already prepared: $rw"
	btrfs subvolume snapshot "$src" "$rw" >&2

	newid=$(btrfs inspect-internal rootid "$rw")
	btrfs subvolume set-default "$newid" "$SNAP_DIR" >&2

	printf '%s: default subvolume set to %s (id %s)\n' "$prog" "$rw" "$newid" >&2
	printf '%s: previous root saved as %s; reboot to complete rollback\n' "$prog" "$restore" >&2
	printf '%s\n' "$rw"
}

cmd=${1:-}
[ -n "$cmd" ] || { usage >&2; exit 1; }
shift || true

case "$cmd" in
	create)   cmd_create "$@" ;;
	list)     cmd_list "$@" ;;
	delete)   cmd_delete "$@" ;;
	rollback) cmd_rollback "$@" ;;
	-h|--help|help) usage ;;
	*) die "unknown command: $cmd (try '$prog --help')" ;;
esac
