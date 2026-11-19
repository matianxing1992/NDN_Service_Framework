"""Select the maintained Waf tree independently of the root configure lock."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class BuildTreeSelection(unittest.TestCase):
    def fixture(self, root):
        root = Path(root)
        (root / 'scripts').mkdir()
        (root / 'build').mkdir()
        (root / 'build/.lock-waf_linux_build').touch()
        (root / '.lock-waf_linux_build').write_text('out_dir = "build-ASAN-UBSAN"\n')
        scripts = Path(__file__).resolve().parents[2] / 'scripts'
        for name in ('build-safe.sh', 'build-storage.py'):
            shutil.copyfile(scripts / name, root / 'scripts' / name)
        waf = root / 'waf'
        waf.write_text('#!/bin/sh\npwd\nprintf "%s\\n" "$WAFLOCK" "$@"\n')
        waf.chmod(0o755)
        return root

    def test_normal_build_selects_its_configured_tree(self):
        with tempfile.TemporaryDirectory(prefix='ndnsf-build-launcher-') as temp:
            root = self.fixture(temp)
            r = subprocess.run(['bash', str(root / 'scripts/build-safe.sh'), 'build',
                                '--targets=unit-tests', '-j4'], cwd=root, capture_output=True,
                               text=True, env={**os.environ, 'XDG_RUNTIME_DIR':str(root)})
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(str(root / 'build'), r.stdout.splitlines())
            self.assertIn('.lock-waf_linux_build', r.stdout.splitlines())
            self.assertIn('--targets=unit-tests', r.stdout.splitlines())
            self.assertIn('-j4', r.stdout.splitlines())

    def test_asan_selects_only_the_diagnostic_tree(self):
        with tempfile.TemporaryDirectory(prefix='ndnsf-build-launcher-') as temp:
            root = self.fixture(temp)
            diagnostic = root / 'build-ASAN-UBSAN'
            diagnostic.mkdir()
            (diagnostic / '.lock-waf_build_asan').touch()
            r = subprocess.run(['bash', str(root / 'scripts/build-safe.sh'), 'asan',
                                '--targets=tp-tests-asan', '-j4'], cwd=root, capture_output=True,
                               text=True, env={**os.environ, 'XDG_RUNTIME_DIR':str(root)})
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(str(diagnostic), r.stdout.splitlines())
            self.assertIn('.lock-waf_build_asan', r.stdout.splitlines())
            self.assertIn('--targets=tp-tests-asan', r.stdout.splitlines())

    def test_explicit_run_preserves_callers_working_directory(self):
        with tempfile.TemporaryDirectory(prefix='ndnsf-build-launcher-') as temp:
            root = self.fixture(temp)
            foreign = root / 'build-ASAN-UBSAN'
            foreign.mkdir()
            r = subprocess.run(['bash', str(root / 'scripts/build-safe.sh'), 'run', '--',
                                str(root / 'waf'), 'build'], cwd=foreign, capture_output=True,
                               text=True, env={**os.environ, 'XDG_RUNTIME_DIR':str(root)})
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(str(foreign), r.stdout.splitlines())

    def test_missing_normal_configuration_fails_before_waf(self):
        with tempfile.TemporaryDirectory(prefix='ndnsf-build-launcher-') as temp:
            root = self.fixture(temp)
            (root / 'build/.lock-waf_linux_build').unlink()
            r = subprocess.run(['bash', str(root / 'scripts/build-safe.sh'), 'build'], cwd=root,
                               capture_output=True, text=True,
                               env={**os.environ, 'XDG_RUNTIME_DIR':str(root)})
            self.assertEqual(r.returncode, 2)
            self.assertIn('Configure', r.stderr)


if __name__ == '__main__':
    unittest.main()
