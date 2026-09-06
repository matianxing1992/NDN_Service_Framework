"""Exercise the registered Python handler, including its compatibility boundary."""

import json
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from ndnsf_distributed_inference.artifact_deployment import (
    ExecutionArtifactSpec, ExecutionContext,
)
from ndnsf_distributed_inference.provider import DistributedInferenceProvider
from ndnsf_distributed_inference.sdk.placement import (
    DeviceBinding, DeviceBindingMode, ExecutionRole,
    ProviderSelectionProjectionV3, RoleAssemblySpec, RoleDataflowContract,
)


def projection(attempt=2):
    digest = "sha256:" + "a" * 64
    role = RoleAssemblySpec(
        "stage0", 0, 0, 2, digest, digest, "onnxruntime-cpu",
        adapter_id="onnx", adapter_version="1")
    return ProviderSelectionProjectionV3(
        provider="/provider/a", request_id="request-2", attempt=attempt,
        plan_core_digest=digest, plan_digest=digest, ack_closed_digest=digest,
        offer_digest=digest, security_policy_snapshot_digest=digest,
        roles=(role,), dependencies=(), deadline_ms=1000,
        execution_role=ExecutionRole(
            "stage0", "stage0", 0, 0, 2, "onnxruntime-cpu",
            adapter_id="onnx", adapter_version="1"),
        assembly=role,
        dataflow=RoleDataflowContract("request-2", attempt, digest, "stage0",
                                      terminal_response_owner=True),
        device_binding=DeviceBinding(
            DeviceBindingMode.CPU, "/provider/a", "stage0", digest, digest,
            digest, 1),
    )


@pytest.fixture
def registered_handler(tmp_path, monkeypatch):
    def register(*, v3=True, fail_preparation=False):
        native = Mock()
        runtime = DistributedInferenceProvider(native)
        execution = ExecutionContext(
            spec=ExecutionArtifactSpec("stage0", "onnxruntime-cpu", "", [], {}),
            artifact_paths={}, work_dir=tmp_path)
        prepare = Mock(return_value=execution)
        monkeypatch.setattr(runtime, "_local_execution", prepare)
        run = Mock()
        monkeypatch.setattr(runtime, "_run_handler", run)
        prefetcher = Mock()
        monkeypatch.setattr(
            "ndnsf_distributed_inference.provider.DependencyPrefetcher",
            Mock(return_value=prefetcher))
        if fail_preparation:
            prepare.side_effect = RuntimeError("fixture preparation failed")
        runtime.add_capability_handler(
            "/Test", ["stage0"], lambda _ctx: None,
            backends=("onnxruntime-cpu",), has_model=True,
            local_artifacts={"stage0": {"path": str(tmp_path / "model.onnx")}},
            selection_offer_issuer_v3=object() if v3 else None)
        wrapped = native.add_collaboration_handler.call_args.args[2]
        ctx = SimpleNamespace(
            assignment=SimpleNamespace(
                role="stage0", service="/Test", assigned_artifact="/artifact/model",
                assignment_payload=projection().to_bytes()),
            local_provider="/provider/a", session_id="request-2",
            is_streamed=False, fail=Mock(), report_operation_status=Mock())
        return wrapped, ctx, prepare, run
    return register


@pytest.mark.parametrize("fail_preparation", [False, True])
def test_v3_preparation_preserves_attempt_in_every_phase(
        registered_handler, fail_preparation):
    wrapped, ctx, prepare, run = registered_handler(fail_preparation=fail_preparation)
    wrapped(ctx, b"request")
    statuses = [call.args[0] for call in ctx.report_operation_status.call_args_list]
    assert len(statuses) >= 2
    assert {status.attempt for status in statuses} == {2}
    # This is a per-Selection operation epoch, not the ACK attempt or boot ID.
    assert {status.epoch for status in statuses} == {1}
    assert [status.sequence for status in statuses] == list(range(1, len(statuses) + 1))
    assert statuses[-1].state.value == ("FAILED" if fail_preparation else "DONE")
    prepare.assert_called_once()
    assert run.call_count == (0 if fail_preparation else 1)


@pytest.mark.parametrize("payload", [
    b"", b"legacy=value;", b"{", b"[]", b'{"schema":"unknown"}',
    b'{"schema":"ndnsf-di-selection-v3","schema_version":3}',
])
def test_v3_provider_rejects_invalid_selection_before_preparation(
        registered_handler, payload):
    wrapped, ctx, prepare, run = registered_handler()
    ctx.assignment.assignment_payload = payload
    wrapped(ctx, b"request")
    ctx.fail.assert_called_once()
    prepare.assert_not_called()
    run.assert_not_called()
    ctx.report_operation_status.assert_not_called()


@pytest.mark.parametrize("v3", [False, True])
@pytest.mark.parametrize("mutation", ["attempt", "role", "encoding", "size"])
def test_declared_v3_cannot_downgrade_after_validation_failure(
        registered_handler, v3, mutation):
    wrapped, ctx, prepare, run = registered_handler(v3=v3)
    value = json.loads(projection().to_bytes())
    if mutation == "attempt":
        value["attempt"] = 0
    elif mutation == "role":
        value["roles"][0]["unknown_role_field"] = True
    payload = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    if mutation == "encoding":
        payload += b" "
    elif mutation == "size":
        payload += b" " * (1024 * 1024)
    ctx.assignment.assignment_payload = payload
    wrapped(ctx, b"request")
    ctx.fail.assert_called_once()
    prepare.assert_not_called()
    run.assert_not_called()
    ctx.report_operation_status.assert_not_called()


@pytest.mark.parametrize("payload", [
    b"deploymentRevision=legacy;", b'{"schema":"legacy-fixture"}',
])
def test_legacy_preparation_keeps_its_initial_attempt(registered_handler, payload):
    wrapped, ctx, prepare, run = registered_handler(v3=False)
    ctx.assignment.assignment_payload = payload
    wrapped(ctx, b"request")
    ctx.fail.assert_not_called()
    prepare.assert_called_once()
    run.assert_called_once()
    statuses = [call.args[0] for call in ctx.report_operation_status.call_args_list]
    assert {status.attempt for status in statuses} == {1}


def test_valid_v3_without_issuer_still_uses_v3_ownership(registered_handler):
    wrapped, ctx, prepare, run = registered_handler(v3=False)
    wrapped(ctx, b"request")
    ctx.fail.assert_not_called()
    prepare.assert_called_once()
    context = run.call_args.args[1]
    assert context.enforce_dataflow_ownership
    assert context.terminal_response_owner
    statuses = [call.args[0] for call in ctx.report_operation_status.call_args_list]
    assert {status.attempt for status in statuses} == {2}
