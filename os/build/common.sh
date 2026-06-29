# Shared helpers for saltOS image/ISO builders (os/build/*.sh).
# Source this after defining: ROOTFS, REPO, VERSION, SALT_BIN, (optional SALTSETUP_BIN).
#
# These functions centralize the bits every target needs so fixes land
# everywhere at once: the salt control plane, the strata config (with
# expose-by-default), the passwordless-sudo rule that makes exposed commands
# work from any user, and the user account.

# Output naming: saltos-<version>-<target>-<arch>.<ext>
saltos_artifact_name() { # <target> <arch> <ext>
  printf 'saltos-%s-%s-%s.%s' "$VERSION" "$1" "$2" "$3"
}

# Install the salt + salt-setup binaries and the stratum recipes.
saltos_install_controlplane() {
  install -Dm755 "$SALT_BIN" "$ROOTFS/usr/bin/salt"
  [ -n "${SALTSETUP_BIN:-}" ] && [ -f "$SALTSETUP_BIN" ] && \
    install -Dm755 "$SALTSETUP_BIN" "$ROOTFS/usr/bin/salt-setup"
  # var/lib/salt must exist or the strata/native sqlite DBs can't be created.
  mkdir -p "$ROOTFS/etc/salt/strata" "$ROOTFS/usr/local/salt/shims" \
    "$ROOTFS/strata" "$ROOTFS/var/lib/salt"
  cp "$REPO"/strata/*.toml "$ROOTFS/etc/salt/strata/" 2>/dev/null || true
  install -Dm644 "$REPO/os/profile.d/salt-shims.sh" \
    "$ROOTFS/etc/profile.d/salt-shims.sh"
}

# Write salt.conf. saltOS images opt into exposing stratum commands globally by
# default: expose_pm + expose_all + auto_expose=always means installing a tool
# in any stratum makes it a host command automatically.
saltos_write_config() {
  cat > "$ROOTFS/etc/salt/repo.conf" <<EOF
repo = "current"
source = "${OTA_SOURCE:-}"
key = "${OTA_KEY:-}"
EOF
  cat > "$ROOTFS/etc/salt/salt.conf" <<'EOF'
[install]
auto_expose = "always"

[strata]
expose_pm = true
expose_all = true
auto_service = true
EOF
}

# Passwordless sudo for the salt binary. This is what lets salt self-escalate
# (sudo -n salt run ...) so exposed commands work from ANY user without typing
# sudo -- salt sets up the stratum as root, then drops back to the calling user.
# Scoped to the salt binary; broad enough for the single-user appliance model.
saltos_write_sudoers() { # <salt-username>
  local user="${1:-salt}"
  mkdir -p "$ROOTFS/etc/sudoers.d"
  cat > "$ROOTFS/etc/sudoers.d/salt-strata" <<'EOF'
# Allow any user to invoke the salt control plane without a password prompt so
# exposed stratum commands (e.g. `nvim`, `apk`) self-escalate transparently.
ALL ALL=(root) NOPASSWD: /usr/bin/salt
EOF
  chmod 0440 "$ROOTFS/etc/sudoers.d/salt-strata"
  # Primary interactive user keeps full passwordless sudo.
  cat > "$ROOTFS/etc/sudoers.d/$user" <<EOF
$user ALL=(ALL) NOPASSWD: ALL
EOF
  chmod 0440 "$ROOTFS/etc/sudoers.d/$user"
}

# os-release block.
saltos_write_os_release() { # <pretty-suffix>
  cat > "$ROOTFS/etc/os-release" <<EOF
NAME="saltOS"
PRETTY_NAME="saltOS $VERSION ($1)"
ID=saltos
ID_LIKE=void
VERSION="$VERSION"
VERSION_ID="$VERSION"
HOME_URL="https://github.com/plyght/saltOS"
EOF
}
