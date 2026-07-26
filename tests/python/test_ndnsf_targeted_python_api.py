#!/usr/bin/env python3
"""Python API contract tests for known-provider Targeted invocation."""

from __future__ import annotations

import importlib
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest

from ndnsf import ServiceUser


REPO = Path(__file__).resolve().parents[2]


def _native_response(payload: bytes):
    return SimpleNamespace(
        status=True,
        payload=payload,
        error="",
        request_id="/request/fake",
        data_name="",
        signer_certificate="",
        wire_digest="",
    )


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
        return _native_response(b"targeted-ok")

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
    def test_compiled_binding_has_no_public_token_disable(self) -> None:
        native = importlib.import_module("ndnsf._ndnsf")
        self.assertEqual(Path(native.__file__).suffix, ".so")
        self.assertFalse(hasattr(native.NativeServiceProvider, "set_use_tokens"))
        self.assertFalse(hasattr(native.NativeServiceUser, "set_use_tokens"))
        self.assertIn("add_service", dir(native.NativeServiceProvider))
        self.assertIn("request_service_targeted", dir(native.NativeServiceUser))

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

        on_response(_native_response(b"reserved"))
        on_timeout("/request/7")
        self.assertEqual(responses[0].payload, b"reserved")
        self.assertEqual(timeouts, ["/request/7"])

    @unittest.skipUnless(
        os.environ.get("SPEC112_RUN_TARGETED_BINDING") == "1",
        "set SPEC112_RUN_TARGETED_BINDING=1 for exclusive MiniNDN evidence",
    )
    def test_real_compiled_handler_runs_exactly_once_for_normal_and_targeted(self) -> None:
        manifest_value = os.environ.get("SPEC112_CANDIDATE_MANIFEST", "")
        self.assertTrue(manifest_value, "SPEC112_CANDIDATE_MANIFEST is required")
        manifest = Path(manifest_value).resolve()
        candidate = json.loads(manifest.read_text(encoding="utf-8"))
        candidate_dir = manifest.parent

        for mode in ("normal", "targeted"):
            cell = candidate_dir / f"python-binding-{mode}"
            command = [
                sys.executable,
                str(REPO / "Experiments/NDNSF_Segmented_Response_Minindn.py"),
                "--candidate-manifest", str(manifest),
                "--output-dir", str(cell),
                "--sizes", "64",
                "--mode", mode,
                "--wall-timeout-s", "120",
            ]
            completed = subprocess.run(
                command,
                cwd=str(REPO),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=180,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout)
            summary = json.loads((cell / "cell-summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["candidateId"], candidate["candidateId"])
            self.assertEqual(summary["status"], "SUCCESS")
            self.assertEqual(summary["passed"], 1)
            provider_log = (cell / "provider.log").read_text(encoding="utf-8")
            self.assertEqual(provider_log.count("SEGMENTED_PROVIDER_HANDLER "), 1)


if __name__ == "__main__":
    unittest.main()
