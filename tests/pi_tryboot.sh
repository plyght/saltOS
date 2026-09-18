#!/bin/sh
set -eu

AB=$1
WORK=$2
rm -rf "$WORK"
mkdir -p "$WORK/fw" "$WORK/root/boot" "$WORK/stage/boot"
FW="$WORK/fw"; ROOT="$WORK/root"; STAGE="$WORK/stage"
export SALTOS_FW_DIR="$FW" SALTOS_LIVE_ROOT="$ROOT" SALTOS_ACTIVE_SUBVOL=@
RUN=$(uname -r)
OLD="0.1.0-factory"

fail() { echo "pi_tryboot: FAIL: $1" >&2; exit 1; }
step() { echo "pi_tryboot: $1"; }
ab() { sh "$AB" "$@"; }
cfg() { grep "^$2=" "$FW/$1" | cut -d= -f2-; }
mkkernel() { echo "kernel $2" > "$1/boot/vmlinuz-$2"; echo "initrd $2" > "$1/boot/initrd.img-$2"; }

mkkernel "$ROOT" "$OLD"
cp "$ROOT/boot/vmlinuz-$OLD" "$FW/vmlinuz_a"
cp "$ROOT/boot/initrd.img-$OLD" "$FW/initramfs_a"
echo "$OLD" > "$FW/kernel_a.release"
printf 'root=LABEL=saltos-root rootflags=subvol=@ rootwait\n' > "$FW/cmdline_a.txt"
cp "$FW/cmdline_a.txt" "$FW/cmdline.txt"
printf '[all]\nkernel=vmlinuz_a\ninitramfs initramfs_a followkernel\ncmdline=cmdline_a.txt\narm_64bit=1\n' > "$FW/config.txt"

step "factory: no pending trial, default kernel $OLD"
ab kernel-status > "$WORK/st.txt" || fail "kernel-status should exit 0 with nothing pending"
grep -q "^default:  $OLD" "$WORK/st.txt" || fail "default kernel not reported"
grep -q '^pending:  none' "$WORK/st.txt" || fail "pending should be none"

step "kernel-update with an unchanged kernel is a no-op"
ab kernel-update >/dev/null
[ ! -f "$FW/tryboot.txt" ] || fail "no-op kernel-update armed a tryboot"

step "kernel-update after installing 99.0.0-bad arms a one-shot tryboot; config.txt untouched"
mkkernel "$ROOT" "99.0.0-bad"
ab kernel-update | grep -q 'tried once on next boot' || fail "kernel-update did not arm a trial"
[ "$(cfg tryboot.txt kernel)" = "vmlinuz_a.next" ] || fail "tryboot.txt does not boot the .next kernel"
grep -q '^initramfs initramfs_a.next followkernel' "$FW/tryboot.txt" || fail "tryboot.txt lacks the .next initramfs"
[ "$(cfg tryboot.txt cmdline)" = "cmdline_a.txt" ] || fail "tryboot.txt cmdline wrong"
grep -q '^arm_64bit=1' "$FW/tryboot.txt" || fail "tryboot.txt lost the firmware settings"
[ "$(cfg config.txt kernel)" = "vmlinuz_a" ] || fail "config.txt changed before confirm"
cmp -s "$FW/vmlinuz_a.next" "$ROOT/boot/vmlinuz-99.0.0-bad" || fail "staged kernel payload differs"
[ "$(cat "$FW/kernel_a.release")" = "$OLD" ] || fail "committed kernel release changed before confirm"
set +e
ab kernel-status > "$WORK/st.txt"; rc=$?
set -e
[ "$rc" -eq 3 ] || fail "kernel-status should exit 3 while a trial is pending (got $rc)"
grep -q '^pending:  99.0.0-bad in @ (armed for next boot)' "$WORK/st.txt" || fail "pending trial not reported as armed"

step "confirm after a fallback boot (running $RUN != 99.0.0-bad) fails and clears the trial"
set +e
ab kernel-confirm > "$WORK/confirm.txt" 2>&1; rc=$?
set -e
[ "$rc" -eq 1 ] || fail "confirm should fail after fallback (got $rc)"
grep -q 'fell back' "$WORK/confirm.txt" || fail "fallback not reported"
[ ! -f "$FW/tryboot.txt" ] || fail "tryboot.txt left behind after fallback"
[ ! -f "$FW/vmlinuz_a.next" ] || fail ".next kernel left behind after fallback"
[ "$(cfg config.txt kernel)" = "vmlinuz_a" ] && [ "$(cat "$FW/kernel_a.release")" = "$OLD" ] || fail "fallback changed the committed kernel"
ab kernel-status >/dev/null || fail "kernel-status should exit 0 after the trial was cleared"

