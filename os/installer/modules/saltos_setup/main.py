#!/usr/bin/env python3

import os
import subprocess

import libcalamares
from libcalamares.utils import debug, warning

import gettext
_ = gettext.translation("calamares-python",
                        localedir=libcalamares.utils.gettext_path(),
                        languages=libcalamares.utils.gettext_languages(),
                        fallback=True).gettext


def pretty_name():
    return _("Installing saltOS")


CONSOLE_KEYMAPS = {
    "us": "us", "gb": "uk", "de": "de-latin1", "fr": "fr-latin1", "es": "es",
    "it": "it", "pt": "pt-latin1", "br": "br-abnt2", "ru": "ru", "pl": "pl",
    "ch": "sg-latin1", "se": "sv-latin1", "no": "no-latin1", "dk": "dk-latin1",
    "fi": "fi", "nl": "nl", "be": "be-latin1", "cz": "cz-lat2", "sk": "sk-qwertz",
    "hu": "hu", "tr": "trq", "jp": "jp106", "latam": "la-latin1", "gr": "gr",
    "ua": "ua-utf", "at": "de-latin1", "ca": "cf", "ie": "ie", "is": "is-latin1",
    "hr": "croat", "si": "slovene", "ro": "ro", "bg": "bg_bds-utf8", "ee": "et",
    "lt": "lt", "lv": "lv", "il": "il", "kr": "kr", "cn": "us", "in": "us",
}


def unobscure(text):
    if not text:
        return ""
    try:
        return libcalamares.utils.obscure(text)
    except AttributeError:
        return "".join(c if ord(c) <= 0x21 else chr(0x1001F - ord(c)) for c in text)


def lua_str(value):
    out = ""
    for c in str(value):
        if c == '"':
            out += '\\"'
        elif c == "\\":
            out += "\\\\"
        elif c == "\n":
            out += "\\n"
        elif c == "\t":
            out += "\\t"
        elif ord(c) < 0x20 or ord(c) == 0x7f:
            out += "\\%03d" % ord(c)
        else:
            out += c
    return '"' + out + '"'


def lua_bool(value):
    return "true" if value else "false"


def lua_config(sections, stratum):
    """Render [(table, [(key, lua_value), ...]), ...] plus the primary stratum
    as a Lua config chunk for salt-setup --from."""
    lines = ["-- saltOS system configuration (written by the Calamares saltos_setup module)",
             "return {"]
    for name, entries in sections:
        lines.append("  {} = {{".format(name))
        for key, value in entries:
            lines.append("    {} = {},".format(key, value))
        lines.append("  },")
    lines.append("  stratum = {")
    lines.append("    {{ name = {}, role = \"primary\", expose = true }},".format(lua_str(stratum)))
    lines.append("  },")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def kbd_model_map(layout, variant):
    for path in ("/usr/share/systemd/kbd-model-map",
                 "/usr/share/calamares/kbd-model-map"):
        if not os.path.exists(path):
            continue
        with open(path) as handle:
            best = None
            for line in handle:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                fields = line.split()
                if len(fields) < 3:
                    continue
                console, xlayout, xvariant = fields[0], fields[1], fields[3] if len(fields) > 3 else "-"
                if xlayout != layout:
                    continue
                if (xvariant == "-" and not variant) or xvariant == variant:
                    return console
                if best is None and xvariant == "-":
                    best = console
            if best:
                return best
    return None


def console_keymap(gs):
    keymap = gs.value("keyboardVConsoleKeymap")
    if keymap:
        return keymap
    layout = gs.value("keyboardLayout") or "us"
    variant = gs.value("keyboardVariant") or ""
    mapped = kbd_model_map(layout, variant)
    if mapped:
        return mapped
    if layout == "us" and variant == "dvorak":
        return "dvorak"
    return CONSOLE_KEYMAPS.get(layout, layout)


def selected_partitions(gs):
    root = None
    esp = None
    swap = None
    for part in gs.value("partitions") or []:
        mount = part.get("mountPoint") or ""
        fs = part.get("fs") or ""
        if mount == "/":
            root = part
        elif mount == "/boot/efi":
            esp = part
        elif fs in ("linuxswap", "swap") and swap is None:
            swap = part
    return root, esp, swap


def live_wifi():
    try:
        out = subprocess.run(["nmcli", "-t", "-f", "TYPE,STATE,CONNECTION", "device"],
                             capture_output=True, text=True, timeout=15).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    for line in out.splitlines():
        fields = line.split(":")
        if len(fields) >= 3 and fields[0] == "wifi" and fields[1].startswith("connected"):
            return fields[2]
    return None


def kernel_cmdline(conf):
    words = (conf.get("extraCmdline", "") or "").split()
    try:
        with open("/proc/cmdline", encoding="utf-8") as f:
            live = f.read().split()
    except OSError:
        live = []
    for w in live:
        if w.startswith("console=") and w not in words:
            words.append(w)
    return " ".join(words)


