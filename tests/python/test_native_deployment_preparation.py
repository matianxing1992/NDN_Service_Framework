"""Fresh input preparation rejects bad delivery before creating credentials."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Experiments"))
SPEC = importlib.util.spec_from_file_location("native_deployment_preparation", ROOT / "Experiments/prepare-native-deployment.py")
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)


class NativePreparationBoundaryTest(unittest.TestCase):
    def test_role_permission_handles_flat_and_absolute_roles(self):
        policy = prepare.policy_text(
            dict(controller='/controller', authority='/authority', requester='/user',
                 provider='/provider'),
            dict(service='/infer', artifact_service='/grant'),
            {'provider': ['umt5-single', '/dit/rank/0']}, None)
        self.assertIn('/infer/ROLE/umt5-single\n', policy)
        self.assertIn('/infer/ROLE/dit/rank/0\n', policy)
        with self.assertRaisesRegex(ValueError, 'absolute Name or a safe single label'):
            prepare.policy_text(
                dict(controller='/controller', authority='/authority', requester='/user',
                     provider='/provider'),
                dict(service='/infer', artifact_service='/grant'),
                {'provider': ['umt5/single']}, None)

    def test_pipeline_profiles_freeze_comparable_roles_and_workload(self):
        resources = ROOT / 'Experiments'
        recipe = prepare.read_json(resources / 'recipes/qwen3-0.6b-int4.json')
        split_catalogs = []
        placements = []
        for name in prepare.PIPELINE_PROFILES:
            profile, layout = prepare.load_profile(resources, name)
            catalog = prepare.read_json(resources / profile['catalog'])
            options = prepare.read_json(resources / profile['generationOptions'])
            roles = prepare.apply_pipeline_layout(catalog, layout,
                recipe['files']['canonicalGraph'], recipe['files']['canonicalWeights'], options)
            self.assertEqual(profile['rendering']['inputTokens'], [24, 24])
            self.assertEqual(profile['rendering']['maxNewTokens'], [32, 32])
            self.assertEqual(profile['rendering']['expectedCommittedPrefix'], [56, 112])
            self.assertEqual(profile['budgets']['storageBytes'], 8 * 1024 ** 3)
            self.assertEqual(catalog['splitter']['tensor_degrees'], [1] * len(roles))
            self.assertRegex(catalog['model']['graph_digest'], r'^sha256:[0-9a-f]{64}$')
            for field, option in (('state_inputs', 'stateInputNames'),
                                  ('state_outputs', 'stateOutputNames')):
                tensors = [name for role in roles for names in catalog[field][role].values()
                           for name in names]
                self.assertCountEqual(tensors, options[option])
            split_catalogs.append(catalog)
            placements.append(layout['stageProviders'])

        self.assertEqual(split_catalogs[0], split_catalogs[1])
        self.assertEqual(split_catalogs[0]['splitter']['layer_ranges'], [[0, 14], [14, 28]])
        self.assertEqual(split_catalogs[2]['splitter']['layer_ranges'], [[0, 28]])
        self.assertNotEqual(split_catalogs[0]['model']['graph_digest'],
                            split_catalogs[2]['model']['graph_digest'])
        self.assertEqual(placements, [[0, 0], [0, 1], [0]])

    def fixture_prefix(self, root):
        prefix = root / 'prefix'
        ledger = dict(schema=1, files={})
        names = ['bin/' + name for name in prepare.NATIVE_PROGRAMS]
        names += ['libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker',
                  'lib/libndn-service-framework.so', 'lib/libndnsf-distributed-inference.so']
        names += ['share/ndnsf/Experiments/' + name for name in prepare.PREPARATION_RESOURCES]
        for name in names:
            path = prefix / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'synthetic delivery identity only\n')
            path.chmod(0o755)
            ledger['files'][str(path)] = dict(installed=dict(kind='file', mode=0o755,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        receipt = prefix / 'share/ndnsf/stack-install-manifest.json'
        receipt.write_text(json.dumps(ledger))
        return prefix, receipt, ledger

    def test_selected_prefix_files_are_hashed_not_just_present(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefix, receipt, _ = self.fixture_prefix(Path(tmp))
            self.assertEqual(prepare.installed_inputs(prefix), receipt)
            (prefix / 'bin/ndnsf-di-tokenize-input').write_bytes(b'changed executable')
            with self.assertRaisesRegex(ValueError, 'INSTALLED_FILE_MISMATCH'):
                prepare.installed_inputs(prefix)

    def test_external_symlink_cannot_borrow_another_installation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            prefix, receipt, ledger = self.fixture_prefix(root)
            foreign = root / 'foreign-program'
            foreign.write_bytes(b'foreign')
            path = prefix / 'bin/ndnsf-di'
            path.unlink()
            path.symlink_to(foreign)
            ledger['files'][str(path)] = dict(installed=dict(kind='symlink', target=str(foreign)))
            receipt.write_text(json.dumps(ledger))
            with self.assertRaises(ValueError):
                prepare.installed_inputs(prefix)

    def test_invalid_namespace_and_existing_work_root_never_generate_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for namespace, work in [('/bad namespace', root / 'absent'), ('/valid', root / 'retained')]:
                if work.name == 'retained':
                    work.mkdir()
                    (work / 'keep').write_text('retained')
                with contextlib.redirect_stderr(io.StringIO()), patch.object(prepare, 'installed_inputs') as installed:
                    self.assertEqual(prepare.main(['--profile', 'qwen06b-tp-cpu', '--prefix', str(root / 'prefix'),
                        '--asset-manifest', str(root / 'asset-manifest.json'), '--work-root', str(work),
                        '--namespace', namespace]), 2)
                    installed.assert_not_called()
            self.assertFalse((root / 'absent').exists())
            self.assertEqual((root / 'retained/keep').read_text(), 'retained')

    def test_owned_finite_command_preserves_error_and_outside_cwd(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            env = dict(PATH='/usr/bin:/bin', HOME=str(root))
            result = prepare.run([sys.executable, '-c', 'from pathlib import Path; print(Path.cwd())'], env, root)
            self.assertEqual(result.strip(), str(root))
            with self.assertRaisesRegex(ValueError, 'first boundary'):
                prepare.run([sys.executable, '-c', 'import sys; sys.stderr.write("first boundary"); sys.exit(7)'], env, root)

    def test_ephemeral_conversation_root_rejected_before_key_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with contextlib.redirect_stderr(io.StringIO()) as errors, patch.object(prepare, 'installed_inputs') as installed:
                self.assertEqual(prepare.main(['--profile', 'qwen06b-tp-cpu', '--prefix', str(root / 'prefix'),
                    '--asset-manifest', str(root / 'asset-manifest.json'), '--work-root', str(root / 'new'),
                    '--namespace', '/portable/test']), 2)
                installed.assert_not_called()
            self.assertIn('PERSISTENT_WORK_ROOT_REQUIRED', errors.getvalue())
            self.assertFalse((root / 'new').exists())


if __name__ == '__main__':
    unittest.main()
