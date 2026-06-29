#!/bin/sh
set -eu

prog=${0##*/}
SALT=${SALT_BIN:-salt}
SNAPSHOT=${SALTOS_SNAPSHOT:-/usr/lib/saltos/snapshot.sh}
AB=${SALTOS_AB_TOOL:-/usr/lib/saltos/ab-update.sh}
STATE_DIR=${SALT_STATE_DIR:-/var/lib/salt}
CONF=${SALTOS_CONF:-/etc/salt/salt.conf}

die() { printf '%s: %s\n' "$prog" "$1" >&2; exit 1; }
log() { printf '%s: %s\n' "$prog" "$1" >&2; }

conf_val() {
	key=$1; def=$2
	[ -f "$CONF" ] || { echo "$def"; return; }
	v=$(awk -F= -v k="$key" '
		$1 ~ "^[[:space:]]*"k"[[:space:]]*$" {
			gsub(/^[[:space:]]*"|"[[:space:]]*$/, "", $2);
			gsub(/^[[:space:]]*|[[:space:]]*$/, "", $2);
			print $2; exit
		}' "$CONF" 2>/dev/null)
	[ -n "$v" ] && echo "$v" || echo "$def"
}

ota_enabled() { [ "$(conf_val enabled true)" = "true" ]; }
ota_interval() { conf_val interval 86400; }
ab_available() { [ -x "$AB" ] && [ -d "${SALTOS_FW_DIR:-/boot/firmware}" ]; }

cmd_status() {
	printf 'ota.enabled : %s\n' "$(conf_val enabled true)"
	printf 'ota.interval: %s s\n' "$(ota_interval)"
	if ab_available; then
		"$AB" status
	else
		printf 'mode        : snapshot+rollback (no A/B firmware)\n'
		if [ -x "$SNAPSHOT" ]; then
			printf 'snapshots   :\n'
			"$SNAPSHOT" list 2>/dev/null | sed 's/^/  /' || true
		fi
	fi
}

run_snapshot_update() {
	id="ota-$(date -u +%Y%m%dT%H%M%SZ)"
	snap=""
	if [ -x "$SNAPSHOT" ] && [ "$(id -u)" -eq 0 ]; then
		snap=$("$SNAPSHOT" create "$id" 2>/dev/null) || snap=""
		[ -n "$snap" ] && log "pre-update snapshot: $snap"
	fi
	if ! "$SALT" --yes sync; then
		die "sync failed; aborting update"
	fi
	if "$SALT" --yes update; then
		log "host updated"
		return 0
	fi
	log "update failed"
	if [ -n "$snap" ]; then
		log "rolling back to $snap"
		"$SNAPSHOT" rollback "$snap" || log "rollback command failed; manual recovery needed"
	fi
	return 1
}

run_ab_update() {
	stage=$("$AB" prepare) || die "A/B prepare failed"
	log "staging update into $stage"
	if "$SALT" --yes --root "$stage" sync && "$SALT" --yes --root "$stage" update; then
		"$AB" finalize "$stage"
		log "A/B update staged; system will tryboot the new root"
		if [ "${SALTOS_AB_REBOOT:-0}" = "1" ]; then
			sync
			reboot "0 tryboot"
		fi
		return 0
	fi
	log "A/B update failed; aborting tryboot"
	"$AB" abort || true
	return 1
}

cmd_run() {
	ota_enabled || { log "ota disabled in $CONF"; return 0; }
	if ab_available && [ "${SALTOS_AB:-0}" = "1" ]; then
		run_ab_update
	else
		run_snapshot_update
	fi
}

cmd_confirm() {
	ab_available || { log "no A/B firmware; nothing to confirm"; return 0; }
	"$AB" commit
}

cmd=${1:-status}
shift 2>/dev/null || true
case "$cmd" in
	run) cmd_run "$@" ;;
	status) cmd_status "$@" ;;
	confirm|commit) cmd_confirm "$@" ;;
	interval) ota_interval ;;
	-h|--help|help)
		cat <<EOF
usage: $prog <command>

  run       snapshot (or A/B stage), sync, update, rollback on failure
  status    show OTA configuration and snapshot/boot state
  confirm   commit a pending A/B tryboot as the new default
  interval  print configured auto-update interval in seconds
EOF
	;;
	*) die "unknown command: $cmd" ;;
esac
