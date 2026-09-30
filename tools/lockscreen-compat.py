#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CC834
# SPDX-License-Identifier: GPL-3.0-only
"""Fix the older Plasma lock-screen fade policy using a managed user override."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile

PACKAGE = 'org.kde.plasma.desktop'
QML = 'contents/lockscreen/LockScreenUi.qml'
MARKER = '.floating-keyboard-compat.json'


def patch_qml(text):
    replacements = [
        ('property bool blockUI: containsMouse && (mainStack.depth > 1 || mainBlock.mainPasswordBox.text.length > 0 || inputPanel.keyboardActive)',
         'property bool blockUI: inputPanel.keyboardActive || (containsMouse && (mainStack.depth > 1 || mainBlock.mainPasswordBox.text.length > 0))'),
        ('uiVisible = seenPositionChange;', 'uiVisible = inputPanel.keyboardActive || seenPositionChange;'),
        ('onExited: {\n            uiVisible = false;\n        }',
         'onExited: {\n            if (!inputPanel.keyboardActive) {\n                uiVisible = false;\n            }\n        }'),
    ]
    for old, new in replacements:
        if text.count(old) != 1:
            raise ValueError('This Plasma lock-screen version is not supported by this compatibility patch; no files changed.')
        text = text.replace(old, new, 1)
    return text


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_managed(target):
    if target.is_symlink() or not (target / MARKER).is_file():
        raise ValueError(f'Existing custom shell package at {target}; refusing to overwrite it.')
    manifest = json.loads((target / MARKER).read_text())
    paths = {str(p.relative_to(target)) for p in target.rglob('*') if p.is_symlink() or p.is_file()}
    if paths != set(manifest['files']) | {MARKER}:
        raise ValueError('The compatibility override has additional or missing files; leaving it untouched.')
    for name, saved in manifest['files'].items():
        path = target / name
        current = {'link': os.readlink(path)} if path.is_symlink() else {'sha256': digest(path)}
        if current != saved:
            raise ValueError(f'{path} was modified; leaving the override untouched.')


def install(shell, data_home):
    shell = shell.resolve()
    # Validate before making any changes, including when refreshing after upgrades.
    patched = patch_qml((shell / QML).read_text())
    target = data_home / 'plasma/shells' / PACKAGE
    if target.exists() or target.is_symlink():
        verify_managed(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.keyboard-compat-', dir=target.parent) as staging:
        staged = Path(staging) / PACKAGE
        staged.mkdir()
        # KPackage rejects symlinks outside a package. Copy the small lock-screen
        # component set and use its supported fallback for all other shell files.
        metadata = json.loads((shell / 'metadata.json').read_text())
        metadata['KPlugin']['Id'] = 'org.kde.plasma.desktop.floating-keyboard-compat'
        metadata['X-Plasma-FallbackPackage'] = str(shell)
        (staged / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
        shutil.copytree(shell / 'contents/lockscreen', staged / 'contents/lockscreen')
        (staged / QML).write_text(patched)
        files = {}
        for path in staged.rglob('*'):
            if path.is_symlink():
                files[str(path.relative_to(staged))] = {'link': os.readlink(path)}
            elif path.is_file():
                files[str(path.relative_to(staged))] = {'sha256': digest(path)}
        (staged / MARKER).write_text(json.dumps({'source': str(shell), 'files': files}, indent=2) + '\n')
        previous = Path(staging) / 'previous'
        if target.exists():
            target.rename(previous)
        try:
            staged.rename(target)
        except BaseException:
            if previous.exists():
                previous.rename(target)
            raise
    return target


def remove(data_home):
    target = data_home / 'plasma/shells' / PACKAGE
    verify_managed(target)
    # Only this verified, tool-owned package is removed; the system package is untouched.
    shutil.rmtree(target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['install', 'remove'])
    parser.add_argument('--shell-dir', type=Path, default=Path('/usr/share/plasma/shells') / PACKAGE)
    parser.add_argument('--data-home', type=Path, default=Path(os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share'))))
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error('Run as your desktop user, without sudo.')
    try:
        if args.action == 'install':
            target = install(args.shell_dir, args.data_home)
            print(f'Installed lock-screen compatibility override: {target}')
            print('Takes effect the next time the screen locks. Re-run after Plasma upgrades, or remove when no longer needed.')
        else:
            remove(args.data_home)
            print('Removed compatibility override; the system shell package will be used on the next lock.')
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
