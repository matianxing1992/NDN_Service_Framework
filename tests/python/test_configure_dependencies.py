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

    def test_script_dry_run_is_non_mutating(self):
        result = subprocess.run(['bash', str(ROOT / 'configure.sh'), '--dry-run',
                                 '--', '--with-tests'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--with-tests', result.stdout)
        self.assertIn('SDKs must already be installed', result.stdout)

    def test_unknown_option_rejected(self):
        result = subprocess.run(['bash', str(ROOT / 'configure.sh'), '--bogus'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)


if __name__ == '__main__':
    unittest.main()
