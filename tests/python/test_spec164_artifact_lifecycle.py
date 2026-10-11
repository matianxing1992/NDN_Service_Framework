"""Retired Python lifecycle owners reject before source or destination mutation."""
import unittest
from py_repoclient import (
    ArtifactReplicaSession, AtomicArtifactDestination, AuthenticatedReplicaReceipt,
    HmacReceiptAuthenticator, resolve_active_artifact,
)
from retired_repo_fixture import LegacySourceFixture


class ArtifactLifecycleTests(LegacySourceFixture):
    def test_python_sessions_and_receipts_cannot_claim_native_state(self):
        for name, replacement, call in (
            ("ArtifactReplicaSession", "use native ArtifactRepositoryApi and ArtifactFetchResult",
             lambda: ArtifactReplicaSession(persistence=self.database, operation_id="retired")),
            ("AuthenticatedReplicaReceipt", "use native ArtifactCommitReceipt",
             lambda: AuthenticatedReplicaReceipt(receipt_id="retired")),
            ("HmacReceiptAuthenticator", "use controller-authenticated native receipts",
             lambda: HmacReceiptAuthenticator("/repo", "/repo/KEY/test", b"M" * 32)),
        ):
            with self.subTest(name=name):
                self.assert_rejected(name, replacement, call)

    def test_python_destination_and_active_lookup_cannot_expose_payload(self):
        self.assert_rejected(
            "AtomicArtifactDestination", "use ArtifactRepositoryApi.fetch_file",
            lambda: AtomicArtifactDestination(self.root / "destination"))
        self.assert_rejected(
            "resolve_active_artifact", "use native ArtifactHandle and check_availability",
            lambda: resolve_active_artifact(self.database, "/legacy/artifact"))


if __name__ == "__main__":
    unittest.main()
