"""T001 registered-handler unit tests with real grant crypto and ONNX files.

Core delivery/fetch and failure injection are controlled fixtures; these are
not network integration or MiniNDN qualification evidence.
"""

from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock
import time

import numpy as np
import onnx
import onnxruntime as ort
import pytest
from cryptography.hazmat.primitives.asymmetric import ed25519

from ndnsf_distributed_inference.core.protected_artifacts import (
    GrantRequestV1, PlaintextLeaseRegistry, grant_to_wire,
)
from ndnsf_distributed_inference.provider import DistributedInferenceProvider
from ndnsf_distributed_inference.sdk.placement import (
    DeviceBinding, DeviceBindingMode, ExecutionRole, GrantBindingV1,
    ProviderSelectionProjectionV3, RoleAssemblySpec, RoleDataflowContract,
)
from ndnsf_distributed_inference.security.artifact_policy_authority import ArtifactPolicyAuthority
from ndnsf_distributed_inference.security.grant_provider import canonical_grant_name


@pytest.fixture(params=["inline", "external"])
def protected_handler(tmp_path, monkeypatch, request):
    external = request.param == "external"
    digest = "sha256:" + "a" * 64
    authority_key = ed25519.Ed25519PrivateKey.generate()
    recipient_key = ed25519.Ed25519PrivateKey.generate()
    requester_key = ed25519.Ed25519PrivateKey.generate()
    now = int(time.time() * 1000)
    authority = ArtifactPolicyAuthority(
        "/authority", authority_key, protection_epoch="epoch-1",
        allowed_model_manifests=frozenset({digest}))
    request = GrantRequestV1(
        provider_identity="/provider/a", request_id="request-2", attempt=2,
        plan_core_digest=digest, grant_view_digest=digest,
        model_manifest_digest=digest, protection_epoch="epoch-1",
        requester_identity="/user", issued_at_ms=now).sign(requester_key)
    grant = authority.issue(
        request, requester_public_key=requester_key.public_key(),
        recipient_public_key=recipient_key.public_key(), content_key=b"k" * 32,
        key_id="key", expires_at_ms=now + 60000, now_ms=now)
    grant_name = canonical_grant_name(
        authority="/authority", provider_identity="/provider/a", request_id="request-2",
        attempt=2, plan_core_digest=digest, model_manifest_digest=digest,
        protection_epoch="epoch-1", grant_digest=grant.grant_digest)
    binding = GrantBindingV1(
        "/provider/a", grant_name, grant.grant_digest, "request-2", 2,
        digest, digest, "epoch-1")
    role = RoleAssemblySpec(
        "stage0", 0, 0, 2, digest, digest, "onnxruntime-cpu",
        adapter_id="onnx", adapter_version="1", protection_epoch="epoch-1")
    projection = ProviderSelectionProjectionV3(
        provider="/provider/a", request_id="request-2", attempt=2,
        plan_core_digest=digest, plan_digest=digest, ack_closed_digest=digest,
        offer_digest=digest, security_policy_snapshot_digest=digest,
        roles=(role,), dependencies=(), deadline_ms=now + 60000,
        execution_role=ExecutionRole(
            "stage0", "stage0", 0, 0, 2, "onnxruntime-cpu",
            adapter_id="onnx", adapter_version="1"),
        assembly=role,
        dataflow=RoleDataflowContract("request-2", 2, digest, "stage0",
                                      terminal_response_owner=True),
        device_binding=DeviceBinding(
            DeviceBindingMode.CPU, "/provider/a", "stage0", digest, digest, digest, 1),
        grant_binding=binding)
    source = tmp_path / "canonical.onnx"
    node = (onnx.helper.make_node("Add", ["x", "w"], ["y"]) if external
            else onnx.helper.make_node("Identity", ["x"], ["y"]))
    model = onnx.helper.make_model(onnx.helper.make_graph(
        [node], "protected-runtime",
        [onnx.helper.make_tensor_value_info("x", onnx.TensorProto.FLOAT, [1])],
        [onnx.helper.make_tensor_value_info("y", onnx.TensorProto.FLOAT, [1])],
        initializer=([onnx.numpy_helper.from_array(np.array([2], np.float32), name="w")]
                     if external else [])),
        opset_imports=[onnx.helper.make_opsetid("", 13)], ir_version=8)
    if external:
        onnx.save_model(model, str(source), save_as_external_data=True,
                        all_tensors_to_one_file=True, location="canonical.weights",
                        size_threshold=0)
    else:
        source.write_bytes(model.SerializeToString())
    native = Mock()
    runtime = DistributedInferenceProvider(
        native, grant_authority_public_key=authority_key.public_key(),
        grant_authority_identity=authority.identity,
        grant_recipient_private_key=recipient_key)
    state = SimpleNamespace(events=[], keys=[], work_dirs=[], paths=[], grant=grant,
                            external=external)
    register_secret = PlaintextLeaseRegistry.register_secret

    def capture_secret(registry, name, data):
        value = register_secret(registry, name, data)
        state.keys.append(value)
        return value

    monkeypatch.setattr(PlaintextLeaseRegistry, "register_secret", capture_secret)

    def fetch(name, **_kwargs):
        assert name == grant_name
        state.events.append("grant-fetch")
        return SimpleNamespace(content=grant_to_wire(state.grant))

    monkeypatch.setattr("ndnsf.fetch_exact_data_packet", fetch)
    local_execution = runtime._local_execution

    def observe_prepare(*args, **kwargs):
        state.events.append("prepare")
        execution = local_execution(*args, **kwargs)
        state.work_dirs.append(execution.work_dir)
        return execution

    monkeypatch.setattr(runtime, "_local_execution", observe_prepare)

    def handler(context):
        state.events.append("handler")
        path = context.execution.artifact_paths["model"]
        state.paths.append(path)
        assert not (context.execution.work_dir / "content-key.bin").exists()
        if external:
            assert (context.execution.work_dir / "model.onnx.data.cipher").is_file()
        session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
        np.testing.assert_array_equal(session.run(None, {"x": np.array([3], np.float32)})[0],
                                      [5 if external else 3])

    runtime.add_capability_handler(
        "/Test", ["stage0"], handler, backends=("onnxruntime-cpu",),
        has_model=True, temp_dir=str(tmp_path / "work"),
        local_artifacts={"stage0": {"path": str(source), "kind": "onnx-model"}})
    wrapped = native.add_collaboration_handler.call_args.args[2]
    ctx = SimpleNamespace(
        assignment=SimpleNamespace(
            role="stage0", service="/Test", assigned_artifact="/artifact/model",
            assignment_payload=projection.to_bytes()),
        local_provider="/provider/a", session_id="request-2", is_streamed=False,
        fail=Mock(), report_operation_status=Mock())
    return runtime, wrapped, ctx, state, source


