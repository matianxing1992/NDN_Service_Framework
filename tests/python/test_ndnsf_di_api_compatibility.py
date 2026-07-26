from __future__ import annotations

from types import SimpleNamespace
import unittest
import warnings
from unittest.mock import Mock

from ndnsf_distributed_inference.compatibility import (
    LegacyClientAdapter, LegacyProviderAdapter, LegacyProviderLifecycleAdapter,
)


class ApiCompatibilityTest(unittest.TestCase):
    def test_client_adapter_warns_once_and_delegates_without_rewriting_result(self):
        value = object()
        client = SimpleNamespace(
            request=Mock(return_value=value), run=Mock(return_value=value),
            run_async=Mock())
        adapter = LegacyClientAdapter(client)
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            self.assertIs(adapter.submit("d", input=b"x", timeout="t"), value)
            self.assertIs(adapter.submit("d", input=b"x", timeout="t"), value)
        self.assertEqual(len(caught), 1)
        client.request.assert_called_with(
            "d", input=b"x", timeout="t", deadline=None, options=None)

    def test_provider_adapter_does_not_expose_admin(self):
        provider = SimpleNamespace(serve=Mock(return_value="registration"))
        adapter = LegacyProviderAdapter(provider)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            self.assertEqual(adapter.serve_service(
                "/s", object(), capabilities={}), "registration")
        self.assertFalse(hasattr(adapter, "stage"))

    def test_lifecycle_compatibility_requires_explicit_admin_port(self):
        admin = SimpleNamespace(
            stage=Mock(return_value="staged"),
            activate=Mock(return_value="active"),
            drain=Mock(return_value="drained"),
            delete=Mock(return_value="deleted"))
        adapter = LegacyProviderLifecycleAdapter(admin)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            self.assertEqual(adapter.stage("r", "t", ()), "staged")
            self.assertEqual(adapter.activate("evidence"), "active")
        admin.stage.assert_called_once_with("r", "t", ())


if __name__ == "__main__":
    unittest.main()
