from __future__ import annotations

import hashlib
import hmac
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.core.group_capability import (  # noqa: E402
    GroupMemberV1,
    GroupOperationV1,
    seal_group_capability_v1,
)


class Spec170GroupCapabilityTest(unittest.TestCase):
    def test_provider_projection_discloses_only_local_wrapped_epoch_key(self):
        epoch_key = bytes(range(32))
        capability = seal_group_capability_v1(
            request_id="/request/projected",
            attempt_id="attempt-1",
            plan_digest="sha256:" + "a" * 64,
            group_id="group-1",
            epoch=1,
            ordered_members=(
                GroupMemberV1("/provider/a", 0, "offer-a", "/provider/a/data"),
                GroupMemberV1("/provider/b", 1, "offer-b", "/provider/b/data"),
            ),
            permitted_operations=(
                GroupOperationV1(
                    7, "PIPELINE_TRANSFER", ("0",), ("1",),
                    "sha256:" + "b" * 64, 4096, 1),
            ),
            max_inflight_bytes=4096,
            no_progress_ms=2000,
            hard_deadline_ms=8000,
            random_bytes=lambda size: epoch_key[:size],
            wrap_epoch_key=lambda provider, key: provider.encode() + key,
        )

        provider_a = capability.project_for_provider("/provider/a")
        provider_b = capability.project_for_provider("/provider/b")

        self.assertEqual(
            set(provider_a.wrapped_epoch_key_by_provider), {"/provider/a"})
        self.assertEqual(
            set(provider_b.wrapped_epoch_key_by_provider), {"/provider/b"})
        self.assertEqual(
            provider_a.wrapped_epoch_key_digest_by_provider,
            provider_b.wrapped_epoch_key_digest_by_provider,
        )
        self.assertEqual(
            provider_a.capability_digest, provider_b.capability_digest)
        self.assertEqual(
            provider_a.sealer_signature, provider_b.sealer_signature)
        self.assertEqual(
            provider_a.canonical_bytes(True), provider_b.canonical_bytes(True))

    def test_sealer_rejects_operation_rank_outside_group_membership(self):
        with self.assertRaisesRegex(ValueError, "operation rank"):
            seal_group_capability_v1(
                request_id="/request/wrong-rank",
                attempt_id="attempt-1",
                plan_digest="sha256:" + "a" * 64,
                group_id="group-1",
                epoch=1,
                ordered_members=(
                    GroupMemberV1(
                        "/provider/a", 0, "offer-a", "/provider/a/data"),
                    GroupMemberV1(
                        "/provider/b", 1, "offer-b", "/provider/b/data"),
                ),
                permitted_operations=(
                    GroupOperationV1(
                        7, "PIPELINE_TRANSFER", ("9",), ("1",),
                        "sha256:" + "b" * 64, 4096, 1),
                ),
                max_inflight_bytes=4096,
                no_progress_ms=2000,
                hard_deadline_ms=8000,
                random_bytes=lambda size: bytes(range(size)),
                wrap_epoch_key=lambda provider, key: provider.encode() + key,
            )

    def test_sealer_emits_cpp_compatible_digest_signature_and_wrapped_keys(self):
        epoch_key = bytes(range(32))
        wrapped_calls: list[tuple[str, bytes]] = []

        def wrap(provider: str, key: bytes) -> bytes:
            wrapped_calls.append((provider, bytes(key)))
            return ("wrapped:" + provider + ":").encode() + bytes(key)

        capability = seal_group_capability_v1(
            request_id="/request/1",
            attempt_id="attempt-1",
            plan_digest="sha256:" + "a" * 64,
            group_id="group-1",
            epoch=1,
            ordered_members=(
                GroupMemberV1("/provider/a", 0, "offer-a", "/provider/a/data"),
                GroupMemberV1("/provider/b", 1, "offer-b", "/provider/b/data"),
            ),
            permitted_operations=(
                GroupOperationV1(
                    7, "PIPELINE_TRANSFER", ("0",), ("1",),
                    "sha256:" + "b" * 64, 4096, 1),
            ),
            max_inflight_bytes=4096,
            no_progress_ms=2000,
            hard_deadline_ms=8000,
            random_bytes=lambda size: epoch_key[:size],
            wrap_epoch_key=wrap,
        )

        self.assertEqual(
            wrapped_calls,
            [("/provider/a", epoch_key), ("/provider/b", epoch_key)],
        )
        self.assertEqual(
            capability.epoch_key_id, hashlib.sha256(epoch_key).hexdigest())
        self.assertEqual(
            capability.capability_digest,
            hashlib.sha256(capability.canonical_bytes(False)).hexdigest(),
        )
        self.assertEqual(
            capability.sealer_signature,
            hmac.new(
                epoch_key, capability.canonical_bytes(True), hashlib.sha256
            ).digest(),
        )
        self.assertTrue(
            capability.to_bytes().startswith(capability.canonical_bytes(True)))
        self.assertTrue(
            capability.to_bytes().endswith(
                len(capability.sealer_signature).to_bytes(8, "big")
                + capability.sealer_signature))
        self.assertLessEqual(len(capability.to_bytes()), 16 << 20)


if __name__ == "__main__":
    unittest.main()
