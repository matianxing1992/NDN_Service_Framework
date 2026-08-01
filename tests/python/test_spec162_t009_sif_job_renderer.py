from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = (
    ROOT
    / "specs/162-itiger-qwen36-generation/jobs"
    / "render-t009-sif-jobs.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location(
        "render_t009_sif_jobs", SCRIPT
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class T009SifJobRendererTests(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_renders_candidate_bound_jobs_and_plan(self):
        output = self.root / "rendered"

        plan = self.module.render(output_dir=output)

        self.assertEqual(
            plan["status"], "READY_FOR_EXPLICIT_AUTHORIZATION"
        )
        self.assertFalse(plan["submitted"])
        self.assertEqual(plan["sourceKind"], "sealed-docker-archive")
        self.assertEqual(
            plan["sourceManifestDigest"],
            self.module.sha256_file(output / "source-manifest.json"),
        )
        self.assertEqual(
            plan["localImageId"], self.module.EXPECTED_IMAGE_ID
        )
        self.assertEqual(
            set(plan["harness"]),
            {
                "local-docker-operation-status-inner.sh",
                "local-docker-operation-status-policy.yaml",
                "nfd.conf.in",
            },
        )
        for script in (
            output / "materialize.sbatch",
            output / "operation-status-sif-smoke.sbatch",
        ):
            text = script.read_text(encoding="utf-8")
            self.assertNotRegex(text, r"@[A-Z][A-Z0-9_]*@")
            subprocess.run(["bash", "-n", str(script)], check=True)
        self.assertIn(
            plan["materialization"]["submissionId"],
            plan["materialization"]["exactCommand"],
        )
        self.assertIn(
            "SPEC162_T009_SIF_SHA256=<64-lowercase-hex>",
            plan["gpuSmoke"]["commandTemplate"],
        )

    def test_rejects_changed_archive_identity(self):
        manifest = self.module.expected_archive_manifest()
        manifest["archive"]["bytes"] += 1
        raw = self.module.canonical_manifest_bytes(manifest)
        with self.assertRaisesRegex(
            self.module.RenderError,
            "ARCHIVE_MANIFEST_IDENTITY_MISMATCH",
        ):
            self.module.validate_manifest(raw, manifest)

    def test_rejects_noncanonical_manifest_bytes(self):
        manifest = self.module.expected_archive_manifest()
        raw = self.module.canonical_manifest_bytes(manifest) + b"\n"
        with self.assertRaisesRegex(
            self.module.RenderError,
            "ARCHIVE_MANIFEST_NOT_CANONICAL",
        ):
            self.module.validate_manifest(raw, manifest)

    def test_rejects_wrong_local_image_id(self):
        manifest = self.module.expected_archive_manifest()
        manifest["localImageId"] = "sha256:" + ("b" * 64)
        raw = self.module.canonical_manifest_bytes(manifest)
        with self.assertRaisesRegex(
            self.module.RenderError,
            "ARCHIVE_MANIFEST_IDENTITY_MISMATCH",
        ):
            self.module.validate_manifest(raw, manifest)

    def test_rejects_nonempty_output_directory(self):
        output = self.root / "rendered"
        output.mkdir()
        (output / "old").write_text("old", encoding="utf-8")
        with self.assertRaisesRegex(
            self.module.RenderError,
            "OUTPUT_DIRECTORY_NOT_EMPTY",
        ):
            self.module.render(output_dir=output)

    def test_renders_explicit_requalified_native_candidate(self):
        output = self.root / "requalified"
        candidate = self.module.CandidateSource(
            image_id="sha256:" + ("c" * 64),
            archive_path="/project/test/native.tar.gz",
            archive_sha256="d" * 64,
            archive_bytes=123456,
            harness_root="/project/test/harness",
            attempt_sequence=4,
            predecessor_materialization="job-previous",
        )

        plan = self.module.render(
            output_dir=output,
            candidate=candidate,
        )

        self.assertEqual(plan["localImageId"], candidate.image_id)
        self.assertEqual(plan["archive"], candidate.archive_path)
        self.assertEqual(plan["archiveBytes"], candidate.archive_bytes)
        self.assertEqual(plan["attemptSequence"], 4)
        self.assertEqual(
            plan["predecessorMaterialization"], "job-previous"
        )
        materialize = (output / "materialize.sbatch").read_text(
            encoding="utf-8"
        )
        self.assertIn("SPEC164_NATIVE_RUNTIME_PASS", materialize)


if __name__ == "__main__":
    unittest.main()
