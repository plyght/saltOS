#!/bin/sh
set -eu

SALT_BIN=$1
OTA=$2
WORK=$3
ARCH=$("$SALT_BIN" --version | sed -E 's/.*\((.*)\)/\1/')

rm -rf "$WORK"
mkdir -p "$WORK/src" "$WORK/recipes" "$WORK/repo" "$WORK/root/etc/salt" "$WORK/bin"
touch "$WORK/src/.keep"
ROOT="$WORK/root"
REPO="$WORK/repo"
export SALT_OUT="$WORK/out" SALT_WORK="$WORK/work"

fail() { echo "ota_smoke: FAIL: $1" >&2; exit 1; }
step() { echo "ota_smoke: $1"; }

recipe() {
	name=$1; ver=$2; payload=$3
	mkdir -p "$WORK/recipes/$name-$ver"
	cat > "$WORK/recipes/$name-$ver/recipe.toml" <<EOF
name = "$name"
version = "$ver"
release = 1
arch = ["x86_64", "aarch64"]
summary = "ota smoke package"
license = "MIT"

[source]
url = "file://$WORK/src"
sha256 = ""

[build]
system = "custom"
script = """
$payload
"""

[package]
deps = []
EOF
	"$SALT_BIN" build "$WORK/recipes/$name-$ver" >/dev/null
}

publish() {
	key=$1; shift
	rm -rf "$REPO/$ARCH"
	mkdir -p "$REPO/$ARCH/packages"
	for g in "$@"; do cp "$SALT_OUT/$ARCH/packages/$g" "$REPO/$ARCH/packages/"; done
	"$SALT_BIN" --key "$key" repo publish "$REPO/$ARCH" >/dev/null
}

hello_payload() {
	printf 'mkdir -p "$SALT_DEST/usr/bin"\n{ echo "#!/bin/sh"; echo "echo hi %s"; } > "$SALT_DEST/usr/bin/hello"\nchmod +x "$SALT_DEST/usr/bin/hello"\n' "$1"
}

step "building hello 1.0 / 1.1 / 1.2 and salt 0.1.0 / 0.1.1 grains"
recipe hello 1.0 "$(hello_payload 1.0)"
recipe hello 1.1 "$(hello_payload 1.1)"
recipe hello 1.2 "$(hello_payload 1.2)"
cp "$SALT_BIN" "$WORK/src/salt"
recipe salt 0.1.0 'mkdir -p "$SALT_DEST/usr/bin"; cp "$SALT_SRC/salt" "$SALT_DEST/usr/bin/salt"; chmod 0755 "$SALT_DEST/usr/bin/salt"'
printf '\n' >> "$WORK/src/salt"
recipe salt 0.1.1 'mkdir -p "$SALT_DEST/usr/bin"; cp "$SALT_SRC/salt" "$SALT_DEST/usr/bin/salt"; chmod 0755 "$SALT_DEST/usr/bin/salt"'
G() { echo "$1-$2-1-$ARCH.grain"; }

"$SALT_BIN" keygen "$WORK/keys" repo >/dev/null
"$SALT_BIN" keygen "$WORK/keys" other >/dev/null
PUB=$(head -1 "$WORK/keys/repo.pub")
printf 'repo = "current"\nsource = "file://%s"\nkey = "%s"\n' "$REPO" "$PUB" > "$ROOT/etc/salt/repo.conf"
printf '[ota]\nenabled = true\ninterval = "60"\nreboot_on_kernel = false\n\n[deploy]\nkeep = 2\n' > "$ROOT/etc/salt/salt.conf"

cat > "$WORK/bin/salt" <<EOF
#!/bin/sh
exec "$SALT_BIN" --root "$ROOT" "\$@"
EOF
chmod +x "$WORK/bin/salt"
export SALT_BIN_WRAPPED="$WORK/bin/salt"
export SALT_STATE_DIR="$ROOT/var/lib/salt" SALTOS_CONF="$ROOT/etc/salt/salt.conf"
export SALTOS_OTA_LOG="$WORK/salt-ota.log" SALTOS_OTA_LOCK="$WORK/ota.lock"
export SALTOS_OTA_STATE="$ROOT/var/lib/salt/ota-state" SALTOS_HEALTH_DIR="$WORK/health.d"
ota() { SALT_BIN="$SALT_BIN_WRAPPED" sh "$OTA" "$@"; }
S() { "$SALT_BIN" --root "$ROOT" "$@"; }

step "deployment 1: install hello 1.0 + salt 0.1.0"
publish "$WORK/keys/repo.sec" "$(G hello 1.0)" "$(G salt 0.1.0)"
S sync >/dev/null
S --yes install hello salt >/dev/null
[ "$(sh "$ROOT/usr/bin/hello")" = "hi 1.0" ] || fail "hello 1.0 not installed"
cmp -s "$SALT_BIN" "$ROOT/usr/bin/salt" || fail "salt 0.1.0 payload differs"

step "update --check with nothing new exits 0; salt-ota check/run exit 0"
S update --check >/dev/null || fail "update --check should exit 0 when current"
ota check >/dev/null 2>&1 || fail "salt-ota check should exit 0 when current"
ota run --no-reboot >/dev/null 2>&1 || fail "salt-ota run should exit 0 when current"
ota status >/dev/null 2>&1 || fail "salt-ota status failed"

