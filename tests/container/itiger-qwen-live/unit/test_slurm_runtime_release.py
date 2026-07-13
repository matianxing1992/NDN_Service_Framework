from __future__ import annotations

import copy
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
LIB = REPO / "packaging" / "ndnsf-di-container" / "lib"
if str(LIB) not in sys.path:
    sys.path.insert(0, str(LIB))

from adapters import slurm_apptainer
import release


class SlurmRuntimeReleaseTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.sif = Path(self.temp.name) / "runtime.sif"
        self.sif.write_bytes(b"immutable-sif")
        digest = "sha256:" + "a" * 64
        self.record = {
            "schemaVersion": "spec110-runtime-release-v1",
            "releaseId": "spec110-r1",
            "sourceRevision": "8b9a4fe709d35b9e4d4961eaa25cefad45cfc0b2",
            "imageReference": "ghcr.io/example/ndnsf-di@" + digest,
            "imageDigest": digest,
            "manifestDigest": digest,
            "sbomDigest": digest,
            "provenanceDigest": digest,
            "signatureBundleDigest": digest,
            "signatureIdentity": {"issuer": "https://token.actions.githubusercontent.com", "subject": "repo:example/ndnsf"},
            "transparencyLog": "rekor-entry-1",
            "visibility": "private",
            "authMode": "ghcr-token",
            "immutable": True,
            "physicalProduction": "DEFERRED",
        }
        self.record["recordDigest"] = release.manifest_digest(self.record)
        self.materialization = {
            "verified": True,
            "ociReference": self.record["imageReference"],
            "ociDigest": digest,
            "sifPath": str(self.sif),
            "sifSha256": release.sha256_file(self.sif),
        }
        self.cluster = {
            "driverCuda": {"driver": "550.54.14", "cuda": "12.4"},
            "apptainerVersions": {"login": "1.3.3", "compute": "1.3.3"},
        }

    def test_accepts_bound_release_sif_and_cluster(self) -> None:
        result = slurm_apptainer.validate_runtime_release(
            self.record, self.materialization, self.cluster
        )
        self.assertEqual("PASS", result["status"])
        self.assertEqual(self.materialization["sifSha256"], result["sifSha256"])

    def test_rejects_release_record_tamper(self) -> None:
        changed = copy.deepcopy(self.record)
        changed["signatureBundleDigest"] = "sha256:" + "c" * 64
        with self.assertRaisesRegex(
            slurm_apptainer.SlurmAdapterError, "RUNTIME_RELEASE_INVALID:SPEC110_RELEASE_RECORD_TAMPERED"
        ):
            slurm_apptainer.validate_runtime_release(changed, self.materialization, self.cluster)

    def test_rejects_sif_tamper(self) -> None:
        self.sif.write_bytes(b"tampered-sif")
        with self.assertRaisesRegex(slurm_apptainer.SlurmAdapterError, "RUNTIME_SIF_TAMPERED"):
            slurm_apptainer.validate_runtime_release(self.record, self.materialization, self.cluster)

    def test_rejects_old_compute_driver(self) -> None:
        changed = copy.deepcopy(self.cluster)
        changed["driverCuda"]["driver"] = "535.104.05"
        with self.assertRaisesRegex(slurm_apptainer.SlurmAdapterError, "RUNTIME_DRIVER_TOO_OLD"):
            slurm_apptainer.validate_runtime_release(self.record, self.materialization, changed)


if __name__ == "__main__":
    unittest.main()
