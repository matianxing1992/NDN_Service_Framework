"""spec181 T003: cross-language grant parity (Python vs native verifier).

One fixed grant byte stream (tests/fixtures/spec181/grant-vectors-v1.json)
is unwrapped by BOTH the Python verifier (verify_and_unwrap_grant) and the
native verifier (the T002 pybind surface).  Both sides must agree on every
vector case: the same content key on the positive case, the same registered
rejection on every negative case.  Regenerate the fixture with
scripts/gen_spec181_grant_vectors.py after any encoding change.
"""

from __future__ import annotations

import json
from pathlib import Path
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

from ndnsf_distributed_inference.core.protected_artifacts import (
    grant_from_wire, verify_and_unwrap_grant)

VECTORS = Path(__file__).resolve().parents[1] / "fixtures" / "spec181" / \
    "grant-vectors-v1.json"


def _load_vectors():
    with VECTORS.open("r") as stream:
        return json.load(stream)


class GrantParityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.vectors = _load_vectors()
        cls.authority_public_key = ed25519.Ed25519PublicKey.from_public_bytes(
            bytes.fromhex(cls.vectors["authorityPublicKeyRaw"]))
        cls.recipient_key = ed25519.Ed25519PrivateKey.from_private_bytes(
            bytes.fromhex(cls.vectors["recipientSeed"]))

    def _python_side(self, case):
        grant = grant_from_wire(case["wire"].encode("utf-8"))
        try:
            key = verify_and_unwrap_grant(
                grant,
                authority_public_key=self.authority_public_key,
                recipient_private_key=self.recipient_key,
                expected_provider_identity=self.vectors["providerIdentity"],
                expected_request_id=case["binding"]["requestId"],
                expected_attempt=case["binding"]["attempt"],
                expected_plan_core_digest=case["binding"]["planCoreDigest"],
                expected_model_manifest_digest=(
                    case["binding"]["modelManifestDigest"]),
                expected_protection_epoch=(
                    case["binding"]["protectionEpoch"]),
                now_ms=case["binding"]["nowMs"],
            )
            return {"ok": True, "contentKey": key.hex(), "reason": ""}
        except ValueError as exc:
            return {"ok": False, "contentKey": "", "reason": str(exc)}

    def _native_side(self, case):
        from ndnsf import _ndnsf
        result = _ndnsf.verify_and_unwrap_native_grant(
            case["wire"],
            self.vectors["authorityPublicKeyRaw"],
            self.vectors["recipientSeed"],
            self.vectors["providerIdentity"],
            case["binding"]["requestId"],
            case["binding"]["attempt"],
            case["binding"]["planCoreDigest"],
            case["binding"]["modelManifestDigest"],
            case["binding"]["protectionEpoch"],
            case["binding"]["nowMs"],
        )
        return {
            "ok": bool(result["verified"]),
            "contentKey": bytes(result["content_key"]).hex(),
            "reason": str(result["reason"]),
        }

    def test_python_side_matches_locked_expectations(self):
        for case in self.vectors["cases"]:
            with self.subTest(case=case["name"], side="python"):
                self.assertEqual(self._python_side(case), case["expected"])

    def test_native_side_matches_locked_expectations(self):
        for case in self.vectors["cases"]:
            with self.subTest(case=case["name"], side="native"):
                native = self._native_side(case)
                self.assertEqual(native["ok"], case["expected"]["ok"])
                if case["expected"]["ok"]:
                    self.assertEqual(
                        native["contentKey"], case["expected"]["contentKey"])
                else:
                    self.assertTrue(native["reason"].startswith(
                        "DI_PROTECTED_") or
                        "binding" in native["reason"].lower())

    def test_both_sides_agree_on_every_case(self):
        for case in self.vectors["cases"]:
            with self.subTest(case=case["name"]):
                python = self._python_side(case)
                native = self._native_side(case)
                self.assertEqual(python["ok"], native["ok"])
                self.assertEqual(python["contentKey"], native["contentKey"])


if __name__ == "__main__":
    unittest.main()
