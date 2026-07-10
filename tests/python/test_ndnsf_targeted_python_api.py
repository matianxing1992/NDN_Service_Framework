#!/usr/bin/env python3
"""Python API contract tests for known-provider Targeted invocation."""

from __future__ import annotations

from types import SimpleNamespace
import unittest

from ndnsf import ServiceUser


class _FakeNativeUser:
    def __init__(self) -> None:
        self.sync_call = None
        self.async_call = None

    def request_service_targeted(
        self,
        provider: str,
        service: str,
        payload: bytes,
        *,
        timeout_ms: int,
    ):
        self.sync_call = (provider, service, payload, timeout_ms)
        return SimpleNamespace(status=True, payload=b"targeted-ok", error="")

    def request_service_targeted_async(
        self,
        provider: str,
        service: str,
        payload: bytes,
        on_response,
        on_timeout,
        *,
        timeout_ms: int,
    ) -> None:
        self.async_call = (
            provider,
            service,
            payload,
            on_response,
            on_timeout,
            timeout_ms,
        )


def _service_user_with_fake_native() -> tuple[ServiceUser, _FakeNativeUser]:
    native = _FakeNativeUser()
    user = ServiceUser.__new__(ServiceUser)
    user._native = native
    return user, native


class TargetedPythonApiTest(unittest.TestCase):
    def test_sync_targeted_forwards_known_provider_and_converts_response(self) -> None:
        user, native = _service_user_with_fake_native()

        response = user.request_service_targeted(
            "/repo/A",
            "/Repo/ObjectStore",
            b"STORE",
            timeout_ms=2400,
        )

        self.assertEqual(
            native.sync_call,
            ("/repo/A", "/Repo/ObjectStore", b"STORE", 2400),
        )
        self.assertTrue(response.status)
        self.assertEqual(response.payload, b"targeted-ok")
        self.assertEqual(response.error, "")

    def test_async_targeted_forwards_callbacks_and_converts_response(self) -> None:
        user, native = _service_user_with_fake_native()
        responses = []
        timeouts = []

        user.request_service_targeted_async(
            "/repo/B",
            "/Repo/ObjectStore",
            b"RESERVE",
            on_response=responses.append,
            on_timeout=timeouts.append,
            timeout_ms=1800,
        )

        self.assertIsNotNone(native.async_call)
        provider, service, payload, on_response, on_timeout, timeout_ms = native.async_call
        self.assertEqual(provider, "/repo/B")
        self.assertEqual(service, "/Repo/ObjectStore")
        self.assertEqual(payload, b"RESERVE")
        self.assertEqual(timeout_ms, 1800)

        on_response(SimpleNamespace(status=True, payload=b"reserved", error=""))
        on_timeout("/request/7")
        self.assertEqual(responses[0].payload, b"reserved")
        self.assertEqual(timeouts, ["/request/7"])


if __name__ == "__main__":
    unittest.main()
