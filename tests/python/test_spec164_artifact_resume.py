"""Python resume ownership is retired; native restart/reopen is tested separately."""
import unittest
from py_repoclient import ArtifactReplicaSession, AtomicArtifactDestination
from retired_repo_fixture import LegacySourceFixture


class ArtifactResumeTests(LegacySourceFixture):
    def test_resume_and_replace_flags_cannot_reactivate_python_destinations(self):
        for options in ({"resume": True}, {"resume": False}, {"replace": True}):
            with self.subTest(options=options):
                self.assert_rejected(
                    "AtomicArtifactDestination", "use ArtifactRepositoryApi.fetch_file",
                    lambda: AtomicArtifactDestination(self.root / "destination", **options))

    def test_reopened_python_session_cannot_adopt_legacy_source(self):
        self.assert_rejected(
            "ArtifactReplicaSession", "use native ArtifactRepositoryApi and ArtifactFetchResult",
            lambda: ArtifactReplicaSession(persistence=self.database,
                                           operation_id="old-operation", resume=True))


if __name__ == "__main__":
    unittest.main()
