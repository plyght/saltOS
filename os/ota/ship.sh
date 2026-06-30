#!/bin/sh
# One-command OTA publish: from a salt binary (and optional kernel), build the
# signed base-grain repo and serve it over HTTP -- keygen, grain build, sign and
# serve in a single step. The matching client side is one command too: `salt
# update` (after a one-time repo.conf; see docs/ota.md).
#
#   SALT=build/src/salt/salt sh os/ota/ship.sh
#   SALT=... KERNEL=out/Image VERSION=0.1.2 PORT=8099 sh os/ota/ship.sh ./ota-repo
#
# Env: SALT (required, the salt binary), KERNEL (optional kernel grain), VERSION,
#      PORT (default 8099), OTA_KEYS (key dir, default ./ota-keys). Arg 1 = repo
#      dir (default ./ota-repo). The signing key is created once and reused;
#      keep OTA_KEYS/ota.sec secret and bake ota.pub into clients.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SALT=${SALT:?set SALT to the salt binary}
REPO=${1:-${OTA_REPO:-./ota-repo}}
KEYS=${OTA_KEYS:-./ota-keys}
VERSION=${VERSION:-0.1.0}
PORT=${PORT:-8099}

# 1. signing key -- create once, reuse thereafter.
if [ ! -f "$KEYS/ota.sec" ]; then
  "$SALT" keygen "$KEYS" ota
  echo "ship: created signing key in $KEYS (keep ota.sec secret; ship ota.pub to clients)"
fi
SEC=$(cat "$KEYS"/*.sec | head -1)

# 2. build + sign the base grains (salt, saltos-base, and the kernel if given).
SALT="$SALT" SEC_KEY="$SEC" VERSION="$VERSION" OUT="$REPO" ${KERNEL:+KERNEL="$KERNEL"} \
  sh "$HERE/../selfhost/build-base-grains.sh"

ARCH=$("$SALT" --version | sed -E 's/.*\((.*)\)/\1/')
cat <<EOF
ship: published $REPO ($ARCH), version $VERSION.
ship: point a client at it (one time):
        printf 'repo = "current"\\nsource = "http://<this-host>:$PORT"\\nkey = "/etc/salt/keys/ota.pub"\\n' > /etc/salt/repo.conf
        # copy $KEYS/ota.pub to the client's /etc/salt/keys/ota.pub
ship: then on the client just run:  salt update
ship: serving on 0.0.0.0:$PORT (Ctrl-C to stop)
EOF
exec python3 -m http.server "$PORT" --bind 0.0.0.0 --directory "$REPO"