def test_registered_handler_authenticates_before_prepare_and_erases_after_execution(protected_handler):
    _runtime, wrapped, ctx, state, source = protected_handler
    original = source.read_bytes()
    wrapped(ctx, b"request")
    ctx.fail.assert_not_called()
    assert state.events == ["grant-fetch", "prepare", "handler"]
    assert state.keys and all(not any(key) for key in state.keys)
    assert state.paths and all(not path.exists() for path in state.paths)
    assert source.read_bytes() == original
    assert all(not (root / "model.onnx.data").exists() for root in state.work_dirs)
    if state.external:
        assert source.with_suffix(".weights").read_bytes() == np.array([2], np.float32).tobytes()


def test_grant_rejection_never_reaches_preparation(protected_handler):
    _runtime, wrapped, ctx, state, _source = protected_handler
    state.grant = replace(state.grant, authority_signature="00" * 64)
    wrapped(ctx, b"request")
    assert state.events == ["grant-fetch"]
    assert state.work_dirs == []
    assert state.keys == []
    assert "DI_PROTECTED_GRANT_REJECTED" in ctx.fail.call_args.args[0]


@pytest.mark.parametrize("changed", ["grant-reference", "selection"])
def test_policy_snapshot_substitution_never_reaches_preparation(protected_handler, changed):
    _runtime, wrapped, ctx, state, _source = protected_handler
    projection = ProviderSelectionProjectionV3.from_bytes(ctx.assignment.assignment_payload)
    different = "sha256:" + "b" * 64
    if changed == "grant-reference":
        projection = replace(projection, grant_binding=replace(
            projection.grant_binding, security_policy_snapshot_digest=different))
    else:
        projection = replace(projection, security_policy_snapshot_digest=different)
    ctx.assignment.assignment_payload = projection.to_bytes()
    wrapped(ctx, b"request")
    assert state.events == []
    assert state.work_dirs == [] and state.keys == []
    assert "DI_PROTECTED_GRANT_REJECTED" in ctx.fail.call_args.args[0]


