"""spec181 T004 unit tests: controller/provider readiness boundary.

FR-011: the Python start()/start_background() readiness wait must exceed the
Core 10 s probe deadline (15000 ms configured); a stop()/cancellation must
never spin the event loop hot.  These tests pin the Python seam against a
fake native handle (no NFD, no process boundary); the native probe-loop
cancellation behavior is asserted on the real ServiceController in the C++
unit layer and the MiniNDN qualification gate.
"""

from __future__ import annotations

import threading
import unittest
from unittest import mock

from ndnsf import _ndnsf


class FakeNativeProvider:
    def __init__(self, *args, **kwargs):
        self.started = False
        self.stopped = False
        self.ready_result = True
        self.ready_timeout_ms = None
        self.ready_raise = None

    def add_service(self, *args, **kwargs):
        return None

    def add_collaboration_service(self, *args, **kwargs):
        return None

    def start(self):
        self.started = True

    def stop(self):
        self.stopped = True

    def wait_until_ready(self, timeout_ms):
        self.ready_timeout_ms = timeout_ms
        if self.ready_raise is not None:
            raise self.ready_raise
        return self.ready_result

    def run(self):
        self.started = True


class ServiceProviderReadinessTest(unittest.TestCase):
    def _provider(self):
        from ndnsf.service import ServiceProvider
        with mock.patch.object(
                _ndnsf, "NativeServiceProvider", FakeNativeProvider):
            provider = ServiceProvider(
                group="/example/hello/group",
                controller="/example/hello/controller",
                provider_prefix="/example/hello/provider",
                trust_schema="examples/trust-schema.conf",
            )
            provider.add_handler("/svc/1", lambda payload: b"ok")
            return provider

    def test_start_waits_15000ms_and_returns_when_ready(self):
        provider = self._provider()
        provider.start("/svc/1")
        native = provider._native
        self.assertTrue(native.started)
        self.assertEqual(native.ready_timeout_ms, 15000)

    def test_start_stops_and_raises_on_readiness_timeout(self):
        provider = self._provider()
        provider._native.ready_result = False
        with self.assertRaises(RuntimeError):
            provider.start("/svc/1")
        self.assertTrue(provider._native.stopped)

    def test_start_stops_and_reraises_native_failure(self):
        provider = self._provider()
        provider._native.ready_raise = RuntimeError("face died")
        with self.assertRaises(RuntimeError):
            provider.start("/svc/1")
        self.assertTrue(provider._native.stopped)

    def test_start_background_waits_and_returns_thread(self):
        provider = self._provider()
        thread = provider.start_background("/svc/1")
        self.assertIsInstance(thread, threading.Thread)
        self.assertEqual(provider._native.ready_timeout_ms, 15000)

    def test_start_background_stops_on_timeout(self):
        provider = self._provider()
        provider._native.ready_result = False
        with self.assertRaises(RuntimeError):
            provider.start_background("/svc/1")
        self.assertTrue(provider._native.stopped)


class ServiceControllerReadinessTest(unittest.TestCase):
    """The ServiceController seam keeps the 15000 ms bound (FR-011)."""

    def _controller(self, native_cls=None):
        from ndnsf.service import ServiceController
        native = native_cls() if native_cls is not None else FakeNativeProvider()
        with mock.patch.object(
                _ndnsf, "NativeServiceController",
                mock.Mock(return_value=native)):
            controller = ServiceController(
                controller_prefix="/example/hello/controller")
            return controller, native

    def test_start_uses_15000ms_readiness_wait(self):
        controller, native = self._controller()
        controller.start()
        self.assertEqual(native.ready_timeout_ms, 15000)

    def test_timeout_stops_and_raises(self):
        native = FakeNativeProvider()
        native.ready_result = False
        controller, native = self._controller(
            lambda: native)
        with self.assertRaises(RuntimeError):
            controller.start()
        self.assertTrue(native.stopped)


if __name__ == "__main__":
    unittest.main()
