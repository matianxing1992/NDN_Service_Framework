import tempfile
import unittest

from Experiments.ndnsf_validation.lineage import InvocationIdentity, LineageLedger
from ndnsf_distributed_inference.app_sdk.client import APPClient
from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal


class LineageTests(unittest.TestCase):
    def setUp(self):
        self.identity = InvocationIdentity(
            request_id="request-1",
            attempt=1,
            plan_id="plan-1",
            selection_digest="sha256:selection",
            model_identity_digest="sha256:model",
        )
        self.ledger = LineageLedger(self.identity)

    def changed(self, **updates):
        values = self.identity.__dict__.copy()
        values.update(updates)
        return InvocationIdentity(**values)

    def test_happy_path_and_terminal(self):
        for kind in ("REQUEST", "ACK", "SELECTION", "INTERMEDIATE"):
            self.assertTrue(
                self.ledger.append(
                    event_type=kind,
                    identity=self.identity,
                    authenticated=True,
                )["admitted"]
            )
        self.assertTrue(
            self.ledger.append(
                event_type="RESPONSE",
                identity=self.identity,
                authenticated=True,
                terminal=True,
            )["admitted"]
        )
        self.assertFalse(
            self.ledger.append(
                event_type="RESPONSE",
                identity=self.identity,
                authenticated=True,
                terminal=True,
            )["admitted"]
        )

    def test_each_identity_mismatch_is_rejected(self):
        changes = {
            "request_id": "other",
            "attempt": 2,
            "plan_id": "other",
            "selection_digest": "sha256:other",
            "model_identity_digest": "sha256:other",
        }
        for field, value in changes.items():
            with self.subTest(field=field):
                event = self.ledger.append(
                    event_type="ACK",
                    identity=self.changed(**{field: value}),
                    authenticated=True,
                )
                self.assertFalse(event["admitted"])

    def test_operation_duplicate_reorder_and_forgery_rejected(self):
        first = self.ledger.append(
            event_type="STATUS",
            identity=self.identity,
            authenticated=True,
            operation_id="prepare-1",
            epoch=1,
            sequence=2,
        )
        self.assertTrue(first["admitted"])
        for sequence in (2, 1):
            event = self.ledger.append(
                event_type="STATUS",
                identity=self.identity,
                authenticated=True,
                operation_id="prepare-1",
                epoch=1,
                sequence=sequence,
            )
            self.assertFalse(event["admitted"])
        forged = self.ledger.append(
            event_type="STATUS",
            identity=self.identity,
            authenticated=False,
            operation_id="prepare-2",
            epoch=1,
            sequence=1,
        )
        self.assertFalse(forged["admitted"])

    def test_application_owned_request_id_is_durable_and_unique(self):
        with tempfile.TemporaryDirectory() as root:
            client = APPClient(
                RuntimeJournal.for_test(root, "spec165-user"),
                executor=lambda payload: payload,
            )
            handle = client.submit(
                "local-deployment",
                "revision-1",
                b"payload",
                request_id="campaign-prompt-1-token-0",
            )
            self.assertEqual(
                handle.request_id, "campaign-prompt-1-token-0")
            with self.assertRaisesRegex(ValueError, "already exists"):
                client.submit(
                    "local-deployment",
                    "revision-1",
                    b"payload",
                    request_id="campaign-prompt-1-token-0",
                )


if __name__ == "__main__":
    unittest.main()
