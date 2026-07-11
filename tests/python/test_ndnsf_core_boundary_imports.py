from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import tempfile
import unittest

import ndnsf
from ndnsf_distributed_inference import (
    ExecutionArtifact,
    ExecutionArtifactSpec,
    prepare_execution,
    publish_execution_artifact_spec,
)
from ndnsf_distributed_inference.retry import RetryPolicy, retry_call
from py_repoclient import RepoDataPlaneProducer


@dataclass
class PublishResult:
    success: bool
    encrypted_data_name: str = ""
    error: str = ""


class FakeUser:
    def __init__(self) -> None:
        self.payloads: list[bytes] = []

    def publish_encrypted_large_data(self, _service, payload, **_kwargs):
        self.payloads.append(bytes(payload))
        return PublishResult(True, f"/data/{len(self.payloads)}")


class FakeAssignment:
    assigned_artifact = "/spec"
    provisioning_timeout_ms = 1000
    service = "/Inference/NativeTracer"


class FakeContext:
    assignment = FakeAssignment()

    def __init__(self, spec: bytes, artifacts: dict[str, bytes]) -> None:
        self.spec = spec
        self.artifacts = artifacts

    def fetch_artifact(self, _name, _timeout):
        return True

    def get_artifact(self, _name):
        return self.spec

    def fetch_encrypted_large_data(self, name, _service):
        return self.artifacts.get(name)


class CoreBoundaryImportsTest(unittest.TestCase):
    def test_native_tracer_launcher_includes_repo_wrapper(self) -> None:
        launcher = (
            Path(__file__).resolve().parents[2]
            / "Experiments"
            / "NDNSF_DI_NativeTracer_Minindn.py"
        ).read_text(encoding="utf-8")
        self.assertGreaterEqual(
            launcher.count('REPO / "NDNSF-DistributedRepo" / "pythonWrapper"'),
            2,
        )

    def test_generic_package_does_not_export_application_owned_symbols(self) -> None:
        for name in (
            "ExecutionArtifact",
            "ExecutionArtifactSpec",
            "ExecutionContext",
            "RepoDataPlaneProducer",
            "RetryPolicy",
            "retry_call",
        ):
            self.assertFalse(hasattr(ndnsf, name), name)
        self.assertEqual(RepoDataPlaneProducer.__module__, "py_repoclient")
        self.assertTrue(callable(retry_call))
        self.assertIsNotNone(RetryPolicy)

    def test_execution_artifact_wire_format_is_stable_after_move(self) -> None:
        artifact = ExecutionArtifact(
            name="model",
            data_name="/data/model",
            filename="model.onnx",
            sha256="abc",
            kind="model",
            chunks=["/data/part0"],
            executable=False,
            cache_name="qwen-stage-0",
            repo_manifest={"objectName": "/model/qwen"},
            large_data_reference={"source": "repo-manifest"},
        )
        spec = ExecutionArtifactSpec(
            role="/Stage/0",
            backend="onnxruntime",
            entrypoint="model.onnx",
            artifacts=[artifact],
            metadata={"stage": 0},
        )
        wire = spec.to_bytes()
        self.assertEqual(ExecutionArtifactSpec.from_bytes(wire), spec)
        self.assertEqual(ExecutionArtifactSpec.from_bytes(wire).to_bytes(), wire)

    def test_publish_and_materialize_use_only_generic_large_data_primitives(self) -> None:
        user = FakeUser()
        result = publish_execution_artifact_spec(
            user,
            "/Inference/NativeTracer",
            role="/Stage/0",
            backend="onnxruntime",
            artifacts={"model": (b"model-bytes", "model.onnx", "model")},
        )
        self.assertTrue(result.success)
        spec = ExecutionArtifactSpec.from_bytes(user.payloads[-1])
        context = FakeContext(spec.to_bytes(), {"/data/1": b"model-bytes"})
        with tempfile.TemporaryDirectory() as tmp:
            execution = prepare_execution(context, temp_root=tmp)
            self.assertEqual(execution.path("model").read_bytes(), b"model-bytes")
            self.assertTrue(str(execution.path("model")).startswith(str(Path(tmp))))

    def test_materialization_rejects_unsafe_filename(self) -> None:
        artifact = ExecutionArtifact(
            "model",
            "/data/model",
            "../model.onnx",
            hashlib.sha256(b"model-bytes").hexdigest(),
        )
        spec = ExecutionArtifactSpec("/Stage/0", "onnxruntime", artifacts=[artifact])
        context = FakeContext(spec.to_bytes(), {"/data/model": b"model-bytes"})
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError, "unsafe artifact filename"):
                prepare_execution(context, temp_root=tmp)


if __name__ == "__main__":
    unittest.main()