def build_config(conf, gs):
    root_mount_point = gs.value("rootMountPoint")
    root, esp, swap = selected_partitions(gs)
    if root is None:
        raise ValueError("no partition is assigned to /")
    if esp is None and gs.value("firmwareType") == "efi":
        raise ValueError("no EFI system partition is assigned to /boot/efi")

    locale_conf = gs.value("localeConf") or {}
    lang = locale_conf.get("LANG") or "en_US.UTF-8"
    region = gs.value("locationRegion") or "UTC"
    zone = gs.value("locationZone") or ""
    timezone = region + "/" + zone if zone else region

    stratum = gs.value("packagechooser_stratum") or conf.get("defaultStratum", "debian")
    stratum = stratum.split(",")[0].strip()
    strata_dir = conf.get("strataDir", "/etc/salt/strata")
    if not os.path.exists(os.path.join(strata_dir, stratum + ".lua")):
        raise ValueError("stratum definition {}/{}.lua is missing".format(strata_dir, stratum))

    desktop = gs.value("packagechooser_desktop") or conf.get("desktop", "keep")
    desktop = desktop.split(",")[0].strip() or "keep"

    username = gs.value("username")
    password = unobscure(gs.value("password"))
    if not username or not password:
        raise ValueError("the users page did not provide a login name and password")
    hostname = gs.value("hostname") or "saltos"
    autologin = bool(gs.value("autoLoginUser"))
    shell = gs.value("userShell") or "/bin/bash"

    root_password = ""
    if gs.value("setRootPassword") and gs.value("reuseRootPassword"):
        root_password = password

    encrypt = bool(root.get("luksMapperName"))
    passphrase = root.get("luksPassphrase") or ""
    if encrypt and not passphrase:
        raise ValueError("the root partition is encrypted but no passphrase was recorded")

    firmware = "uefi" if gs.value("firmwareType") == "efi" else "bios"

    network = "dhcp"
    ssid = live_wifi()
    if ssid:
        network = "wifi"

    system = [
        ("hostname", lua_str(hostname)),
        ("locale", lua_str(lang)),
        ("timezone", lua_str(timezone)),
        ("keymap", lua_str(console_keymap(gs))),
        ("xkb_layout", lua_str(gs.value("keyboardLayout") or "us")),
        ("xkb_variant", lua_str(gs.value("keyboardVariant") or "")),
    ]
    install = [
        ("mode", lua_str("mounted")),
        ("target", lua_str(root_mount_point)),
        ("filesystem", lua_str(root.get("fs") or "btrfs")),
        ("encrypt", lua_bool(encrypt)),
    ]
    if encrypt:
        install.append(("passphrase", lua_str(passphrase)))
    if swap is not None:
        install.append(("swap", lua_str("partition")))
        install.append(("swap_device", lua_str(swap.get("device"))))
    else:
        install.append(("swap", lua_str("none")))
    install.append(("desktop", lua_str(desktop)))
    boot = [
        ("firmware", lua_str(firmware)),
        ("os_prober", "true"),
        ("cmdline", lua_str(kernel_cmdline(conf))),
        ("shim", lua_str("auto")),
    ]
    user = [
        ("name", lua_str(username)),
        ("password", lua_str(password)),
    ]
    if root_password:
        user.append(("root_password", lua_str(root_password)))
    user += [
        ("shell", lua_str(shell)),
        ("sudo", "true"),
        ("autologin", lua_bool(autologin)),
        ("create", "true"),
    ]
    net = [("mode", lua_str(network))]
    if ssid:
        net.append(("wifi_ssid", lua_str(ssid)))
    kernel = [("source", lua_str(conf.get("kernelSource", "native")))]
    return lua_config([
        ("system", system),
        ("install", install),
        ("boot", boot),
        ("user", user),
        ("network", net),
        ("kernel", kernel),
    ], stratum)


def run():
    conf = libcalamares.job.configuration
    gs = libcalamares.globalstorage

    root_mount_point = gs.value("rootMountPoint")
    if not root_mount_point:
        return ("No mount point", "rootMountPoint is not set in global storage.")

    salt_setup = conf.get("saltSetup", "/usr/bin/salt-setup")
    if not os.access(salt_setup, os.X_OK):
        return ("salt-setup missing", "{} is not executable on this medium.".format(salt_setup))

    try:
        text = build_config(conf, gs)
    except ValueError as exc:
        return ("Incomplete installer configuration", str(exc))

    config_dir = conf.get("configDir", "/run/saltos-installer")
    os.makedirs(config_dir, mode=0o700, exist_ok=True)
    config_path = os.path.join(config_dir, "system.lua")
    log_path = os.path.join(config_dir, "salt-setup.log")
    fd = os.open(config_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as handle:
        handle.write(text)

    cmd = [salt_setup, "--from", config_path, "--target", root_mount_point, "--yes"]
    debug("running {}".format(" ".join(cmd)))
    console = None
    try:
        console = open("/dev/console", "w")
    except OSError:
        console = None

    tail = []
    steps = 0
    rc = 1
    try:
        with open(log_path, "w") as log:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, bufsize=1)
            for line in proc.stdout:
                log.write(line)
                log.flush()
                tail.append(line.rstrip("\n"))
                if len(tail) > 40:
                    tail.pop(0)
                debug("salt-setup: " + line.rstrip("\n"))
                if console is not None:
                    try:
                        console.write("salt-setup: " + line)
                        console.flush()
                    except OSError:
                        console = None
                if line.startswith("==> "):
                    steps += 1
                    libcalamares.job.setprogress(min(0.95, steps / 20.0))
            rc = proc.wait()
    finally:
        try:
            os.unlink(config_path)
        except OSError:
            pass
        if console is not None:
            console.close()

    if rc != 0:
        warning("salt-setup exited with {}".format(rc))
        return ("salt-setup failed",
                "salt-setup exited with status {}.\n\n{}".format(rc, "\n".join(tail)))
    libcalamares.job.setprogress(1.0)
    return None
