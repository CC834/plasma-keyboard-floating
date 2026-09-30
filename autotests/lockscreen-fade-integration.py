#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CC834
# SPDX-License-Identifier: GPL-3.0-only
"""Optional regression test using the real greeter in a private headless KWin.
Run inside dbus-run-session. No physical screen is locked by this test.
"""
from pathlib import Path
import os, subprocess, time, tempfile, signal, importlib.util, sys
import argparse
import shutil
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('mode', choices=['before', 'after'])
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--log-dir', type=Path, default=Path('build/lockscreen-fade-test'))
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
mode = args.mode
binary = args.binary.resolve()
if not binary.is_file(): parser.error('Build the keyboard binary first')
qdbus = shutil.which('qdbus6') or shutil.which('qdbus') or '/usr/lib64/qt6/bin/qdbus'
probe = subprocess.run([qdbus, 'org.kde.KWin', '/KWin', 'org.freedesktop.DBus.Peer.Ping'], capture_output=True)
if probe.returncode == 0: parser.error('Run inside dbus-run-session, isolated from your desktop KWin')
args.log_dir.mkdir(parents=True, exist_ok=True)
spec=importlib.util.spec_from_file_location('compat',root/'tools/lockscreen-compat.py')
compat=importlib.util.module_from_spec(spec);spec.loader.exec_module(compat)
with tempfile.TemporaryDirectory(prefix='keyboard-fade-test-') as temp:
    data=Path(temp)/'data'
    shell=Path('/usr/share/plasma/shells/org.kde.plasma.desktop')
    package=compat.install(shell,data)
    file=package/compat.QML
    if mode=='before':file.write_text((shell/compat.QML).read_text())
    probe='''
        Timer { interval: 1500; running: true; onTriggered: lockScreenRoot.uiVisible = true }
        Timer { interval: 3000; running: true; onTriggered: lockScreenRoot.exited() }
        Timer { interval: 1000; running: true; repeat: true;
            onTriggered: console.warn("FADEPROBE ui=" + lockScreenRoot.uiVisible + " keyboard=" + inputPanel.keyboardActive + " block=" + lockScreenRoot.blockUI)
        }
'''
    file.write_text(file.read_text().replace('id: lockScreenRoot','id: lockScreenRoot\n'+probe))
    env=os.environ.copy()
    env.update(XDG_CONFIG_HOME=temp,XDG_DATA_HOME=str(data),QT_QUICK_BACKEND='software',KWIN_COMPOSE='Q',QT_FORCE_STDERR_LOGGING='1')
    Path(temp,'kwinrc').write_text('[Wayland]\nVirtualKeyboardMode=2\n')
    logpath=args.log_dir / f'lock-fade-{mode}.log'
    with logpath.open('w') as log:
        kwin=subprocess.Popen(['kwin_wayland','--virtual','--width','1280','--height','800','--no-global-shortcuts','--no-kactivities','--socket','floating-fade-test','--inputmethod',str(binary),'--lockscreen'],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        def call(*args):
            r=subprocess.run([qdbus,*args],env=env,capture_output=True,text=True,timeout=10)
            return r.stdout.strip()
        try:
            for _ in range(20):
                time.sleep(.5)
                if call('org.freedesktop.ScreenSaver','/ScreenSaver','org.freedesktop.ScreenSaver.GetActive')=='true':break
            call('org.kde.KWin','/VirtualKeyboard','org.kde.kwin.VirtualKeyboard.forceActivate')
            time.sleep(14)
            assert call('org.freedesktop.ScreenSaver','/ScreenSaver','org.freedesktop.ScreenSaver.GetActive')=='true'
        finally:
            os.killpg(kwin.pid,signal.SIGTERM)
            try:kwin.wait(timeout=5)
            except subprocess.TimeoutExpired:os.killpg(kwin.pid,signal.SIGKILL)
    lines=[x for x in logpath.read_text().splitlines() if 'FADEPROBE' in x]
    print('\n'.join(lines))
    assert len(lines)>=10, 'Actual greeter probe did not run'
    expected='ui=false' if mode=='before' else 'ui=true'
    assert all(expected in x and 'keyboard=true' in x for x in lines[-5:]),lines[-5:]
    print('PASS',mode)
