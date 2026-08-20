from __future__ import annotations

from types import SimpleNamespace
import json
import unittest
from unittest.mock import Mock

from ndnsf_distributed_inference.api import InferenceProvider
from ndnsf_distributed_inference.app_sdk.provider import (
    APPProvider, ProviderAdminPort,
)
from ndnsf_distributed_inference.artifact_deployment import (
    ExecutionArtifactSpec, ExecutionContext,
)
from ndnsf_distributed_inference.provider import DistributedInferenceProvider


class ProviderSurfaceTest(unittest.TestCase):
    def test_serving_surface_has_no_lifecycle_authority(self):
        network = SimpleNamespace(
            serve_service=Mock(return_value="registration"),
            run=Mock(return_value=None), stop=Mock(return_value=None))
        provider = InferenceProvider(network)
        registration = provider.serve(
            "/LLM/Test", lambda payload: payload,
            capabilities={"roles": ("prefill",), "backends": ("onnx",)})
        self.assertEqual(registration, "registration")
        self.assertFalse(hasattr(provider, "stage"))
        self.assertFalse(hasattr(provider, "activate"))
        self.assertFalse(hasattr(provider, "admin"))
        network.serve_service.assert_called_once()

    def test_admin_port_requires_separate_credential(self):
        provider = APPProvider("/provider/a")
        with self.assertRaises(PermissionError):
            ProviderAdminPort(provider, credential_id="")
        admin = ProviderAdminPort(provider, credential_id="coordinator-key")
        self.assertEqual(admin.credential_id, "coordinator-key")

    def test_provider_reports_observational_progress_without_admin_authority(self):
        reported = []
        ctx = SimpleNamespace(
            assignment=SimpleNamespace(role="prefill", service="/LLM/Test"),
            local_provider="/provider/a", session_id="request-1",
            report_operation_status=reported.append)
        execution = ExecutionContext(
            spec=ExecutionArtifactSpec(
                role="prefill", backend="onnxruntime", entrypoint="",
                artifacts=[], metadata={"planDigest": "sha256:" + "1" * 64}),
            artifact_paths={}, work_dir=__import__("pathlib").Path("/tmp"))
        DistributedInferenceProvider._report_preparation(
            ctx, phase="READY", sequence=6, progress=1.0,
            execution=execution)
        self.assertEqual(len(reported), 1)
        self.assertEqual(reported[0].state.value, "DONE")
        self.assertEqual(reported[0].role, "prefill")
        self.assertEqual(reported[0].details_schema,
                         "ndnsf-di-preparation-progress-v1")
        self.assertNotIn("certificate", reported[0].details_payload.decode())

    @staticmethod
    def _execution():
        return ExecutionContext(
            spec=ExecutionArtifactSpec(
                role="prefill", backend="onnxruntime", entrypoint="",
                artifacts=[], metadata={
                    "planDigest": "sha256:" + "1" * 64,
                    "deploymentRevision": "sha256:" + "2" * 64,
                }),
            artifact_paths={}, work_dir=__import__("pathlib").Path("/tmp"))

    def test_all_role_readiness_barrier_precedes_runner(self):
        execution = self._execution()
        selection = "sha256:" + "3" * 64
        peer_value = {
            "schema": "ndnsf-di-readiness-v1",
            "revision": "sha256:" + "2" * 64,
            "planDigest": "sha256:" + "1" * 64,
            "bindingDigest": selection, "memberId": "decode",
            "role": "decode", "provider": "/provider/b",
            "adapter": "onnxruntime", "artifactDigests": [],
        }
        published = []
        ctx = SimpleNamespace(
            assignment=SimpleNamespace(
                role="prefill", service="/LLM/Test",
                selection_digest=selection,
                assignment_payload=b"executionPolicy=LEGACY_READY_SET_V1;",
                role_providers={
                    "prefill": "/provider/a", "decode": "/provider/b"}),
            local_provider="/provider/a",
            publish=lambda scope, topic, payload: published.append(
                (scope, topic, payload)),
            wait_for=lambda *args: [SimpleNamespace(
                producer_role="decode", producer="/provider/b",
                payload=json.dumps(
                    peer_value, sort_keys=True,
                    separators=(",", ":")).encode())],
        )
        DistributedInferenceProvider._await_all_role_readiness(
            ctx, execution, timeout_ms=20)
        self.assertEqual(published[0][0], "ndnsf-di-readiness-v1")

    def test_all_role_readiness_barrier_rejects_cross_role_sender(self):
        execution = self._execution()
        selection = "sha256:" + "3" * 64
        value = {
            "schema": "ndnsf-di-readiness-v1",
            "revision": "sha256:" + "2" * 64,
            "planDigest": "sha256:" + "1" * 64,
            "bindingDigest": selection, "memberId": "decode",
            "role": "decode", "provider": "/provider/b",
            "adapter": "onnxruntime", "artifactDigests": [],
        }
        ctx = SimpleNamespace(
            assignment=SimpleNamespace(
                role="prefill", service="/LLM/Test",
                selection_digest=selection,
                assignment_payload=b"executionPolicy=LEGACY_READY_SET_V1;",
                role_providers={
                    "prefill": "/provider/a", "decode": "/provider/b"}),
            local_provider="/provider/a", publish=lambda *args: None,
            wait_for=lambda *args: [SimpleNamespace(
                producer_role="decode", producer="/attacker",
                payload=json.dumps(
                    value, sort_keys=True,
                    separators=(",", ":")).encode())],
        )
        with self.assertRaisesRegex(RuntimeError, "DI_READINESS_BINDING_MISMATCH"):
            DistributedInferenceProvider._await_all_role_readiness(
                ctx, execution, timeout_ms=20)


if __name__ == "__main__":
    unittest.main()
