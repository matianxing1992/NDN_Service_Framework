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
    def test_explicit_profile_binds_portable_inputs_and_native_roles(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            path = root / "profile.json"

            def commit(name, value):
                payload = json.dumps(value).encode()
                (root / name).write_bytes(payload)
                return dict(path=name, bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())

            catalog = dict(model=dict(model_name="fixture", source_revision="frozen",
                                      precision="float16", quantization_subtype="weight_only_int4"),
                           splitter=dict(roles=["/Stage/0"], tensor_degrees=[2],
                                         tensor_rank_weights_by_role={"/Stage/0": [3, 1]}))
            roles = ["/Stage/0/Tensor/Rank/0", "/Stage/0/Tensor/Rank/1"]
            profile = dict(schema="ndnsf-experiment-profile-v1", id="two-rank-derived",
                           assetRecipe=commit("asset.json", {"recipeVersion": 1, "revision": "frozen"}),
                           catalog=commit("catalog.json", catalog),
                           generationOptions=commit("generation.json", {"maxNewTokens": 32}),
                           model=dict(repository="fixture", revision="frozen", precision="float16",
                                      quantization="weight_only_int4"),
                           budgets=dict(maxPreparedBytes=10 << 30, artifactCacheMaxMiB=8192,
                                        conversationRetentionMs=600000, requestTimeoutMs=300000),
                           providerRoles=roles[::-1], turns=["first", "next"],
                           rendering=dict(addSpecialTokens=False, bos="none"))

            def load():
                path.write_text(json.dumps(profile))
                return prepare.load_explicit_profile(path)

            loaded, _, native, _, canonical_roles = load()
            self.assertEqual(loaded["providerRoles"], roles[::-1])
            self.assertEqual(canonical_roles, roles)
            self.assertEqual(native["splitter"]["tensor_rank_weights_by_role"], {"/Stage/0": [3, 1]})
            profile["assetRecipe"] = commit("asset.json", {"recipeVersion": 1, "revision": "different"})
            with self.assertRaisesRegex(ValueError, "ASSET_REVISION_MISMATCH"):
                load()
            profile["assetRecipe"] = commit("asset.json", {"recipeVersion": 1, "revision": "frozen"})
            profile["providerRoles"] = [roles[0], roles[0]]
            with self.assertRaisesRegex(ValueError, "ROLE_MAP_INVALID"):
                load()
            profile["providerRoles"] = roles
            profile["budgets"]["requestTimeoutMs"] = 600000
            with self.assertRaisesRegex(ValueError, "CONVERSATION_RETENTION_MUST_OUTLAST_REQUEST"):
                load()
            profile["budgets"]["requestTimeoutMs"] = 300000
            profile["budgets"]["artifactCacheMaxMiB"] = True
            with self.assertRaisesRegex(ValueError, "ARTIFACT_CACHE_BUDGET_INVALID"):
                load()
            profile["budgets"]["artifactCacheMaxMiB"] = 8192
            (root / "catalog.json").write_text("{}")
            with self.assertRaisesRegex(ValueError, "FILE_COMMITMENT_MISMATCH"):
                load()

    def test_slurm_placement_preserves_roles_and_scopes_devices_and_routes(self):
        for gpu_count in (0, 1):
            with self.subTest(gpu_count=gpu_count), tempfile.TemporaryDirectory() as tmp:
                cache = Path(tmp) / "shared-source"
                cache.mkdir()
                (cache / "cache-identity.json").write_text("{}")
                placement = dict(platform=dict(kind="slurm-apptainer", slurm=dict(gpu=dict(count=gpu_count))),
                    release={"prefix": "/image"}, turns=["first", "next"],
                    model_input=dict(mode="source-cache", source_directory=str(cache),
                                     source_namespace="sha256:" + "a" * 64),
                    nodes=[dict(id=node, host="host-" + node, gpu_devices=["allocated:0"] * gpu_count)
                           for node in ("a", "b")])
                target = Path(tmp) / "placement.json"
                target.write_text(json.dumps(placement))
                effective = prepare.load_slurm_placement(target)
                config = dict(network={"group": "/run/group"},
                    requester={"fixed_placement": {"role_providers": {"/Stage/Last": "/run/p1"}}},
                    actors=[dict(id=name, identity="/run/" + name, kind=kind, resources={}, parameters={})
                            for name, kind in (("requester", "requester"),
                                               ("p0", "provider"), ("p1", "provider"))])
                prepare.apply_slurm_placement(config, effective)
                self.assertEqual([actor["node"] for actor in config["actors"]], ["a", "a", "b"])
                self.assertEqual(config["actors"][0]["resources"]["gpu_devices"], [])
                for actor in config["actors"][1:]:
                    self.assertEqual(actor["resources"]["gpu_devices"], ["allocated:0"] * gpu_count)
                    self.assertEqual(actor["parameters"]["offer-backend"],
                                     "onnxruntime-cuda" if gpu_count else "onnxruntime-cpu")
                self.assertIn({"from": "a", "to": "b", "prefix": "/run/p1"}, config["network"]["routes"])
                self.assertEqual(config["requester"]["fixed_placement"]["role_providers"],
                                 {"/Stage/Last": "/run/p1"})
                placement["nodes"][1]["host"] = placement["nodes"][0]["host"]
                target.write_text(json.dumps(placement))
                with self.assertRaisesRegex(ValueError, "SLURM_TWO_NODE_PLACEMENT_INVALID"):
                    prepare.load_slurm_placement(target)

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