def test_preparation_failure_after_unwrap_erases_keys_and_loaded_model(protected_handler, monkeypatch):
    runtime, wrapped, ctx, state, source = protected_handler

    def fail_metadata(*_args):
        raise RuntimeError("injected post-decrypt preparation failure")

    monkeypatch.setattr(runtime, "_bind_assignment_metadata", fail_metadata)
    wrapped(ctx, b"request")
    assert state.events == ["grant-fetch", "prepare"]
    assert state.keys and all(not any(key) for key in state.keys)
    assert state.work_dirs and all(not (root / "assembled-role.onnx").exists()
                                  for root in state.work_dirs)
    assert source.exists()
    assert "injected post-decrypt preparation failure" in ctx.fail.call_args.args[0]


def test_stored_entry_tampering_erases_partial_loaded_state(protected_handler, monkeypatch):
    from ndnsf_distributed_inference.core.protected_artifacts import AssembledCiphertextV1
    _runtime, wrapped, ctx, state, _source = protected_handler
    write = Path.write_bytes
    target = "model.onnx.data.cipher" if state.external else "assembled-role.onnx.cipher"

    def tamper(path, data):
        if path.name == target:
            sealed = AssembledCiphertextV1.from_bytes(data)
            data = replace(sealed, ciphertext=bytes([sealed.ciphertext[0] ^ 1])
                           + sealed.ciphertext[1:]).to_bytes()
        return write(path, data)

    monkeypatch.setattr(Path, "write_bytes", tamper)
    wrapped(ctx, b"request")
    assert state.events == ["grant-fetch", "prepare"]
    assert "DI_PROTECTED_GRANT_REJECTED" in ctx.fail.call_args.args[0]
    assert "authentication" in ctx.fail.call_args.args[0]
    assert state.keys and all(not any(key) for key in state.keys)
    for root in state.work_dirs:
        assert not (root / "assembled-role.onnx").exists()
        assert not (root / "model.onnx.data").exists()


@pytest.mark.parametrize("boundary", ["before-fetch", "during-fetch", "after-prepare"])
def test_cancelled_request_never_enters_protected_handler(protected_handler, monkeypatch, boundary):
    import ndnsf
    runtime, wrapped, ctx, state, source = protected_handler
    original = source.read_bytes()
    ctx.is_streamed = True
    ctx.stream_cancelled = boundary == "before-fetch"
    if boundary == "during-fetch":
        fetch = ndnsf.fetch_exact_data_packet
        def cancel_fetch(*args, **kwargs):
            packet = fetch(*args, **kwargs)
            ctx.stream_cancelled = True
            return packet
        monkeypatch.setattr(ndnsf, "fetch_exact_data_packet", cancel_fetch)
    elif boundary == "after-prepare":
        bind = runtime._bind_assignment_metadata
        def cancel_prepared(*args, **kwargs):
            execution = bind(*args, **kwargs)
            ctx.stream_cancelled = True
            return execution
        monkeypatch.setattr(runtime, "_bind_assignment_metadata", cancel_prepared)
    wrapped(ctx, b"request")
    assert "handler" not in state.events
    if boundary == "before-fetch":
        assert state.events == []
    if boundary != "after-prepare":
        assert state.work_dirs == [] and state.keys == []
    else:
        assert state.keys and all(not any(key) for key in state.keys)
    assert "DI_PROTECTED_GRANT_REJECTED" in ctx.fail.call_args.args[0]
    assert "cancel" in ctx.fail.call_args.args[0]
    assert source.read_bytes() == original
    for root in state.work_dirs:
        assert not (root / "assembled-role.onnx").exists()
        assert not (root / "model.onnx.data").exists()


