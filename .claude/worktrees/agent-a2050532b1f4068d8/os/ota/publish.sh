#!/bin/sh
set -eu

prog=${0##*/}
SALT=${SALT_BIN:-salt}
REPO_DIR=${1:-${REPO_DIR:-./repo}}
SECRET_KEY=${OTA_SECRET_KEY:-}

die() { printf '%s: %s\n' "$prog" "$1" >&2; exit 1; }

[ -d "$REPO_DIR" ] || die "repo dir not found: $REPO_DIR (expected <dir>/<arch>/packages/*.grain)"

if [ -n "$SECRET_KEY" ]; then
	[ -f "$SECRET_KEY" ] || die "secret key not found: $SECRET_KEY"
	"$SALT" --key "$SECRET_KEY" repo publish "$REPO_DIR"
else
	printf '%s: warning: no OTA_SECRET_KEY set; publishing an UNSIGNED index\n' "$prog" >&2
	"$SALT" repo publish "$REPO_DIR"
fi

printf '%s: published. serve %s and set source = "<base-url>" in clients\n' "$prog" "$REPO_DIR" >&2
printf '%s: clients fetch <base-url>/<arch>/index.toml\n' "$prog" >&2
