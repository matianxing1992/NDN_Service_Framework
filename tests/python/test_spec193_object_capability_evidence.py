"""Regression for the object-capability oracle's native-evidence assertions.

The request and ACKs travel encrypted, so neither their names nor their wire appear in a capture. The
oracle therefore asserts on the records the admitting requester publishes. These cases pin the properties
that must still fail closed — one ACK per expected provider, the right signer, a trust-validated admission,
a digest that matches what the client accepted — without needing a MiniNDN run.
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
ORACLE = ROOT / "tests/minindn/spec193_packet_oracle.py"

spec = importlib.util.spec_from_file_location("spec193_packet_oracle_under_test", ORACLE)
oracle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oracle)

PROVIDER = "/example/repo/provider/repoA"
OTHER = "/example/repo/provider/repoB"
USER = "/example/repo/user"
DIGEST = "sha256:" + "a" * 64
OTHER_DIGEST = "sha256:" + "b" * 64
LOCATOR = "/example/repo/provider/repoA/KEY/1"


class _Signature:
    """Minimal stand-in for the certificate signature the oracle inspects."""

    def __init__(self, locator: str) -> None:
        name = mock.Mock()
        name.__str__ = lambda self: locator  # pragma: no cover - repr helper
        self.signature_info = mock.Mock()
        self.signature_info.key_locator = mock.Mock()
        self.signature_info.key_locator.name = locator


def record(provider=PROVIDER, signer=None, validated=True, digest=DIGEST,
           repo_node="/example/repo/node/repoA"):
    return {"provider": provider, "signerIdentity": signer or provider,
            "trustSchemaValidated": validated, "wireDigest": digest,
            "capabilityRepoNode": repo_node}


class ObjectCapabilityEvidenceTest(unittest.TestCase):
    def run_oracle(self, records, *, digest=DIGEST, locator=LOCATOR, request_id="req-1"):
        certificates = {"neu": (USER + "/KEY/1", None, _Signature("/example/repo"), "c" * 64),
                        "ucla": (LOCATOR, None, _Signature("/example/repo"), "d" * 64),
                        "wustl": (OTHER + "/KEY/1", None, _Signature("/example/repo"), "e" * 64)}
        # A self-issued root: its key locator is a prefix of its own name.
        root = (USER, None, _Signature("/example/repo"), "f" * 64)
        def certificate(path):
            name = Path(path).name
            if name == "root.cert":
                return root
            return certificates[name.split(".", 1)[0]]

        with mock.patch.object(oracle, "_certificate", side_effect=certificate), \
                mock.patch.object(oracle, "_verify_signature", lambda *a, **k: None):
            return oracle.verify_object_capability(
                Path("/nonexistent"), "/svc", USER, PROVIDER, OTHER,
                records, request_id, digest, locator)

    def test_admitted_ack_passes_and_reports_the_set(self) -> None:
        result = self.run_oracle([record()])
        self.assertEqual(result["accepted_provider"], PROVIDER)
        self.assertEqual(result["accepted_ack_sha256"], DIGEST)
        self.assertEqual(result["selected_repo_node"], "/example/repo/node/repoA")
        self.assertEqual([item["provider"] for item in result["admitted_acks"]], [PROVIDER])

    def test_no_admission_fails_closed(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "admitted no provider ACK"):
            self.run_oracle([])

    def test_unvalidated_ack_fails_closed(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "not trust-schema validated"):
            self.run_oracle([record(validated=False)])

    def test_signer_must_match_the_provider(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "signer differs from its provider"):
            self.run_oracle([record(signer="/example/repo/provider/repoB")])

    def test_unexpected_provider_fails_closed(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "unexpected provider"):
            self.run_oracle([record(provider="/example/repo/provider/repoC")])

    def test_digest_must_match_the_accepted_ack(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "differs from the admitted ACK"):
            self.run_oracle([record(digest=OTHER_DIGEST)])

    def test_duplicate_ack_from_one_provider_fails_closed(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "exactly one signed ACK"):
            self.run_oracle([record(), record(digest=OTHER_DIGEST)])

    def test_disagreeing_repo_nodes_fail_closed(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "disagree on the selected Repo node"):
            self.run_oracle([record(), record(provider=OTHER, digest=OTHER_DIGEST,
                                              repo_node="/example/repo/node/repoB")])

    def test_malformed_request_id_fails_closed(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "request ID is malformed"):
            self.run_oracle([record()], request_id="/seg/ment")


if __name__ == "__main__":
    unittest.main()
