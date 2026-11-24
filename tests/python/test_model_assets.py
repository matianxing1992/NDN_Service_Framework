"""Asset admission must never turn a missing or changed package into a test PASS."""
import contextlib
import errno
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Experiments"))
import model_assets as assets

SPEC = importlib.util.spec_from_file_location("prepare_model_assets", ROOT / "Experiments/prepare-model-assets.py")
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)


class AssetAdmissionTest(unittest.TestCase):
    def test_source_cache_is_reusable_and_rejects_bad_cross_filesystem_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            recipe, manifest = self.fixture(root)
            recipe["files"]["canonicalGraph"] = recipe["files"].pop("graph")
            payload = b"external data fixture"
            (root / "sidecar").write_bytes(payload)
            recipe["files"]["canonicalWeights"] = dict(
                path="sidecar", bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())
            manifest.write_text(json.dumps(assets.sealed_manifest(recipe)))
            admitted = assets.validate_manifest(manifest, recipe)
            cache = root / "cache"
            cache.mkdir()
            with patch.object(assets.os, "link", side_effect=OSError(errno.EXDEV, "cross device")), \
                    patch.object(assets.shutil, "copyfile", side_effect=lambda source, path: path.touch()):
                with self.assertRaisesRegex(ValueError, "SOURCE_CACHE_COPY_MISMATCH"):
                    assets.prepare_source_cache(cache, manifest, recipe, admitted)
            self.assertEqual(list(cache.iterdir()), [])

            result = assets.prepare_source_cache(cache, manifest, recipe, admitted)
            self.assertEqual(result, assets.prepare_source_cache(cache, manifest, recipe, admitted))
            location = Path(result["source_directory"])
            digest = recipe["files"]["canonicalWeights"]["sha256"]
            obj = location / "payloads" / "sha256" / digest[:2] / digest
            self.assertTrue(obj.samefile(root / "sidecar"))
            self.assertEqual(obj.read_bytes(), payload)
            (location / "cache-identity.json").write_text("{}")
            with self.assertRaisesRegex(ValueError, "SOURCE_CACHE_IDENTITY_MISMATCH"):
                assets.prepare_source_cache(cache, manifest, recipe, admitted)

    def fixture(self, root):
        # Deliberately synthetic file contract; never identifies a real model.
        payload = b"synthetic asset\n"
        (root / "graph").write_bytes(payload)
        recipe = dict(assetId="synthetic-test-only", recipeVersion=1,
                      repository="test/fixture", revision="a" * 40,
                      license={}, toolVersions={}, transforms=[], modelContract={},
                      files={"graph": dict(path="graph", bytes=len(payload),
                                           sha256=hashlib.sha256(payload).hexdigest())})
        manifest = root / "asset-manifest.json"
        manifest.write_text(json.dumps(assets.sealed_manifest(recipe)))
        return recipe, manifest

    def test_sealed_content_is_bound_to_recipe_not_just_existing_filename(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            recipe, manifest = self.fixture(root)
            self.assertEqual(assets.validate_manifest(manifest, recipe), {"graph": root / "graph"})
            (root / "graph").write_bytes(b"modified asset!\n")
            with self.assertRaisesRegex(ValueError, "COMMITMENT_MISMATCH"):
                assets.validate_manifest(manifest, recipe)

    def test_unsealed_wrong_revision_and_forged_file_commitment_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            recipe, manifest = self.fixture(root)
            original = assets.sealed_manifest(recipe)
            for field, value in (("state", "STAGED"), ("revision", "b" * 40),
                                 ("files", {"graph": {"path": "graph", "bytes": 1, "sha256": "0" * 64}})):
                with self.subTest(field=field):
                    changed = dict(original)
                    changed[field] = value
                    manifest.write_text(json.dumps(changed))
                    with self.assertRaisesRegex(ValueError, "RECIPE_MISMATCH"):
                        assets.validate_manifest(manifest, recipe)

    def test_missing_sidecar_and_symlink_escape_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            recipe, manifest = self.fixture(root)
            (root / "graph").unlink()
            with self.assertRaises(FileNotFoundError):
                assets.validate_manifest(manifest, recipe)
            (root / "graph").symlink_to(ROOT / "README.md")
            with self.assertRaisesRegex(ValueError, "SYMLINK_REJECTED"):
                assets.validate_manifest(manifest, recipe)
            with self.assertRaisesRegex(ValueError, "PATH_INVALID"):
                assets.contained_file(root, "../escape")

    def test_json_numeric_aliases_do_not_change_declared_schema_types(self):
        with tempfile.TemporaryDirectory() as tmp:
            recipe, manifest = self.fixture(Path(tmp))
            for field in ('version', 'size'):
                value = json.loads(json.dumps(assets.sealed_manifest(recipe)))
                if field == 'version':
                    value['recipeVersion'] = True
                else:
                    value['files']['graph']['bytes'] = float(value['files']['graph']['bytes'])
                manifest.write_text(json.dumps(value))
                with self.assertRaisesRegex(ValueError, 'RECIPE_MISMATCH'):
                    assets.validate_manifest(manifest, recipe)

    def test_duplicate_json_fields_cannot_hide_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            path.write_text('{"state":"STAGED","state":"SEALED"}')
            with self.assertRaisesRegex(ValueError, "DUPLICATE_FIELD"):
                assets.read_json(path)

    def test_offline_miss_has_no_output_or_download(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "empty-cache"
            with contextlib.redirect_stderr(io.StringIO()) as error, patch.object(prepare, "verify_tools") as tools:
                self.assertEqual(prepare.main(["--recipe", "qwen3-0.6b-int4", "--asset-root", str(root), "--offline"]), 2)
                self.assertIn("ASSET_OFFLINE_MISS", error.getvalue())
                tools.assert_not_called()
            self.assertFalse(root.exists())

    def test_interrupted_conversion_does_not_leave_sealed_or_large_partial(self):
        for failure in (ValueError("bad weights"), KeyboardInterrupt()):
            with self.subTest(failure=type(failure).__name__), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp) / "owned"
                sentinel = Path(tmp) / "unrelated"
                sentinel.write_text("preserve")
                def interrupted(*args):
                    (root / "partial.onnx").write_bytes(b"partial")
                    raise failure
                with contextlib.redirect_stderr(io.StringIO()), patch.object(prepare, "verify_tools", return_value=(Path("normalize"), Path("download"))), patch.object(prepare, "prepare", side_effect=interrupted):
                    self.assertEqual(prepare.main(["--recipe", "qwen3-0.6b-int4", "--asset-root", str(root), "--allow-download"]), 2)
                self.assertFalse(root.exists())
                self.assertEqual(sentinel.read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
