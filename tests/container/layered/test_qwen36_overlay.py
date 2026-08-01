#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
LAYERED = ROOT / "packaging/ndnsf-di-container/oci/layered"
LOCK = LAYERED / "locks/qwen36-overlay.lock.json"
PREPARE = LAYERED / "scripts/prepare-qwen36-wheelhouse.py"
VERIFY = LAYERED / "scripts/verify-qwen36-runtime.py"
DOCKERFILE = LAYERED / "Dockerfile.qwen36"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class Qwen36OverlayTests(unittest.TestCase):
    def test_lock_freezes_complete_observed_wheel_closure(self) -> None:
        lock = json.loads(LOCK.read_text(encoding="utf-8"))
        wheels = lock["wheelClosure"]

        self.assertEqual(len(wheels), 19)
        self.assertEqual(wheels["transformers"]["version"], "5.14.1")
        self.assertEqual(wheels["tokenizers"]["version"], "0.22.2")
        self.assertEqual(wheels["huggingface-hub"]["version"], "1.25.1")
        for package, row in wheels.items():
            self.assertRegex(row["sha256"], r"^[0-9a-f]{64}$", package)
            self.assertTrue(row["url"].startswith(
                "https://files.pythonhosted.org/packages/"), package)
            self.assertTrue(row["url"].endswith(row["filename"]), package)

    def test_wheelhouse_verifier_and_requirements_are_hash_bound(self) -> None:
        prepare = load_module("prepare_qwen36_wheelhouse", PREPARE)
        payload = b"immutable-wheel-fixture"
        digest = hashlib.sha256(payload).hexdigest()
        lock = {
            "wheelClosure": {
                "example-package": {
                    "version": "1.2.3",
                    "filename": "example_package-1.2.3-py3-none-any.whl",
                    "sha256": digest,
                    "url": "https://files.pythonhosted.org/packages/example.whl",
                }
            }
        }
        with tempfile.TemporaryDirectory(prefix="qwen36-wheelhouse-") as tmp:
            root = Path(tmp)
            (root / lock["wheelClosure"]["example-package"]["filename"]).write_bytes(
                payload)
            prepare.verify_wheelhouse(lock, root)
            requirements = prepare.write_requirements(lock, root)
            text = requirements.read_text(encoding="utf-8")

        self.assertIn("example-package==1.2.3", text)
        self.assertIn(f"--hash=sha256:{digest}", text)

    def test_overlay_build_is_offline_and_parent_bound(self) -> None:
        dockerfile = DOCKERFILE.read_text(encoding="utf-8")

        self.assertIn("ARG APP_RUNTIME_IMAGE", dockerfile)
        self.assertIn("FROM ${APP_RUNTIME_IMAGE}", dockerfile)
        self.assertIn("COPY --from=qwen36_wheelhouse", dockerfile)
        self.assertIn("--no-index", dockerfile)
        self.assertIn("--require-hashes", dockerfile)
        self.assertIn("org.ndnsf.di.qwen36-lock-digest", dockerfile)

    def test_runtime_probe_disables_pip_cache_for_read_only_execution(self) -> None:
        probe = VERIFY.read_text(encoding="utf-8")

        self.assertIn('"--disable-pip-version-check"', probe)
        self.assertIn('"--no-cache-dir"', probe)


if __name__ == "__main__":
    unittest.main()
