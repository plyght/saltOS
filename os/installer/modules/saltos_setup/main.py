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


def toml_str(value):
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
        elif ord(c) < 0x20:
            out += "\\u%04x" % ord(c)
        else:
            out += c
    return '"' + out + '"'


def toml_bool(value):
    return "true" if value else "false"


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
    if not os.path.exists(os.path.join(strata_dir, stratum + ".toml")):
        raise ValueError("stratum definition {}/{}.toml is missing".format(strata_dir, stratum))

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

    lines = [
        "[system]",
        "hostname = " + toml_str(hostname),
        "locale = " + toml_str(lang),
        "timezone = " + toml_str(timezone),
        "keymap = " + toml_str(console_keymap(gs)),
        "xkb_layout = " + toml_str(gs.value("keyboardLayout") or "us"),
        "xkb_variant = " + toml_str(gs.value("keyboardVariant") or ""),
        "",
        "[install]",
        "mode = \"mounted\"",
        "target = " + toml_str(root_mount_point),
        "filesystem = " + toml_str(root.get("fs") or "btrfs"),
        "encrypt = " + toml_bool(encrypt),
    ]
    if encrypt:
        lines.append("passphrase = " + toml_str(passphrase))
    if swap is not None:
        lines.append("swap = \"partition\"")
        lines.append("swap_device = " + toml_str(swap.get("device")))
    else:
        lines.append("swap = \"none\"")
    lines += [
        "desktop = " + toml_str(desktop),
        "",
        "[boot]",
        "firmware = " + toml_str(firmware),
        "os_prober = true",
        "cmdline = " + toml_str(conf.get("extraCmdline", "") or ""),
        "shim = \"auto\"",
        "",
        "[user]",
        "name = " + toml_str(username),
        "password = " + toml_str(password),
    ]
    if root_password:
        lines.append("root_password = " + toml_str(root_password))
    lines += [
        "shell = " + toml_str(shell),
        "sudo = true",
        "autologin = " + toml_bool(autologin),
        "create = true",
        "",
        "[network]",
        "mode = " + toml_str(network),
    ]
    if ssid:
        lines.append("wifi_ssid = " + toml_str(ssid))
    lines += [
        "",
        "[kernel]",
        "source = " + toml_str(conf.get("kernelSource", "native")),
        "",
        "[[stratum]]",
        "name = " + toml_str(stratum),
        "role = \"primary\"",
        "expose = true",
        "",
    ]
    return "\n".join(lines)


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
    config_path = os.path.join(config_dir, "system.toml")
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
