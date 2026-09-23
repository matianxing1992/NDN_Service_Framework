"""Wrapper-only checks for the Spec185 native prepared-model surface.

Native protocol behavior is qualified by the C++ selectors.  These checks
cover Python naming, seconds-unit conversion boundaries, callback marshalling,
and cancellation of a Python waiter without implementing native behavior in
Python.
"""

from __future__ import annotations

import asyncio
import os
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINDINGS = ROOT / "pythonWrapper/src/ndnsf/di_bindings.cpp"
API = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/api/__init__.py"
ASYNC = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/api/_async.py"
PROVIDER_API = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/provider_api.py"
CLIENT = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py"


class _FakeSubscription:
    def __init__(self):
        self.unsubscribed = False

    def unsubscribe(self):
        self.unsubscribed = True


class _FakeHandle:
    def __init__(self):
        self.callback = None
        self.subscription = _FakeSubscription()

    def result_async(self, timeout_s, callback):
        self.timeout_s = timeout_s
        self.callback = callback
        return self.subscription

    def on_completion(self, callback):
        self.completion_callback = callback
        return self.subscription


class _FakeUser:
    def __init__(self, handle):
        self.handle = handle

    def start_prepare(self, model_key, options, timeout_s=None):
        self.model_key = model_key
        self.options = options
        self.timeout_s = timeout_s
        return self.handle


