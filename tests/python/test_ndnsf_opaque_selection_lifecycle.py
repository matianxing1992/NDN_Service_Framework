from __future__ import annotations

from types import MappingProxyType
from pathlib import Path
import re
import unittest

from ndnsf.service import ServiceProvider


class _NativeProvider:
    def __init__(self):
        self.store = None
        self.registration = None

    def configure_opaque_selection_store(
        self, wal_path, storage_key, storage_key_epoch, max_prepare_ms,
    ):
        self.store = (
            wal_path, bytes(storage_key), storage_key_epoch, max_prepare_ms,
        )

    def register_opaque_selection_participant(
        self, service, participant_id, participant_version, prepare,
        on_committed, on_aborted,
    ):
        self.registration = (
            service, participant_id, participant_version, prepare,
            on_committed, on_aborted,
        )


class OpaqueSelectionLifecycleApiTest(unittest.TestCase):
    def setUp(self):
        self.provider = ServiceProvider.__new__(ServiceProvider)
        self.provider._native = _NativeProvider()

    def test_store_requires_exact_secret_and_positive_bound(self):
        with self.assertRaises(ValueError):
            self.provider.configure_opaque_selection_store(
                wal_path="/tmp/opaque.wal",
                storage_key=b"short",
                storage_key_epoch="epoch-1",
            )
        self.provider.configure_opaque_selection_store(
            wal_path="/tmp/opaque.wal",
            storage_key=b"k" * 32,
            storage_key_epoch="epoch-1",
            max_prepare_ms=25,
        )
        self.assertEqual(
            self.provider._native.store,
            ("/tmp/opaque.wal", b"k" * 32, "epoch-1", 25),
        )

    def test_non_di_participant_receives_immutable_context_and_opaque_bytes(self):
        committed = []
        aborted = []

        def prepare(context, payload):
            self.assertIsInstance(context, MappingProxyType)
            with self.assertRaises(TypeError):
                context["attempt"] = 9
            self.assertEqual(payload, b"opaque-work-order")
            return {
                "commit_blob": b"generic-side-effect-plan",
                "acceptance_payload": b"accepted",
            }

        self.provider.register_opaque_selection_participant(
            "/generic/image-transform",
            participant_id="image-transform-policy",
            participant_version=3,
            prepare=prepare,
            on_committed=committed.append,
            on_aborted=lambda transaction_id, reason: aborted.append(
                (transaction_id, reason)
            ),
        )
        registration = self.provider._native.registration
        self.assertEqual(registration[:3], (
            "/generic/image-transform", "image-transform-policy", 3,
        ))
        native_result = registration[3](
            {"attempt": 1, "selection_payload_digest": "sha256:test"},
            b"opaque-work-order",
        )
        self.assertEqual(native_result, {
            "commit_blob": b"generic-side-effect-plan",
            "acceptance_payload": b"accepted",
        })
        registration[4]({"transaction_id": "txn-1"})
        self.assertIsInstance(committed[0], MappingProxyType)
        registration[5]("txn-2", "PREPARE_REJECTED")
        self.assertEqual(aborted, [("txn-2", "PREPARE_REJECTED")])

    def test_prepare_result_schema_is_closed(self):
        self.provider.register_opaque_selection_participant(
            "/generic/work",
            participant_id="generic-policy",
            participant_version=1,
            prepare=lambda _context, _payload: {
                "commit_blob": b"a",
                "acceptance_payload": b"b",
                "model_specific_field": b"forbidden",
            },
            on_committed=lambda _view: None,
            on_aborted=lambda _transaction_id, _reason: None,
        )
        with self.assertRaises(ValueError):
            self.provider._native.registration[3]({}, b"x")

    def test_new_core_transaction_surface_has_no_di_semantic_dependency(self):
        root = Path(__file__).resolve().parents[2]
        core = (
            (root / "ndn-service-framework/GenericSelectionTxnStore.hpp")
            .read_text()
            + (root / "ndn-service-framework/GenericSelectionTxnStore.cpp")
            .read_text()
        )
        binding = (
            root / "pythonWrapper/src/ndnsf/_ndnsf.cpp"
        ).read_text()
        binding = binding[
            binding.index("class PyOpaqueSelectionParticipant"):
            binding.index("class NativeServiceProvider")
        ]
        forbidden = re.compile(
            r"\b(model|split|gpu|fragment|artifact|preparation|tensor|dag|"
            r"di[_-]?recovery)\b",
            re.IGNORECASE,
        )
        self.assertEqual(forbidden.findall(core), [])
        self.assertEqual(forbidden.findall(binding), [])


if __name__ == "__main__":
    unittest.main()
