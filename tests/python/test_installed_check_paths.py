"""Installed SDK check preflight must work outside a Git checkout."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class InstalledCheckPaths(unittest.TestCase):
    def invoke(self, root, manifest):
        root = Path(root)
        script = root / 'checks/run-spec185-installed-consumer.sh'
        script.parent.mkdir()
        source = Path(__file__).resolve().parents[1] / 'installed-api'
        shutil.copyfile(source / script.name, script)
        shutil.copyfile(source / 'spec185-installed-consumer.cpp',
                        script.parent / 'spec185-installed-consumer.cpp')
        prefix = root / 'prefix'
        prefix.mkdir()
        if manifest:
            path = prefix / 'share/ndnsf/stack-install-manifest.json'
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps({'files': {}}))
        env = {k:v for k,v in os.environ.items() if k != 'NDNSF_INSTALL_MANIFEST'}
        return subprocess.run(['bash', str(script), *([str(prefix)] * 4), str(root / 'output')], cwd=root,
                              capture_output=True, text=True, env=env), prefix

    def test_missing_manifest_names_the_selected_prefix(self):
        with tempfile.TemporaryDirectory(prefix='ndnsf-sdk-path-check-') as root:
            result, prefix = self.invoke(root, False)
            self.assertEqual(result.returncode, 6)
            self.assertIn(str(prefix / 'share/ndnsf/stack-install-manifest.json'), result.stderr)
            self.assertNotIn('not a git repository', result.stderr)

    def test_existing_manifest_is_read_without_git(self):
        with tempfile.TemporaryDirectory(prefix='ndnsf-sdk-path-check-') as root:
            result, prefix = self.invoke(root, True)
            self.assertEqual(result.returncode, 6)
            self.assertIn('INSTALL_MANIFEST_HAS_NO_HEADERS:', result.stderr)
            self.assertNotIn('INSTALL_MANIFEST_MISSING:', result.stderr)

    def test_same_install_cannot_qualify_opposite_backend_variants(self):
        with tempfile.TemporaryDirectory(prefix='ndnsf-sdk-path-check-') as root:
            _, prefix = self.invoke(root, True)
            manifest = prefix / 'share/ndnsf/stack-install-manifest.json'
            manifest.write_text(json.dumps({'files': {
                str(prefix / 'include/ndn-service-framework/ServiceUser.hpp'): {}}}))
            result = subprocess.run([
                'bash', str(Path(root) / 'checks/run-spec185-installed-consumer.sh'),
                *([str(prefix)] * 4), str(Path(root) / 'output')], cwd=root,
                capture_output=True, text=True,
                env={k: v for k, v in os.environ.items() if k != 'NDNSF_INSTALL_MANIFEST'})
            self.assertEqual(result.returncode, 5)
            self.assertIn('BACKEND_VARIANT_PREFIX_REUSED:', result.stderr)
            self.assertNotIn('PASS', result.stdout)

    def test_single_prefix_requires_a_declared_backend_variant(self):
        with tempfile.TemporaryDirectory(prefix='ndnsf-sdk-path-check-') as root:
            _, prefix = self.invoke(root, False)
            result = subprocess.run([
                'bash', str(Path(root) / 'checks/run-spec185-installed-consumer.sh'),
                '--single', str(prefix), 'cuda', str(Path(root) / 'output')],
                cwd=root, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('BACKEND_VARIANT_INVALID:cuda', result.stderr)
            self.assertNotIn('PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
