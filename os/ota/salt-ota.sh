#!/bin/sh
set -u

prog=${0##*/}
SALT=${SALT_BIN:-salt}
AB=${SALTOS_AB_TOOL:-/usr/lib/saltos/ab-update.sh}
STATE_DIR=${SALT_STATE_DIR:-/var/lib/salt}
# salt.lua is the config; systems installed before the Lua switch only have the
# legacy TOML salt.conf, which is still read when salt.lua is absent.
if [ -n "${SALTOS_CONF:-}" ]; then
	CONF=$SALTOS_CONF
elif [ ! -f /etc/salt/salt.lua ] && [ -f /etc/salt/salt.conf ]; then
	CONF=/etc/salt/salt.conf
else
	CONF=/etc/salt/salt.lua
fi
LOG=${SALTOS_OTA_LOG:-/var/log/salt-ota.log}
LOCK=${SALTOS_OTA_LOCK:-/run/salt-ota.lock}
STATE=${SALTOS_OTA_STATE:-$STATE_DIR/ota-state}
HEALTH_DIR=${SALTOS_HEALTH_DIR:-/etc/salt/health.d}

E_NOTHING=0
E_ERROR=1
E_UPDATED=2
E_REBOOT=3
E_ROLLED_BACK=4
E_UNHEALTHY=5

now() { date -u +%Y-%m-%dT%H:%M:%SZ; }
log() {
	msg="$(now) $prog: $1"
	printf '%s\n' "$msg" >&2
	if log_writable; then
		printf '%s\n' "$msg" >>"$LOG"
	fi
}
die() { log "$1"; exit "${2:-$E_ERROR}"; }

log_writable() {
	[ -n "$LOG" ] && { [ -w "$LOG" ] || { [ ! -e "$LOG" ] && [ -w "${LOG%/*}" ]; }; }
}

run_logged() {
	log_writable || { "$@" >&2; return $?; }
	st=$(mktemp "${TMPDIR:-/tmp}/salt-ota.XXXXXX") || { "$@" >&2; return $?; }
	{ "$@" 2>&1; echo $? >"$st"; } | tee -a "$LOG" >&2
	rc=$(cat "$st" 2>/dev/null)
	rm -f "$st"
	return "${rc:-1}"
}

conf_val() {
	key=$1; def=$2
	[ -f "$CONF" ] || { echo "$def"; return; }
	case $CONF in
	*.lua)
		v=$("$SALT" eval "$CONF" "ota.$key" 2>/dev/null) || v=
		;;
	*)
		v=$(awk -F= -v k="$key" '
			/^[[:space:]]*\[/ { sect=$0; gsub(/[][[:space:]]/, "", sect) }
			sect == "ota" && $1 ~ "^[[:space:]]*"k"[[:space:]]*$" {
				gsub(/^[[:space:]]*"|"[[:space:]]*$/, "", $2);
				gsub(/^[[:space:]]*|[[:space:]]*$/, "", $2);
				print $2; exit
			}' "$CONF" 2>/dev/null)
		;;
	esac
	[ -n "$v" ] && echo "$v" || echo "$def"
}

ota_enabled() { [ "$(conf_val enabled true)" = "true" ]; }
ota_interval() { conf_val interval 86400; }
reboot_on_kernel() { [ "$(conf_val reboot_on_kernel false)" = "true" ]; }
ab_mode() { [ "$(conf_val ab false)" = "true" ] && [ -x "$AB" ] && [ -d "${SALTOS_FW_DIR:-/boot/firmware}" ]; }

save_state() {
	mkdir -p "${STATE%/*}" 2>/dev/null || return 0
	{
		printf 'last_run=%s\n' "$(now)"
		printf 'last_result=%s\n' "$1"
		printf 'last_exit=%s\n' "$2"
	} >"$STATE.tmp" 2>/dev/null && mv "$STATE.tmp" "$STATE" 2>/dev/null || true
}

with_lock() {
	command -v flock >/dev/null 2>&1 || die "flock not found; cannot serialize OTA runs"
	exec 9>"$LOCK" || die "cannot open lock $LOCK"
	flock -n 9 || die "another $prog run holds $LOCK" "$E_ERROR"
}

reboot_required() {
	"$SALT" boot status >/dev/null 2>&1
	[ $? -eq 3 ]
}

maybe_reboot() {
	if [ "$REBOOT_POLICY" = "yes" ] || { [ "$REBOOT_POLICY" = "conf" ] && reboot_on_kernel; }; then
		log "rebooting to try the new kernel/root (reboot_on_kernel policy)"
		sync
		if [ -n "${REBOOT_ARG:-}" ]; then
			reboot "$REBOOT_ARG"
		else
			reboot
		fi
	else
		log "reboot required to activate the update; run 'reboot' then 'salt-ota confirm'"
	fi
}

cmd_check() {
	with_lock
	ota_enabled || { log "ota disabled in $CONF"; return "$E_NOTHING"; }
	run_logged "$SALT" --yes sync || die "sync failed" "$E_ERROR"
	out=$("$SALT" update --check 2>&1)
	rc=$?
	printf '%s\n' "$out"
	case $rc in
		0) log "check: everything is up to date"; return "$E_NOTHING" ;;
		100) log "check: updates available"; return "$E_UPDATED" ;;
		*) die "check failed: $out" "$E_ERROR" ;;
	esac
}

