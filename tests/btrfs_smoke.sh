#!/bin/sh
# Exercise salt's Btrfs backend on a real btrfs filesystem: a loop-mounted
# image whose root is a subvolume (like a booted saltOS '@'). Needs root,
# btrfs-progs and loop devices. Usage: btrfs_smoke.sh <salt-bin> <workdir>
set -eu
SALT="$1"
WORK="$2"
[ "$(id -u)" -eq 0 ] || { echo "btrfs_smoke: must run as root" >&2; exit 1; }
command -v mkfs.btrfs >/dev/null 2>&1 || { echo "btrfs_smoke: mkfs.btrfs missing" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/mnt" "$WORK/src" "$WORK/recipes"
: > "$WORK/src/.keep"
ARCH="$("$SALT" --version | sed -n 's/.*(\([a-z0-9_]*\)).*/\1/p')"

recipe() { # name version
  mkdir -p "$WORK/recipes/$1-$2"
  cat > "$WORK/recipes/$1-$2/recipe.lua" <<EOF
return {
  name = "$1",
  version = "$2",
  release = 1,
  arch = { "x86_64", "aarch64" },
  summary = "btrfs smoke package $1",
  license = "MIT",
  source = { url = "file://$WORK/src" },
  build = {
    system = "custom",
    script = [[
mkdir -p "\$SALT_DEST/usr/bin"
printf '#!/bin/sh\\necho $1 $2\\n' > "\$SALT_DEST/usr/bin/$1"
chmod +x "\$SALT_DEST/usr/bin/$1"
]],
  },
  package = { deps = {}, conflicts = {} },
  reproducibility = { status = "verified" },
}
EOF
}

export SALT_OUT="$WORK/out" SALT_WORK="$WORK/work"
recipe hello 1.0
recipe hello 1.1
"$SALT" build "$WORK/recipes/hello-1.0"
"$SALT" keygen "$WORK/keys" repo
"$SALT" --key "$WORK/keys/repo.sec" repo publish "$WORK/out/$ARCH"

truncate -s 512M "$WORK/disk.img"
mkfs.btrfs -q -f -L saltos-smoke "$WORK/disk.img"
LOOP="$(losetup --find --show "$WORK/disk.img")"
cleanup() {
  umount "$WORK/mnt" 2>/dev/null || true
  losetup -d "$LOOP" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
mount -o subvolid=5 "$LOOP" "$WORK/mnt"
btrfs subvolume create "$WORK/mnt/@" >/dev/null
btrfs subvolume create "$WORK/mnt/@snapshots" >/dev/null
umount "$WORK/mnt"
mount -o subvol=@ "$LOOP" "$WORK/mnt"
ROOT="$WORK/mnt"
mkdir -p "$ROOT/.snapshots"
mount -o subvol=@snapshots "$LOOP" "$ROOT/.snapshots"
cleanup() {
  umount "$ROOT/.snapshots" 2>/dev/null || true
  umount "$ROOT" 2>/dev/null || true
  losetup -d "$LOOP" 2>/dev/null || true
}

mkdir -p "$ROOT/etc/salt"
cat > "$ROOT/etc/salt/repo.lua" <<EOF
return {
  repo = "current",
  source = "file://$WORK/out",
  key = "$(cat "$WORK/keys/repo.pub")",
}
EOF

fail() { echo "btrfs_smoke: FAIL: $*" >&2; exit 1; }

remount() {
  umount "$ROOT/.snapshots"
  umount "$ROOT"
  mount -o subvol=@ "$LOOP" "$ROOT"
  mount -o subvol=@snapshots "$LOOP" "$ROOT/.snapshots"
}

rollback() { # [N]: like a user would, then "reboot" into the swapped-in root
  set +e
  "$SALT" --root "$ROOT" --yes rollback "$@"
  rc=$?
  set -e
  [ "$rc" -eq 3 ] || fail "rollback exited $rc (expected 3: reboot required)"
  remount
}

"$SALT" --root "$ROOT" sync
"$SALT" --root "$ROOT" --yes install hello
[ -x "$ROOT/usr/bin/hello" ] || fail "install did not place the file"
"$ROOT/usr/bin/hello" | grep -q "hello 1.0" || fail "installed hello is not 1.0"
btrfs subvolume show "$ROOT/.snapshots/root-1" >/dev/null || fail "install took no btrfs snapshot"
"$SALT" --root "$ROOT" deployments | grep -q "root-1" || fail "deployment does not record its snapshot"

"$SALT" build "$WORK/recipes/hello-1.1"
rm -f "$WORK/out/$ARCH/packages/hello-1.0-1-$ARCH.grain"
"$SALT" --key "$WORK/keys/repo.sec" repo publish "$WORK/out/$ARCH"
"$SALT" --root "$ROOT" sync
"$SALT" --root "$ROOT" --yes update
"$ROOT/usr/bin/hello" | grep -q "hello 1.1" || fail "update did not install 1.1"
btrfs subvolume show "$ROOT/.snapshots/root-2" >/dev/null || fail "update took no btrfs snapshot"
"$ROOT/.snapshots/root-2/usr/bin/hello" | grep -q "hello 1.0" || fail "pre-update snapshot has the wrong content"

echo "tampered" >> "$ROOT/usr/bin/hello"
rollback
"$ROOT/usr/bin/hello" | grep -q "hello 1.0" || fail "rollback did not bring back the pre-update root"
"$SALT" --root "$ROOT" list --installed | grep -q "hello.*1\.0" || fail "rollback did not restore the database"
"$SALT" --root "$ROOT" verify hello || fail "restored files do not match the restored manifest"
"$SALT" --root "$ROOT" deployments | grep -q "^ *2 .*undone\|2.*undone" || fail "undone deployment is not recorded in the new root"
btrfs subvolume show "$ROOT/.snapshots/root-3" >/dev/null || fail "outgoing root was not kept as a snapshot"
grep -q "tampered" "$ROOT/.snapshots/root-3/usr/bin/hello" || fail "kept snapshot is not the outgoing root"

"$SALT" --root "$ROOT" --yes update
"$ROOT/usr/bin/hello" | grep -q "hello 1.1" || fail "update after rollback failed"
"$SALT" --root "$ROOT" --yes remove hello
[ ! -e "$ROOT/usr/bin/hello" ] || fail "remove left the file"
rollback
[ -x "$ROOT/usr/bin/hello" ] || fail "rollback of remove did not bring the file back"
"$ROOT/usr/bin/hello" | grep -q "hello 1.1" || fail "rollback of remove restored the wrong version"
"$SALT" --root "$ROOT" list --installed | grep -q "hello.*1\.1" || fail "rollback of remove did not restore the database"

"$SALT" --root "$ROOT" lock --output "$WORK/system.lock.toml"
grep -q 'name = "hello"' "$WORK/system.lock.toml" || fail "lock lacks hello"
"$SALT" --root "$ROOT" lock diff "$WORK/system.lock.toml"

before="$(btrfs subvolume list "$ROOT" | grep -c 'root-')"
"$SALT" --root "$ROOT" config gc --keep 2 --dry-run
after_dry="$(btrfs subvolume list "$ROOT" | grep -c 'root-')"
[ "$before" = "$after_dry" ] || fail "gc --dry-run deleted snapshots"
"$SALT" --root "$ROOT" --yes config gc --keep 2
after="$(btrfs subvolume list "$ROOT" | grep -c 'root-')"
[ "$after" -lt "$before" ] || fail "gc did not delete any btrfs snapshot ($before -> $after)"
[ ! -e "$ROOT/.snapshots/root-1" ] || fail "gc kept the oldest snapshot"
"$SALT" --root "$ROOT" verify hello || fail "files after gc do not match the manifest"
rollback
"$SALT" --root "$ROOT" verify || fail "root after rollback-after-gc does not match its database"

echo "btrfs_smoke: OK (snapshots before gc: $before, after: $after)"