step "kernel-try re-arms $RUN; confirm on the tried kernel commits it"
rm -f "$ROOT/boot/vmlinuz-99.0.0-bad" "$ROOT/boot/initrd.img-99.0.0-bad"
mkkernel "$ROOT" "$RUN"
ab kernel-try | grep -q "kernel $RUN will be tried once" || fail "kernel-try did not arm $RUN"
ab kernel-status | grep -q "^pending:  $RUN (booted, awaiting confirm)" || fail "booted trial not reported"
ab kernel-confirm 2>&1 | grep -q "committed @ (kernel $RUN)" || fail "confirm did not commit"
[ ! -f "$FW/tryboot.txt" ] || fail "tryboot.txt left behind after confirm"
[ "$(cat "$FW/kernel_a.release")" = "$RUN" ] || fail "committed release not updated"
cmp -s "$FW/vmlinuz_a" "$ROOT/boot/vmlinuz-$RUN" || fail "slot a kernel not replaced"
cmp -s "$FW/initramfs_a" "$ROOT/boot/initrd.img-$RUN" || fail "slot a initramfs not replaced"
[ "$(cfg config.txt kernel)" = "vmlinuz_a" ] || fail "config.txt kernel wrong after confirm"
ab kernel-confirm | grep -q "nothing to confirm (default kernel $RUN)" || fail "second confirm should be a no-op"

step "A/B: finalize stages @b with its kernel and arms tryboot; commit after fallback is refused"
mkkernel "$STAGE" "$RUN"
SALTOS_STAGE_ROOT="$STAGE" ab kernel-update | grep -q "staged root provides kernel $RUN" || fail "staged kernel-update failed"
ab finalize "$STAGE" 2>&1 | grep -q 'prepared tryboot into @b' || fail "finalize failed"
grep -q 'subvol=@b' "$FW/cmdline_b.txt" || fail "cmdline_b.txt does not select @b"
[ "$(cfg tryboot.txt kernel)" = "vmlinuz_b" ] && [ "$(cfg tryboot.txt cmdline)" = "cmdline_b.txt" ] || fail "tryboot.txt does not boot slot b"
cmp -s "$FW/vmlinuz_b" "$STAGE/boot/vmlinuz-$RUN" || fail "slot b kernel not copied"
ab status | grep -q 'tryboot: pending (@b, kernel' || fail "status does not show the pending A/B tryboot"
set +e
ab commit >/dev/null 2>&1; rc=$?
set -e
[ "$rc" -eq 1 ] || fail "commit while still on @ should fail (got $rc)"
[ ! -f "$FW/tryboot.txt" ] || fail "tryboot.txt left behind after A/B fallback"
[ "$(cfg config.txt kernel)" = "vmlinuz_a" ] || fail "A/B fallback changed config.txt"

step "A/B: commit from the tried root @b promotes slot b"
ab finalize "$STAGE" 2>/dev/null
SALTOS_ACTIVE_SUBVOL=@b ab commit 2>&1 | grep -q 'committed @b' || fail "commit from @b failed"
[ "$(cfg config.txt kernel)" = "vmlinuz_b" ] && [ "$(cfg config.txt cmdline)" = "cmdline_b.txt" ] || fail "config.txt does not boot slot b"
grep -q '^arm_64bit=1' "$FW/config.txt" || fail "config.txt lost the firmware settings"
[ ! -f "$FW/tryboot.txt" ] || fail "tryboot.txt left behind after commit"

step "rollback: the restored root's kernel becomes the committed default without a trial"
rm -f "$ROOT/boot/vmlinuz-$RUN" "$ROOT/boot/initrd.img-$RUN"
SALTOS_ACTIVE_SUBVOL=@b SALTOS_STAGE_ROOT="$ROOT" ab kernel-update rollback | grep -q "default kernel $OLD (rollback)" || fail "rollback kernel-update failed"
[ "$(cat "$FW/kernel_b.release")" = "$OLD" ] || fail "rollback did not commit the old kernel"
cmp -s "$FW/vmlinuz_b" "$ROOT/boot/vmlinuz-$OLD" || fail "rollback did not copy the old kernel"
[ ! -f "$FW/tryboot.txt" ] || fail "rollback left a tryboot armed"
SALTOS_ACTIVE_SUBVOL=@b ab kernel-status | grep -q "^default:  $OLD" || fail "default after rollback wrong"
echo "pi_tryboot: all steps passed"
