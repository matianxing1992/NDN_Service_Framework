from __future__ import annotations

from ndnsf.service import CollaborationContext


def test_python_collaboration_assignment_preserves_native_role_provider_map() -> None:
    class NativeAssignment:
        role = "/D2A/Independent/0"
        service = "/Inference/Spec170LocalTwoGpu"
        assigned_artifact = ""
        artifact_data_name = ""
        requires_provisioning = False
        provisioning_timeout_ms = 0
        selection_digest = "sha256:" + "1" * 64
        assignment_payload = b'{"schema":"DI_PLACEMENT_V3"}'
        role_providers = {
            "/D2A/Independent/0": "/NDNSF-DI/Tracer/provider/local-two-gpu",
            "/D2A/LocalGroup#0": "/NDNSF-DI/Tracer/provider/local-two-gpu",
        }

    class NativeContext:
        assignment = NativeAssignment()

    assignment = CollaborationContext(NativeContext()).assignment

    assert assignment.assignment_payload.startswith(b'{"schema"')
    assert assignment.role_providers == NativeAssignment.role_providers