class Spec185PreparedModelWrapperTests(unittest.TestCase):
    def test_binding_exports_all_core_objects_and_releases_gil_for_blocking_calls(self):
        source = BINDINGS.read_text(encoding="utf-8")
        for symbol in (
            '"Runtime"', '"User"', '"PreparedModel"', '"PreparationHandle"',
            '"RequestHandle"', '"Conversation"', '"EventReader"', '"Subscription"',
            '"RuntimeConfig"', '"PrepareOptions"', '"RequestOptions"', '"Input"',
            '"ModelManifest"', '"PreparationReceipt"', '"ModelCapabilities"',
            '"NativeProviderConfig"', '"Provider"', '"ServiceDefinition"', '"ProviderRegistration"',
            '"DiError"',
        ):
            self.assertIn(symbol, source)
        self.assertIn("pybind11/stl/filesystem.h", source)
        self.assertIn('"__enter__"', source)
        self.assertIn('"__aenter__"', source)
        self.assertIn('"events_async"', source)
        self.assertGreaterEqual(source.count("py::gil_scoped_release"), 8)
        self.assertIn("py::gil_scoped_acquire", source)
        self.assertIn("timeout.is_none() ? reader.remainingTimeout()", source)
        self.assertIn("py::kw_only()", source)
        self.assertIn("register_local_exception_translator", source)
        self.assertIn("PyImport_AddModule", source)
        self.assertNotIn("g_diErrorType", source)

    def test_binding_exposes_user_owned_request_surface(self):
        source = BINDINGS.read_text(encoding="utf-8")
        self.assertIn(
            '.def("request", [] (const di::User& user, const di::PreparedModel& model,',
            source)
        self.assertIn(
            '.def("run", [] (const di::User& user, const di::PreparedModel& model,',
            source)
        self.assertIn(
            '.def("open_conversation", [] (const di::User& user,',
            source)
        self.assertIn('py::arg("model")', source)

    def test_api_exports_direct_native_views_without_python_planner_fallback(self):
        api = API.read_text(encoding="utf-8")
        async_api = ASYNC.read_text(encoding="utf-8")
        provider_api = PROVIDER_API.read_text(encoding="utf-8")
        client = CLIENT.read_text(encoding="utf-8")
        for name in ("Runtime", "PreparedModel", "PreparationHandle", "RequestHandle",
                     "Conversation", "EventReader", "Subscription", "Input", "DiError"):
            self.assertIn(f'"{name}"', api)
        for name in ("ProviderConfig", "Provider", "ServiceDefinition",
                     "ProviderRegistration"):
            self.assertIn(name, provider_api)
        self.assertIn("call_soon_threadsafe", async_api)
        self.assertIn("subscription.unsubscribe()", async_api)
        self.assertIn("events_async", async_api)
        self.assertIn("runtime_exit", async_api)
        self.assertIn('attr("request_result")', BINDINGS.read_text(encoding="utf-8"))
        self.assertIn('attr("preparation_result")', BINDINGS.read_text(encoding="utf-8"))
        self.assertIn('attr("drain_result")', BINDINGS.read_text(encoding="utf-8"))
        self.assertIn('attr("_prepare_positional")', BINDINGS.read_text(encoding="utf-8"))
        self.assertIn("refusing Python planner fallback", client)
        self.assertNotIn("asyncio.to_thread", async_api)

    def test_async_preparation_marshals_completion_and_preserves_seconds_argument(self):
        from ndnsf_distributed_inference.api._async import prepare

        async def run():
            handle = _FakeHandle()
            user = _FakeUser(handle)
            task = asyncio.create_task(
                prepare(user, "model-a", options=object(), timeout_s=1.25))
            await asyncio.sleep(0)
            self.assertEqual(user.model_key, "model-a")
            self.assertEqual(user.timeout_s, 1.25)
            handle.completion_callback(None, "prepared")
            self.assertEqual(await task, "prepared")
            self.assertFalse(handle.subscription.unsubscribed)

        asyncio.run(run())

    def test_async_wait_cancellation_unsubscribes_only_the_waiter(self):
        from ndnsf_distributed_inference.api._async import request_result

        async def run():
            handle = _FakeHandle()
            task = asyncio.create_task(request_result(handle, timeout_s=0.0))
            await asyncio.sleep(0)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertTrue(handle.subscription.unsubscribed)

        asyncio.run(run())

    def test_async_default_wait_uses_terminal_completion_not_zero_poll(self):
        from ndnsf_distributed_inference.api._async import request_result

        async def run():
            handle = _FakeHandle()
            task = asyncio.create_task(request_result(handle))
            await asyncio.sleep(0)
            self.assertTrue(hasattr(handle, "completion_callback"))
            handle.completion_callback(None, "completed")
            self.assertEqual(await task, "completed")

        asyncio.run(run())

    def test_async_error_preserves_native_identity_fields(self):
        from ndnsf_distributed_inference.api._async import NativeCallbackError

        error = NativeCallbackError({
            "code": "WAIT_TIMEOUT", "domain": "local", "boundary": "request",
            "request_id": "req-1", "attempt": 3, "message": "timed out",
        })
        self.assertEqual(error.code, "WAIT_TIMEOUT")
        self.assertEqual(error.domain, "local")
        self.assertEqual(error.boundary, "request")
        self.assertEqual(error.request_id, "req-1")
        self.assertEqual(error.attempt, 3)

    def test_async_context_shutdown_does_not_replace_body_exception(self):
        from ndnsf_distributed_inference.api._async import runtime_exit

        class Runtime:
            def __init__(self):
                self.closed = False
                self.subscription = _FakeSubscription()

            def close(self):
                self.closed = True

            def drain_async(self, timeout_s, callback):
                self.timeout_s = timeout_s
                callback(None, False)
                return self.subscription

        async def run():
            runtime = Runtime()
            self.assertFalse(await runtime_exit(
                runtime, ValueError, ValueError("body"), None, timeout_s=0.0))
            self.assertTrue(runtime.closed)

        asyncio.run(run())

    def test_native_extension_surface_is_available_when_built(self):
        try:
            from ndnsf import _ndnsf
        except ModuleNotFoundError as exc:
            self.skipTest(f"native extension unavailable: {exc}")
        if not hasattr(_ndnsf, "Runtime"):
            self.skipTest("current extension predates Spec185 direct bindings")
        for name in ("Runtime", "User", "PreparedModel", "PreparationHandle", "RequestHandle",
                     "Conversation", "EventReader", "Subscription"):
            self.assertTrue(hasattr(_ndnsf, name), name)
        self.assertTrue(hasattr(_ndnsf.User, "request"))
        self.assertTrue(hasattr(_ndnsf.User, "run"))
        self.assertTrue(hasattr(_ndnsf.User, "open_conversation"))

    def test_native_error_translator_survives_module_teardown(self):
        try:
            from ndnsf import _ndnsf
        except ModuleNotFoundError as exc:
            self.skipTest(f"native extension unavailable: {exc}")
        if not hasattr(_ndnsf, "NativeRequestCatalog"):
            self.skipTest("current extension predates Spec185 catalog binding")
        script = """
from ndnsf import _ndnsf
try:
    _ndnsf.Runtime.open(_ndnsf.RuntimeConfig())
except _ndnsf.DiError as error:
    assert error.code == 'INVALID_RUNTIME_CONFIGURATION'
    assert error.domain == 'local'
    assert error.boundary == 'configuration'
else:
    raise AssertionError('invalid runtime configuration did not raise DiError')
"""
        completed = subprocess.run(
            [sys.executable, "-c", script],
            env=os.environ.copy(),
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)


if __name__ == "__main__":
    unittest.main()
