#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CC834
# SPDX-License-Identifier: GPL-3.0-only
"""Install a built floating keyboard without modifying the system package."""

import argparse
import datetime
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def desktop_quote(path):
    # Desktop Entry Exec quoting is different from shell quoting.
    value = str(path)
    if any(c in value for c in "\n\r\0"):
        raise ValueError("Installation paths cannot contain line breaks or NUL")
    for char in ("\\", '"', "`", "$"):
        value = value.replace(char, "\\" + char)
    return '"' + value.replace("%", "%%") + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path, nargs="?", default=Path("build"))
    parser.add_argument("--with-audio-rule", action="store_true",
                        help="Install the optional WirePlumber 0.5+ keyboard audio rule")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Run as your regular desktop user, without sudo")
    binary = args.build_dir.resolve() / "bin/plasma-keyboard"
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"Build the executable first: {binary}")
    candidates = [shutil.which("qdbus6"), shutil.which("qdbus"),
                  "/usr/lib64/qt6/bin/qdbus", "/usr/lib/qt6/bin/qdbus"]
    qdbus = next((Path(p) for p in candidates if p and os.access(p, os.X_OK)), None)
    if qdbus is None:
        parser.error("Install Qt's qdbus utility before installing the launcher")

    home = Path.home()
    root = Path(__file__).resolve().parents[1]
    libexec = home / ".local/libexec"
    applications = home / ".local/share/applications"
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    backup = home / ".local/state/plasma-keyboard-floating/backups" / stamp
    executable = libexec / "plasma-keyboard-floating"
    show = libexec / "show-floating-keyboard"
    payloads = [
        (executable, binary.read_bytes(), 0o755),
        (show, ("#!/bin/sh\n"
                "# Let the launcher return focus to the target application.\n"
                "sleep 0.3\n"
                f"exec {shlex.quote(str(qdbus))} org.kde.plasma.keyboard.Floating "
                "/Keyboard org.kde.plasma.keyboard.Floating.showKeyboard\n").encode(), 0o755),
        (applications / "org.kde.plasma.keyboard-floating.desktop", (
            "[Desktop Entry]\nName=Floating Plasma Keyboard\n"
            "Comment=Movable touch keyboard for Plasma\n"
            f"Exec={desktop_quote(executable)}\n"
            "Type=Application\nX-KDE-Wayland-VirtualKeyboard=true\n"
            "Icon=input-keyboard-virtual\nNoDisplay=true\n").encode(), 0o644),
        (applications / "org.kde.plasma.keyboard-floating-show.desktop", (
            "[Desktop Entry]\nName=Show Floating Keyboard\n"
            "Comment=Open the floating keyboard in the current app\n"
            f"Exec={desktop_quote(show)}\nIcon=input-keyboard-virtual\n"
            "Type=Application\nStartupNotify=false\nTerminal=false\n"
            "Categories=Utility;Accessibility;\n").encode(), 0o644),
    ]
    if args.with_audio_rule:
        config = Path(os.environ.get("XDG_CONFIG_HOME", str(home / ".config")))
        payloads.append((config / "wireplumber/wireplumber.conf.d/80-floating-keyboard.conf",
                         (root / "config/80-floating-keyboard.conf").read_bytes(), 0o644))

    for index, (destination, data, mode) in enumerate(payloads):
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            backup.mkdir(parents=True, exist_ok=True)
            shutil.copy2(destination, backup / f"{index}-{destination.name}")
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(data)
            temporary.chmod(mode)
            os.replace(temporary, destination)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        print(f"Installed {destination}")

    update = shutil.which("update-desktop-database")
    if update:
        subprocess.run([update, str(applications)], check=True)
    if backup.exists():
        print(f"Previous files backed up to {backup}")
    print(f"Next: import {root / 'config/floating-keyboard.kwinrule'} in Plasma Window Rules.")
    print("Then select Floating Plasma Keyboard in the Virtual Keyboard settings.")
    if args.with_audio_rule:
        print("Log out and back in to load the separate keyboard audio rule.")


if __name__ == "__main__":
    main()