run_snapshot_update() {
	run_logged "$SALT" --yes sync || die "sync failed; system unchanged" "$E_ERROR"
	"$SALT" update --check >/dev/null 2>&1
	case $? in
		0) log "everything is up to date"; save_state nothing "$E_NOTHING"; return "$E_NOTHING" ;;
		100) ;;
		*) die "cannot evaluate updates; system unchanged" "$E_ERROR" ;;
	esac
	before=$("$SALT" deployments 2>/dev/null | awk '/^\*/{print $2; exit}')
	if run_logged "$SALT" --yes update; then
		after=$("$SALT" deployments 2>/dev/null | awk '/^\*/{print $2; exit}')
		log "host updated (deployment ${before:-?} -> ${after:-?})"
		if reboot_required; then
			save_state updated-reboot-required "$E_REBOOT"
			maybe_reboot
			return "$E_REBOOT"
		fi
		save_state updated "$E_UPDATED"
		return "$E_UPDATED"
	fi
	log "update failed; salt reverted the transaction, system unchanged"
	save_state failed-rolled-back "$E_ROLLED_BACK"
	return "$E_ROLLED_BACK"
}

run_ab_update() {
	stage=$("$AB" prepare) || die "A/B prepare failed; system unchanged" "$E_ERROR"
	log "staging update into $stage"
	if ! run_logged "$SALT" --yes --root "$stage" sync; then
		"$AB" abort || true
		die "sync failed; system unchanged" "$E_ERROR"
	fi
	"$SALT" --root "$stage" update --check >/dev/null 2>&1
	case $? in
		0) "$AB" abort || true; log "everything is up to date"; save_state nothing "$E_NOTHING"; return "$E_NOTHING" ;;
		100) ;;
		*) "$AB" abort || true; die "cannot evaluate updates; system unchanged" "$E_ERROR" ;;
	esac
	if run_logged "$SALT" --yes --root "$stage" update; then
		if "$AB" finalize "$stage"; then
			log "A/B update staged; the new root will be tried once on the next boot"
			save_state updated-reboot-required "$E_REBOOT"
			REBOOT_ARG="0 tryboot"
			maybe_reboot
			return "$E_REBOOT"
		fi
		log "A/B finalize failed; discarding the staged root"
		"$AB" abort || true
		save_state failed-rolled-back "$E_ROLLED_BACK"
		return "$E_ROLLED_BACK"
	fi
	log "update failed in the staged root; discarding it, system unchanged"
	"$AB" abort || true
	save_state failed-rolled-back "$E_ROLLED_BACK"
	return "$E_ROLLED_BACK"
}

