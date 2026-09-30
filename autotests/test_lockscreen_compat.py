# SPDX-FileCopyrightText: 2026 CC834
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('compat', Path(__file__).resolve().parents[1] / 'tools/lockscreen-compat.py')
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)

# Minimal affected-version fixture; rendering is verified separately with the
# installed Plasma greeter in an isolated nested KWin session.
AFFECTED = '''property bool blockUI: containsMouse && (mainStack.depth > 1 || mainBlock.mainPasswordBox.text.length > 0 || inputPanel.keyboardActive)
uiVisible = seenPositionChange;
onExited: {
            uiVisible = false;
        }
'''


class CompatibilityInstallTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.shell = self.base / 'system'
        self.qml = self.shell / compat.QML
        self.qml.parent.mkdir(parents=True)
        self.qml.write_text(AFFECTED)
        (self.shell / 'metadata.json').write_text(json.dumps({'KPlugin': {'Id': compat.PACKAGE}, 'X-Plasma-APIVersion': '2'}))
        self.data = self.base / 'user'

    def test_install_refresh_remove_preserve_system(self):
        target = compat.install(self.shell, self.data)
        self.assertEqual(self.qml.read_text(), AFFECTED)
        self.assertNotEqual((target / compat.QML).read_text(), AFFECTED)
        metadata = json.loads((target / 'metadata.json').read_text())
        self.assertEqual(metadata['X-Plasma-FallbackPackage'], str(self.shell))
        self.assertNotEqual(metadata['KPlugin']['Id'], compat.PACKAGE)
        self.qml.write_text('// updated package\n' + AFFECTED)
        compat.install(self.shell, self.data)
        self.assertTrue((target / compat.QML).read_text().startswith('// updated package'))
        compat.remove(self.data)
        self.assertFalse(target.exists())
        self.assertTrue(self.qml.exists())

    def test_custom_package_is_not_overwritten(self):
        target = self.data / 'plasma/shells' / compat.PACKAGE
        target.mkdir(parents=True)
        (target / 'custom').write_text('keep')
        with self.assertRaises(ValueError):
            compat.install(self.shell, self.data)
        self.assertEqual((target / 'custom').read_text(), 'keep')

    def test_modified_override_is_not_removed_or_replaced(self):
        target = compat.install(self.shell, self.data)
        file = target / compat.QML
        file.write_text('custom change')
        with self.assertRaises(ValueError):
            compat.remove(self.data)
        with self.assertRaises(ValueError):
            compat.install(self.shell, self.data)
        self.assertEqual(file.read_text(), 'custom change')

    def test_unsupported_upgrade_leaves_previous_override_intact(self):
        target = compat.install(self.shell, self.data)
        before = (target / compat.QML).read_text()
        self.qml.write_text('new upstream implementation')
        with self.assertRaises(ValueError):
            compat.install(self.shell, self.data)
        self.assertEqual((target / compat.QML).read_text(), before)

    def test_extra_user_file_prevents_removal(self):
        target = compat.install(self.shell, self.data)
        (target / 'extra').write_text('keep')
        with self.assertRaises(ValueError):
            compat.remove(self.data)
        self.assertTrue((target / 'extra').exists())


if __name__ == '__main__':
    unittest.main()
