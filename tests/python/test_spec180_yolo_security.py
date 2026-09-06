"""Focused Spec180 input-integrity and terminal-ownership regressions."""

from __future__ import annotations

import hashlib
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pythonWrapper"))
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf.service import CollaborationContext  # noqa: E402
from ndnsf_distributed_inference.adapters import ApplicationInput  # noqa: E402
from ndnsf_distributed_inference.app_sdk.placement import (  # noqa: E402
    AutomaticPlanningCoordinator,
    InferenceTaskRef,
    ModelRef,
    TaskOptions,
)
from ndnsf_distributed_inference.provider import (  # noqa: E402
    ProviderRuntimeContext,
)


def _digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _reference(payload: bytes, **overrides) -> dict:
    value = {
        "source": "repo-manifest",
        "dataName": "/repo/input/image-1",
        "manifestDigest": _digest(b"manifest"),
        "plaintextSize": len(payload),
        "ciphertextDigest": _digest(payload),
        "authorizationScope": "/service/object-detection/input",
        "protectionEpoch": "input-epoch-1",
        "encrypted": True,
    }
    value.update(overrides)
    return value


def _request_wire(reference: dict) -> bytes:
    task = InferenceTaskRef(
        task_name="object-detection",
        adapter_name="yolo26n",
        adapter_descriptor_digest=_digest(b"adapter"),
        adapter_composition_digest=_digest(b"composition"),
        task_descriptor_digest=_digest(b"task"),
    )
    model = ModelRef(
        model_name="YOLO26n",
        content_digest=_digest(b"model"),
        semantics_digest=_digest(b"semantics"),
        source_revision="checkpoint-r1",
    )
    value = ApplicationInput.from_repo_ref(
        task_name=task.task_name,
        input_schema_digest=_digest(b"input-schema"),
        options_schema_digest=_digest(b"options-schema"),
        reference=reference,
        options=b"{}",
    )
    return AutomaticPlanningCoordinator._encode_request(
        model,
        task,
        value,
        TaskOptions(_digest(b"options-schema"), b"{}"),
        9_999_999_999_999,
        "request-security",
        "/ObjectDetection/YOLO26n",
        "invocation-security",
    )


class _FakeNative:
    session_id = "/request-security"
    role = "BackboneNeck"
    local_provider = "/provider/backbone"

    def fetch_large(self, _data_name, _key_scope, _timeout_ms):
        return self.payload


def test_python_wrapper_reference_verification_accepts_bound_content():
    payload = b"image-payload"
    native = _FakeNative()
    native.payload = payload
    context = CollaborationContext(native)
    assert context.fetch_large_reference(
        _reference(payload), "/service/object-detection/input", 1234) == payload


@pytest.mark.parametrize(
    "overrides, expected",
    [
        ({"authorizationScope": "/service/other/input"},
         "metadata is not authenticated"),
        ({"encrypted": False}, "metadata is not authenticated"),
        ({"protectionEpoch": ""}, "metadata is not authenticated"),
        ({"protectionEpoch": "plaintext-v1"},
         "metadata is not authenticated"),
        ({"ciphertextDigest": _digest(b"different")}, "SHA-256 mismatch"),
    ],
)
def test_python_wrapper_reference_verification_rejects_tampering(
    overrides, expected,
):
    payload = b"image-payload"
    native = _FakeNative()
    native.payload = payload
    with pytest.raises(ValueError, match=expected):
        CollaborationContext(native).fetch_large_reference(
            _reference(payload, **overrides),
            "/service/object-detection/input",
            1234,
        )


def test_provider_terminal_response_is_single_use():
    class FakeCollaboration:
        session_id = "/request-security"
        responses = []

        def publish_final_response(self, payload):
            self.responses.append(bytes(payload))

    fake = FakeCollaboration()
    context = ProviderRuntimeContext(
        ndnsf=fake,
        execution=object(),
        request=b"unused",
        role="Merge",
        terminal_response_owner=True,
        enforce_dataflow_ownership=True,
    )
    context.publish_terminal_result(b"result")
    with pytest.raises(RuntimeError, match="already published"):
        context.publish_terminal_result(b"duplicate")
    assert fake.responses == [b"result"]
