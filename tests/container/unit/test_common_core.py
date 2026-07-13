from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from contract._support import FIXTURES, REPO, load_impl, valid_slurm_evidence

profile_impl = load_impl("profile")
release_impl = load_impl("release")
evidence_impl = load_impl("evidence")
redaction_impl = load_impl("redaction")


class CommonCoreTest(unittest.TestCase):
    def test_runtime_contracts_are_exact_spec_copies(self) -> None:
        pairs = (
            ("contracts/node-profile.schema.json", "schemas/deployment-profile.schema.json"),
            ("contracts/container-evidence.schema.json", "schemas/deployment-evidence.schema.json"),
        )
        spec = REPO / "specs" / "108-ndnsf-di-container-deployment"
        package = REPO / "packaging" / "ndnsf-di-container"
        for source, target in pairs:
            with self.subTest(target=target):
                self.assertEqual((spec / source).read_bytes(), (package / target).read_bytes())

    def test_environment_expansion_is_allowlisted(self) -> None:
        source = (FIXTURES / "profiles" / "cloud-cpu-valid.yaml").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.yaml"
            path.write_text(source.replace("cloud-cpu-a-001", "${NDNSF_RUN_ID}"), encoding="utf-8")
            value = profile_impl.load_profile(path, {"NDNSF_RUN_ID": "allowed-run"})
            self.assertEqual(value["runId"], "allowed-run")
            path.write_text(source.replace("cloud-cpu-a-001", "${AWS_SECRET_ACCESS_KEY}"), encoding="utf-8")
            with self.assertRaisesRegex(profile_impl.ProfileError, "PROFILE_ENV_NOT_ALLOWED"):
                profile_impl.load_profile(path, {"AWS_SECRET_ACCESS_KEY": "not-read"})

    def test_profile_digest_is_canonical(self) -> None:
        left = {"b": 2, "a": 1}
        right = {"a": 1, "b": 2}
        self.assertEqual(profile_impl.profile_digest(left), profile_impl.profile_digest(right))

    def test_materialization_requires_digest_bound_oci_and_sif(self) -> None:
        sha = "a" * 64
        value = release_impl.materialization_record(
            adapter="slurm-apptainer",
            oci_reference=f"registry.example/x@sha256:{sha}",
            materialization_type="sif",
            materialization_id=f"sha256:{sha}",
            runtime_version="1.3.3",
            path="/project/user/ndnsf-di/sif/x.sif",
        )
        self.assertTrue(value["verified"])
        with self.assertRaises(release_impl.ReleaseError):
            release_impl.materialization_record(
                adapter="slurm-apptainer", oci_reference="registry.example/x:latest",
                materialization_type="sif", materialization_id=f"sha256:{sha}",
                runtime_version="1.3.3", path="/tmp/x.sif",
            )

    def test_redaction_removes_secret_keys_and_values(self) -> None:
        value = {"token": "raw", "message": "password=hunter2", "nested": ["safe"]}
        redacted = redaction_impl.redact(value)
        self.assertEqual(redacted["token"], "<redacted>")
        self.assertNotIn("hunter2", redacted["message"])
        self.assertEqual(redaction_impl.secret_findings(redacted), [])

    def test_evidence_promotion_is_exclusive_and_manifest_bound(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "evidence.json"
            source.write_text(json.dumps(valid_slurm_evidence()), encoding="utf-8")
            target = root / "durable" / "run-1"
            result = evidence_impl.promote_evidence([source], target)
            self.assertTrue((target / "evidence.json").is_file())
            self.assertTrue((target / "promotion-manifest.json").is_file())
            self.assertRegex(result["manifestDigest"], r"^sha256:[a-f0-9]{64}$")
            with self.assertRaisesRegex(evidence_impl.EvidenceError, "DESTINATION_EXISTS"):
                evidence_impl.promote_evidence([source], target)

    def test_evidence_promotion_rejects_duplicate_basenames(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            left = root / "left" / "result.json"
            right = root / "right" / "result.json"
            left.parent.mkdir()
            right.parent.mkdir()
            left.write_text("left", encoding="utf-8")
            right.write_text("right", encoding="utf-8")
            with self.assertRaisesRegex(evidence_impl.EvidenceError, "DUPLICATE_BASENAME"):
                evidence_impl.promote_evidence([left, right], root / "durable" / "run")

    def test_evidence_digest_is_canonical(self) -> None:
        self.assertEqual(evidence_impl.canonical_digest({"a": 1, "b": 2}),
                         evidence_impl.canonical_digest({"b": 2, "a": 1}))


if __name__ == "__main__":
    unittest.main()
