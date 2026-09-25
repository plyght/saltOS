#!/bin/sh
# Declared install hooks: build, lint, run confined, fail closed, remove.
# Needs root (hooks chroot into the target and unshare network/mount
# namespaces); exits 77 (skipped) otherwise.
set -eu

SALT_BIN=$1
WORK=$2
[ "$(id -u)" = 0 ] || { echo "hooks_smoke: skipped (needs root)"; exit 77; }
ARCH=$("$SALT_BIN" --version | sed -E 's/.*\((.*)\)/\1/')

rm -rf "$WORK"
mkdir -p "$WORK/src" "$WORK/recipes" "$WORK/repo/$ARCH/packages" "$WORK/root/etc/salt"
touch "$WORK/src/.keep"
ROOT="$WORK/root"
export SALT_OUT="$WORK/out" SALT_WORK="$WORK/work"

fail() { echo "hooks_smoke: FAIL: $1" >&2; exit 1; }
step() { echo "hooks_smoke: $1"; }
S() { "$SALT_BIN" --root "$ROOT" "$@"; }

# A target root with a working /bin/sh: the host's shell and its libraries.
sh_bin=$(readlink -f /bin/sh)
mkdir -p "$ROOT/bin" "$ROOT/var" "$ROOT/tmp"
cp "$sh_bin" "$ROOT/bin/sh"
for lib in $(ldd "$sh_bin" | grep -oE '/[^ ]+'); do
	mkdir -p "$ROOT$(dirname "$lib")"
	cp -L "$lib" "$ROOT$lib"
done

recipe() {
	name=$1; ver=$2; hooks=$3
	mkdir -p "$WORK/recipes/$name-$ver"
	cat > "$WORK/recipes/$name-$ver/recipe.toml" <<EOF
name = "$name"
version = "$ver"
release = 1
arch = ["x86_64", "aarch64"]
summary = "hooks smoke package"
license = "MIT"

[source]
url = "file://$WORK/src"

[build]
system = "custom"
script = """
mkdir -p "\$SALT_DEST/usr/share/$name"
echo $ver > "\$SALT_DEST/usr/share/$name/version"
"""

[package]
deps = []

$hooks

[reproducibility]
status = "verified"
EOF
}

step "build: hooks are packaged; unknown hooks and scripts/ are rejected"
recipe greet 1.0 '[hooks]
post_install = """
echo "$SALT_HOOK $SALT_PKG $SALT_VERSION home=$HOME leak=${LEAK:-none}" >> /var/hooks.log
test -f /usr/share/greet/version
"""
pre_remove = """
echo "$SALT_HOOK $SALT_PKG $SALT_VERSION" >> /var/hooks.log
"""
post_remove = """
test ! -e /usr/share/greet/version
echo "$SALT_HOOK $SALT_PKG" >> /var/hooks.log
"""'
recipe greet 2.0 '[hooks]
post_upgrade = """
echo "$SALT_HOOK $SALT_PKG $SALT_OLD_VERSION -> $SALT_VERSION" >> /var/hooks.log
"""'
recipe broken 1.0 '[hooks]
post_install = """
echo about to fail >> /var/hooks.log
exit 3
"""'
recipe bogus 1.0 '[hooks]
post_frobnicate = "true"'
recipe loose 1.0 ''
mkdir -p "$WORK/recipes/loose-1.0/scripts" && echo true > "$WORK/recipes/loose-1.0/scripts/postinst"
for r in greet-1.0 greet-2.0 broken-1.0; do
	"$SALT_BIN" build "$WORK/recipes/$r" >/dev/null || fail "build $r"
done
if "$SALT_BIN" build "$WORK/recipes/bogus-1.0" >/dev/null 2>&1; then fail "built an undeclared hook"; fi
if "$SALT_BIN" build "$WORK/recipes/loose-1.0" >/dev/null 2>&1; then fail "built a scripts/ directory"; fi
"$SALT_BIN" lint "$WORK/recipes/greet-1.0" 2>&1 | grep -q 'install-hook' || fail "lint did not surface the hook"
if "$SALT_BIN" lint "$WORK/recipes/bogus-1.0" >/dev/null 2>&1; then fail "lint passed an undeclared hook"; fi
if "$SALT_BIN" lint "$WORK/recipes/loose-1.0" >/dev/null 2>&1; then fail "lint passed scripts/"; fi

publish() {
	rm -f "$WORK/repo/$ARCH/packages/"*
	for g in "$@"; do cp "$SALT_OUT/$ARCH/packages/$g-1-$ARCH.grain" "$WORK/repo/$ARCH/packages/"; done
	"$SALT_BIN" repo publish "$WORK/repo/$ARCH" >/dev/null
	S sync >/dev/null 2>&1
}
printf 'repo = "current"\nsource = "file://%s/repo"\n' "$WORK" > "$ROOT/etc/salt/repo.conf"

step "post_install runs chrooted with a clean environment"
publish greet-1.0 broken-1.0
LEAK=host S --yes install greet >/dev/null
grep -qx 'post_install greet 1.0 home=/root leak=none' "$ROOT/var/hooks.log" ||
	fail "post_install log: $(cat "$ROOT/var/hooks.log")"

step "a failing post_install rolls the install back"
if S --yes install broken >/dev/null 2>&1; then fail "install with a failing hook succeeded"; fi
grep -qx 'about to fail' "$ROOT/var/hooks.log" || fail "broken hook never ran"
[ ! -e "$ROOT/usr/share/broken/version" ] || fail "files of the failed install were left behind"
if S list | grep -q "^broken "; then fail "failed package recorded as installed"; fi

step "post_upgrade sees the old version"
publish greet-2.0
S --yes update >/dev/null
grep -qx 'post_upgrade greet 1.0 -> 2.0' "$ROOT/var/hooks.log" || fail "post_upgrade log: $(cat "$ROOT/var/hooks.log")"

step "remove runs the installed version's pre_remove/post_remove (none declared in 2.0)"
: > "$ROOT/var/hooks.log"
publish greet-1.0
S --yes remove greet >/dev/null
[ ! -s "$ROOT/var/hooks.log" ] || fail "2.0 declares no remove hooks, yet: $(cat "$ROOT/var/hooks.log")"
S --yes install greet >/dev/null
: > "$ROOT/var/hooks.log"
S --yes remove greet >/dev/null
printf 'pre_remove greet 1.0\npost_remove greet\n' | cmp -s - "$ROOT/var/hooks.log" ||
	fail "remove hooks log: $(cat "$ROOT/var/hooks.log")"

step "SALT_SKIP_HOOKS=1 skips hooks"
: > "$ROOT/var/hooks.log"
SALT_SKIP_HOOKS=1 S --yes install greet >/dev/null 2>&1
[ ! -s "$ROOT/var/hooks.log" ] || fail "hook ran despite SALT_SKIP_HOOKS=1"

echo "hooks_smoke: ok"
