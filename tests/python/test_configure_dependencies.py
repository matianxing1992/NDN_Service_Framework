"""Isolated configure inventory tests: no apt, network, or native build."""
import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'configure_dependencies', ROOT / 'scripts/configure_dependencies.py')
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


class ConfigureDependenciesTest(unittest.TestCase):
    def test_core_and_repo_inventory_do_not_require_model_or_gui_sdks(self):
        for profile in ('core', 'repo'):
            with self.subTest(profile=profile), \
                 patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
                 patch.object(inventory.subprocess, 'run', return_value=Mock(returncode=0)) as run, \
                 patch.object(inventory.Path, 'is_file', autospec=True,
                              side_effect=lambda p: 'nac-abe' in str(p)):
                self.assertEqual(inventory.check_dependencies(components=profile), ([], []))
            queried = ' '.join(str(call.args) for call in run.call_args_list)
            self.assertNotIn('onnx', queried)
            self.assertNotIn('gtk', queried)
            self.assertNotIn('gstreamer', queried)

    def test_di_requires_model_sdk_but_not_gui(self):
        with patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
             patch.object(inventory.subprocess, 'run', return_value=Mock(returncode=1)) as run, \
             patch.object(inventory.Path, 'is_file', return_value=False):
            errors, warnings = inventory.check_dependencies(components='di')
        self.assertTrue(any('onnxruntime' in e for e in errors))
        self.assertTrue(any('tokenizer_bridge' in e for e in errors))
        self.assertFalse(any('gtk' in str(call.args) for call in run.call_args_list))
        self.assertEqual(warnings, [])

    def test_core_sdk_only_requires_no_model_tools(self):
        with patch.object(inventory.shutil, 'which', return_value=None), \
             patch.object(inventory.subprocess, 'run') as run:
            self.assertEqual(inventory.check_dependencies(components='core', sdk_only=True), ([], []))
        run.assert_not_called()

    def test_unknown_component_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'components must be'):
            inventory.check_dependencies(components='unknown')

    def test_aggregates_missing_metadata_and_files(self):
        with patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
             patch.object(inventory.subprocess, 'run', return_value=Mock(returncode=1)), \
             patch.object(inventory.Path, 'is_file', return_value=False):
            errors, warnings = inventory.check_dependencies()
        self.assertTrue(any('ndn-cxx' in e for e in errors))
        self.assertTrue(any('onnxruntime' in e for e in errors))
        self.assertTrue(any('tokenizer' in e for e in errors))
        self.assertTrue(warnings)

    def test_missing_tools_do_not_attempt_pkg_config(self):
        with patch.object(inventory.shutil, 'which', return_value=None), \
             patch.object(inventory.subprocess, 'run') as run, \
             patch.object(inventory.Path, 'is_file', return_value=False):
            errors, _ = inventory.check_dependencies()
        run.assert_not_called()
        self.assertTrue(any('protoc' in e for e in errors))

    def test_installed_inventory(self):
        with patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
             patch.object(inventory.subprocess, 'run', return_value=Mock(returncode=0)), \
             patch.object(inventory.Path, 'is_file', return_value=True):
            self.assertEqual(inventory.check_dependencies(), ([], []))

    def test_sdk_only_reports_sdk_not_ndn_or_gui(self):
        with patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
             patch.object(inventory.subprocess, 'run', return_value=Mock(returncode=1)), \
             patch.object(inventory.Path, 'is_file', return_value=False):
            errors, warnings = inventory.check_dependencies(sdk_only=True)
        text = '\n'.join(errors)
        self.assertIn('onnxruntime', text)
        self.assertIn('tokenizer_bridge', text)
        self.assertNotIn('nac-abe', text)
        self.assertNotIn('gtkmm', text)
        self.assertEqual(warnings, [])

    def test_script_dry_run_is_non_mutating(self):
        result = subprocess.run(['bash', str(ROOT / 'configure.sh'), '--dry-run',
                                 '--', '--with-tests'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--with-tests', result.stdout)
        self.assertIn('Configure-only mode requires ONNX Runtime', result.stdout)
        self.assertIn('no apt, git fetch, builds, installation or Waf execution', result.stdout)

    def test_unknown_option_rejected(self):
        result = subprocess.run(['bash', str(ROOT / 'configure.sh'), '--bogus'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)


if __name__ == '__main__':
    unittest.main()
