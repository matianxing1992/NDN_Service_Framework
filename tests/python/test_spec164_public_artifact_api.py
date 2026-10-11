import unittest
import tempfile

from py_repoclient import ArtifactRepositoryApi, RepoCompatibilityError


class RetiredPythonArtifactApiOptionsTest(unittest.TestCase):
    def make_local_api(self):
        store = tempfile.TemporaryDirectory(prefix="spec192-api-options-")
        self.addCleanup(store.cleanup)
        return ArtifactRepositoryApi.make_in_app(
            durable_root=store.name,
            owner_id="/spec164/test-owner",
        )

    def test_in_app_factory_requires_native_owner_identity(self):
        with tempfile.TemporaryDirectory(prefix="spec192-api-owner-") as store:
            with self.assertRaisesRegex(TypeError, "owner_id"):
                ArtifactRepositoryApi.make_in_app(durable_root=store)

    def assert_retired_call(self, operation, callback):
        with self.assertRaises(RepoCompatibilityError) as caught:
            callback()
        self.assertEqual(caught.exception.code, "SPEC192_COMPATIBILITY_V1")
        self.assertEqual(caught.exception.operation, operation)
        self.assertTrue(caught.exception.replacement)

    def test_backend_sessions_async_and_python_callbacks_are_rejected(self):
        api = self.make_local_api()
        reference = object()
        self.assert_retired_call(
            "ArtifactRepositoryApi(backend=...)",
            lambda: ArtifactRepositoryApi(object(), publisher_identity="/app"),
        )
        self.assert_retired_call("configure_artifact_backend", api.configure_artifact_backend)
        self.assert_retired_call("begin_upload", api.begin_upload)
        self.assert_retired_call("begin_fetch", api.begin_fetch)
        self.assert_retired_call("publish_file_async", api.publish_file_async)
        self.assert_retired_call("fetch_file_async", api.fetch_file_async)
        self.assert_retired_call(
            "publish_file legacy options",
            lambda: api.publish_file("source.bin", name="/app/model"),
        )
        self.assert_retired_call(
            "fetch_file legacy options",
            lambda: api.fetch_file(reference, "destination.bin", on_progress=lambda _: None),
        )


if __name__ == "__main__":
    unittest.main()