step "deployment 2: salt-ota run applies hello 1.1 + salt 0.1.1 (rc 2), self-update via rename-over"
publish "$WORK/keys/repo.sec" "$(G hello 1.1)" "$(G salt 0.1.1)"
S sync >/dev/null
set +e
S update --check >/dev/null; rc=$?
set -e
[ "$rc" -eq 100 ] || fail "update --check should exit 100 when updates exist (got $rc)"
set +e
ota check >/dev/null 2>&1; rc=$?
set -e
[ "$rc" -eq 2 ] || fail "salt-ota check should exit 2 when updates exist (got $rc)"
[ "$(sh "$ROOT/usr/bin/hello")" = "hi 1.0" ] || fail "check must not apply anything"
set +e
"$ROOT/usr/bin/salt" --root "$ROOT" --yes update >"$WORK/selfupdate.log" 2>&1; rc=$?
set -e
[ "$rc" -eq 0 ] || { cat "$WORK/selfupdate.log"; fail "self-update transaction failed"; }
cmp -s "$WORK/src/salt" "$ROOT/usr/bin/salt" || fail "running salt did not replace itself with salt 0.1.1"
[ "$(sh "$ROOT/usr/bin/hello")" = "hi 1.1" ] || fail "hello 1.1 not active after update"
"$ROOT/usr/bin/salt" --root "$ROOT" list | grep -q '^salt *0.1.1-1' || fail "new salt binary does not run / list 0.1.1"
S deployments | grep -q 'hello 1.0-1 -> 1.1-1' || fail "deployments does not list the hello change"
S deployments | grep -q 'salt 0.1.0-1 -> 0.1.1-1' || fail "deployments does not list the salt change"
S pin 2 >/dev/null || fail "pin 2 failed"
S deployments | grep -q '^.P 2 ' || fail "deployment 2 not shown as pinned"

step "salt-ota run: nothing to do after update (rc 0); state + log written"
ota run --no-reboot >/dev/null 2>&1 || fail "salt-ota run should exit 0 when current"
grep -q 'everything is up to date' "$SALTOS_OTA_LOG" || fail "salt-ota did not log to $SALTOS_OTA_LOG"
[ -f "$SALTOS_OTA_STATE" ] || fail "salt-ota state file missing"

step "lock: a second run is refused while the lock is held (rc 1)"
set +e
(
	exec 9>"$SALTOS_OTA_LOCK"
	flock 9
	ota run --no-reboot >/dev/null 2>&1
	echo $? > "$WORK/locked.rc"
)
set -e
[ "$(cat "$WORK/locked.rc")" = 1 ] || fail "overlapping salt-ota run should exit 1 (got $(cat "$WORK/locked.rc"))"

step "bad hash: salt-ota run exits 4, hello stays 1.1, failed txn recorded"
publish "$WORK/keys/repo.sec" "$(G hello 1.2)" "$(G salt 0.1.1)"
python3 - "$REPO/$ARCH/packages/$(G hello 1.2)" <<'EOF'
import sys
p = sys.argv[1]
b = bytearray(open(p, "rb").read())
b[40] ^= 0xff
open(p, "wb").write(b)
EOF
set +e
ota run --no-reboot >"$WORK/badhash.log" 2>&1; rc=$?
set -e
[ "$rc" -eq 4 ] || { cat "$WORK/badhash.log"; fail "bad hash should exit 4 (got $rc)"; }
grep -q 'HASH MISMATCH' "$WORK/badhash.log" || fail "bad hash was not reported"
[ "$(sh "$ROOT/usr/bin/hello")" = "hi 1.1" ] || fail "bad hash changed the system"
S deployments | grep -q 'failed' || fail "failed transaction not recorded"

step "bad signature: salt-ota run exits 1, system unchanged"
publish "$WORK/keys/other.sec" "$(G hello 1.2)" "$(G salt 0.1.1)"
set +e
ota run --no-reboot >"$WORK/badsig.log" 2>&1; rc=$?
set -e
[ "$rc" -eq 1 ] || { cat "$WORK/badsig.log"; fail "bad signature should exit 1 (got $rc)"; }
grep -qi 'SIGNATURE' "$WORK/badsig.log" || fail "bad signature was not reported"
[ "$(sh "$ROOT/usr/bin/hello")" = "hi 1.1" ] || fail "bad signature changed the system"

step "deployment: hello 1.2 via salt-ota run (rc 2); pin, rollback [N], prune honour pins"
publish "$WORK/keys/repo.sec" "$(G hello 1.2)" "$(G salt 0.1.1)"
set +e
ota run --no-reboot >"$WORK/run12.log" 2>&1; rc=$?
set -e
[ "$rc" -eq 2 ] || { cat "$WORK/run12.log"; fail "salt-ota run should exit 2 after applying (got $rc)"; }
[ "$(sh "$ROOT/usr/bin/hello")" = "hi 1.2" ] || fail "hello 1.2 not active"
S deployments | grep -q '^   1 .*  -$' || fail "deployment 1 snapshot should have been pruned (keep = 2)"
S rollback >/dev/null || fail "rollback (latest) failed"
[ "$(sh "$ROOT/usr/bin/hello")" = "hi 1.1" ] || fail "rollback did not restore hello 1.1"
S deployments | grep -q 'rollback' || fail "rollback not recorded as a deployment"
S rollback 2 >/dev/null || fail "rollback 2 failed"
[ "$(sh "$ROOT/usr/bin/hello")" = "hi 1.0" ] || fail "rollback 2 did not restore hello 1.0"
cmp -s "$SALT_BIN" "$ROOT/usr/bin/salt" || fail "rollback 2 did not restore salt 0.1.0"
S rollback 1 >/dev/null 2>&1 && fail "rollback 1 should fail once its snapshot is pruned"
S pin --unpin 2 >/dev/null || fail "unpin 2 failed"
S deployments | grep -q '^.P 2 ' && fail "deployment 2 still pinned after --unpin"
S deployments >/dev/null
echo "ota_smoke: all steps passed"