cmd_run() {
	REBOOT_POLICY=conf
	for a in "$@"; do
		case $a in
			--reboot) REBOOT_POLICY=yes ;;
			--no-reboot) REBOOT_POLICY=no ;;
			*) die "unknown option for run: $a" ;;
		esac
	done
	with_lock
	ota_enabled || { log "ota disabled in $CONF"; return "$E_NOTHING"; }
	[ "$(id -u)" -eq 0 ] || [ "$STATE_DIR" != /var/lib/salt ] || die "run requires root"
	if ab_mode; then
		run_ab_update
	else
		run_snapshot_update
	fi
}

health_ok() {
	[ -d "$HEALTH_DIR" ] || return 0
	for h in "$HEALTH_DIR"/*; do
		[ -x "$h" ] || continue
		if ! run_logged "$h"; then
			log "health check failed: $h"
			return 1
		fi
	done
	return 0
}

cmd_confirm() {
	fallback=no
	for a in "$@"; do
		case $a in
			--fallback) fallback=yes ;;
			*) die "unknown option for confirm: $a" ;;
		esac
	done
	with_lock
	if ! health_ok; then
		if reboot_required || { ab_mode && "$AB" status 2>/dev/null | grep -q '^tryboot *: *pending'; }; then
			log "not confirming the trial boot"
			if [ "$fallback" = yes ]; then
				log "rebooting into the previous kernel/root"
				sync
				reboot
			fi
		else
			log "system unhealthy; nothing pending to fall back to"
		fi
		return "$E_UNHEALTHY"
	fi
	rc=0
	if ab_mode; then
		run_logged "$AB" commit || rc=1
	fi
	run_logged "$SALT" boot confirm || rc=1
	if [ $rc -eq 0 ]; then log "boot confirmed"; else log "confirm failed"; fi
	return $rc
}

cmd_status() {
	printf 'ota.enabled         : %s\n' "$(conf_val enabled true)"
	printf 'ota.interval        : %s s\n' "$(ota_interval)"
	printf 'ota.reboot_on_kernel: %s\n' "$(conf_val reboot_on_kernel false)"
	printf 'mode                : %s\n' "$(ab_mode && echo 'A/B root (tryboot)' || echo 'btrfs generations + GRUB trial boot')"
	if [ -f "$STATE" ]; then
		sed 's/^/ota.state.          /; s/=/: /' "$STATE"
	else
		printf 'ota.state           : never run\n'
	fi
	printf 'lock                : %s\n' "$( { exec 9>"$LOCK"; } 2>/dev/null && { flock -n 9 && echo free || echo 'held (run in progress)'; } )"
	printf 'log                 : %s\n' "$LOG"
	printf -- '--- boot\n'
	"$SALT" boot status 2>&1 | sed 's/^/  /'
	if ab_mode; then
		"$AB" status 2>&1 | sed 's/^/  /'
	fi
	printf -- '--- deployments\n'
	"$SALT" deployments 2>&1 | head -n 20 | sed 's/^/  /'
}

cmd=${1:-status}
shift 2>/dev/null || true
case "$cmd" in
	run) cmd_run "$@" ;;
	check) cmd_check "$@" ;;
	status) cmd_status "$@" ;;
	confirm|commit) cmd_confirm "$@" ;;
	interval) ota_interval ;;
	-h|--help|help)
		cat <<EOF
usage: $prog <command>

  run [--reboot|--no-reboot]
            sync, apply all updates atomically (salt snapshots the root before
            the transaction and reverts it on failure); arm a new kernel/root
            for one trial boot and reboot according to ota.reboot_on_kernel
  check     sync and report available updates without applying them
  status    show OTA configuration, last run, boot trial state and deployments
  confirm [--fallback]
            after a successful boot, run $HEALTH_DIR/* and make the tried
            kernel/root the default; with --fallback an unhealthy trial boot
            reboots into the previous kernel/root instead
  interval  print the configured auto-update interval in seconds

exit codes: 0 nothing to do, 1 error (unchanged), 2 updated,
            3 updated + reboot required, 4 failed and rolled back,
            5 health check failed (confirm)
log: $LOG
EOF
	;;
	*) die "unknown command: $cmd" ;;
esac