@pytest.mark.parametrize("boundary", ["before-fetch", "during-fetch", "after-prepare"])
def test_selection_deadline_bounds_protected_preparation(protected_handler, monkeypatch, boundary):
    import importlib
    import ndnsf
    module = importlib.import_module("ndnsf_distributed_inference.provider")
    runtime, wrapped, ctx, state, source = protected_handler
    now = int(time.time() * 1000)
    deadline = now + 250
    projection = ProviderSelectionProjectionV3.from_bytes(ctx.assignment.assignment_payload)
    ctx.assignment.assignment_payload = replace(projection, deadline_ms=deadline).to_bytes()
    clock = [deadline if boundary == "before-fetch" else now]
    monkeypatch.setattr(module, "time", lambda: clock[0] / 1000)
    fetch = ndnsf.fetch_exact_data_packet
    def bounded_fetch(*args, **kwargs):
        state.fetch_timeout_ms = kwargs["timeout_ms"]
        packet = fetch(*args, **kwargs)
        if boundary == "during-fetch":
            clock[0] = deadline
        return packet
    monkeypatch.setattr(ndnsf, "fetch_exact_data_packet", bounded_fetch)
    if boundary == "after-prepare":
        bind = runtime._bind_assignment_metadata
        def expire_prepared(*args, **kwargs):
            execution = bind(*args, **kwargs)
            clock[0] = deadline
            return execution
        monkeypatch.setattr(runtime, "_bind_assignment_metadata", expire_prepared)
    wrapped(ctx, b"request")
    assert "handler" not in state.events
    if boundary == "before-fetch":
        assert state.events == []
    else:
        assert 0 < state.fetch_timeout_ms <= 250
    if boundary != "after-prepare":
        assert state.work_dirs == [] and state.keys == []
    else:
        assert state.keys and all(not any(key) for key in state.keys)
    assert "DI_PROTECTED_GRANT_REJECTED" in ctx.fail.call_args.args[0]
    assert "deadline" in ctx.fail.call_args.args[0]
    assert source.exists()
    for root in state.work_dirs:
        assert not (root / "assembled-role.onnx").exists()
        assert not (root / "model.onnx.data").exists()


@pytest.mark.parametrize("fence", ["cancel", "request-deadline", "grant-expiry"])
def test_queued_handler_rechecks_protected_authority(protected_handler, monkeypatch, fence):
    import importlib
    from concurrent.futures import ThreadPoolExecutor
    from threading import Event, Thread
    module = importlib.import_module("ndnsf_distributed_inference.provider")
    runtime, wrapped, ctx, state, source = protected_handler
    now = int(time.time() * 1000)
    deadline = now + 250 if fence == "request-deadline" else state.grant.expires_at_ms + 30000
    projection = ProviderSelectionProjectionV3.from_bytes(ctx.assignment.assignment_payload)
    ctx.assignment.assignment_payload = replace(projection, deadline_ms=deadline).to_bytes()
    clock = [now]
    monkeypatch.setattr(module, "time", lambda: clock[0] / 1000)
    ctx.is_streamed, ctx.stream_cancelled = True, False
    gate, occupied, queued = Event(), Event(), Event()
    errors = []
    with ThreadPoolExecutor(max_workers=1) as pool:
        def occupy():
            occupied.set()
            assert gate.wait(10), "test worker gate timed out"
        blocker = pool.submit(occupy)
        assert occupied.wait(5)
        runtime._handler_executor = pool
        submit = pool.submit
        def observe_submit(*args, **kwargs):
            future = submit(*args, **kwargs)
            queued.set()
            return future
        monkeypatch.setattr(pool, "submit", observe_submit)
        def invoke():
            try:
                wrapped(ctx, b"request")
            except Exception as error:
                errors.append(error)
        callback = Thread(target=invoke)
        callback.start()
        try:
            assert queued.wait(5), "registered handler never entered the real executor queue"
            if fence == "cancel":
                ctx.stream_cancelled = True
            else:
                clock[0] = deadline if fence == "request-deadline" else state.grant.expires_at_ms
        finally:
            gate.set()
            callback.join(10)
            runtime._handler_executor = None
        blocker.result()
        assert not callback.is_alive()
    assert errors == []
    assert "handler" not in state.events
    assert "DI_PROTECTED_GRANT_REJECTED" in ctx.fail.call_args.args[0]
    assert ("cancel" if fence == "cancel" else "deadline" if fence == "request-deadline"
            else "grant expired") in ctx.fail.call_args.args[0]
    assert state.keys and all(not any(key) for key in state.keys)
    assert source.exists()
    for root in state.work_dirs:
        assert not (root / "assembled-role.onnx").exists()
        assert not (root / "model.onnx.data").exists()


def test_handler_exception_drains_protected_files_and_key(protected_handler, monkeypatch):
    runtime, wrapped, ctx, state, source = protected_handler
    run = runtime._run_handler
    def fail_after_load(handler, context):
        run(handler, context)
        raise RuntimeError("injected handler failure after model load")
    monkeypatch.setattr(runtime, "_run_handler", fail_after_load)
    with pytest.raises(RuntimeError, match="handler failure after model load"):
        wrapped(ctx, b"request")
    assert "handler" in state.events
    assert state.keys and all(not any(key) for key in state.keys)
    assert source.exists()
    for root in state.work_dirs:
        assert not (root / "assembled-role.onnx").exists()
        assert not (root / "model.onnx.data").exists()
