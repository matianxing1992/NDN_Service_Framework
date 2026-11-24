"""Focused oracle/production-User tests; no model, NFD or qualification run."""
from __future__ import annotations

import ast
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
from threading import Event
from types import SimpleNamespace

import numpy as np
import pytest

from ndnsf_distributed_inference.adapters.yolo.reference import (
    compare_reference, fixture_tensor, load_reference,
)

ROOT = Path(__file__).resolve().parents[2]
EXAMPLE = ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2"
FIXTURE = "tests/fixtures/spec180/yolo26n/fixed-fixture.ppm"


@pytest.fixture
def reference(tmp_path):
    package = tmp_path / "package"
    (package / "oracle").mkdir(parents=True)
    expected = np.array([[[1, 2, 3, 4, .9, 0], [2, 3, 4, 5, .8, 1]]], dtype=np.float32)
    np.save(package / "oracle/full-model-output.npy", expected)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = {
        "fixture": {"path": FIXTURE, "revision": "spec180-fixed-fixture-v1",
                    "sha256": sha(ROOT / FIXTURE)},
        "preprocessing": {"identity": "float32-NCHW-RGB-0-to-1", "inputName": "images", "shape": [1, 3, 640, 640]},
        "postprocessing": {"identity": "YOLO26n-canonical-detection-rows", "outputName": "predictions",
                           "confidenceThreshold": .001, "sort": "confidence-desc,class-asc,xyxy-asc"},
        "oracle": {"fixtureRevision": "spec180-fixed-fixture-v1", "fixtureSha256": sha(ROOT / FIXTURE),
                   "inputShape": [1, 3, 640, 640], "outputShape": [1, 2, 6], "outputDtype": "float32",
                   "outputPath": "full-model-output.npy", "outputDigest": "sha256:" + sha(package / "oracle/full-model-output.npy")},
    }
    (package / "manifest.json").write_text(json.dumps(manifest))
    return package, expected


def test_preprocessing_matches_offline_torch_half_pixel_resize():
    import torch
    # Independent existing exporter implementation, not a model forward pass.
    namespace = {"Path": Path, "ExportError": ValueError}
    tree = ast.parse((ROOT / "tools/ndnsf-di/export_spec180_yolo26_onnx.py").read_text())
    node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "_fixture_tensor")
    exec(compile(ast.Module(body=[node], type_ignores=[]), "exporter", "exec"), namespace)
    expected = namespace["_fixture_tensor"](ROOT / FIXTURE, 640).numpy()
    actual = fixture_tensor(ROOT / FIXTURE, 640)
    assert actual.dtype == np.float32 and actual.shape == (1, 3, 640, 640)
    np.testing.assert_allclose(actual, expected, atol=2e-7, rtol=0)
    assert "torch" not in fixture_tensor.__globals__


def test_reference_comparison_is_redacted_and_canonical(reference):
    package, expected = reference
    loaded = load_reference(package, ROOT, 640)
    result = compare_reference(loaded, expected[:, ::-1, :])
    assert result["matched"] is True and result["maxAbsError"] == 0
    assert result["atol"] == .001 and result["rtol"] == .0001
    assert result["shape"] == [1, 2, 6]
    assert "predictions" not in result and "input" not in result
    changed = expected.copy()
    changed[0, 0, 0] += .1
    assert compare_reference(loaded, changed)["matched"] is False


def test_reference_uses_package_bound_input_resolution(reference):
    package, _ = reference
    path = package / "manifest.json"
    manifest = json.loads(path.read_text())
    shape = [1, 3, 32, 32]
    manifest["preprocessing"]["shape"] = shape
    manifest["oracle"]["inputShape"] = shape
    path.write_text(json.dumps(manifest))

    loaded = load_reference(package, ROOT, 32)

    assert loaded.input_tensor.shape == tuple(shape)


@pytest.mark.parametrize("bad", ["nan", "inf", "class", "dtype", "shape"])
def test_invalid_numerical_output_is_rejected(reference, bad):
    package, expected = reference
    actual = expected.copy()
    if bad in ("nan", "inf"):
        actual[0, 0, 0] = float(bad)
    elif bad == "class":
        actual[0, 0, 5] = .5
    elif bad == "dtype":
        actual = actual.astype(np.float64)
    else:
        actual = actual[0]
    with pytest.raises(ValueError):
        compare_reference(load_reference(package, ROOT, 640), actual)


@pytest.mark.parametrize("bad", ["oracle_hash", "fixture_hash", "path", "semantics", "input_size"])
def test_reference_preflight_rejects_mismatched_subject(reference, bad):
    package, _ = reference
    path = package / "manifest.json"
    manifest = json.loads(path.read_text())
    if bad == "oracle_hash":
        manifest["oracle"]["outputDigest"] = "sha256:" + "0" * 64
    elif bad == "fixture_hash":
        manifest["fixture"]["sha256"] = "0" * 64
    elif bad == "path":
        manifest["oracle"]["outputPath"] = "../../escape.npy"
    elif bad == "semantics":
        manifest["postprocessing"]["sort"] = "xyxy-first"
    path.write_text(json.dumps(manifest))
    with pytest.raises(ValueError):
        load_reference(package, ROOT, 320 if bad == "input_size" else 640)


def _production_functions():
    namespace = {"np": np, "struct": struct, "Path": Path, "json": json, "os": os,
                 "hashlib": hashlib, "compare_reference": compare_reference,
                 "load_reference": load_reference, "_REPO_ROOT": ROOT,
                 "math": math, "sys": sys, "Event": Event}
    for filename, names in (
        ("yolo_2x2_lib.py", {"encode_native_tensor_bundle", "_decode_native_tensor_bundle", "decode_yolo_output"}),
        ("user.py", {"_record_yolo_numerical_result", "_prepare_yolo_input",
                     "_load_yolo_native_payload"}),
    ):
        tree = ast.parse((EXAMPLE / filename).read_text())
        nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
        assert len(nodes) == len(names)
        exec(compile(ast.Module(body=nodes, type_ignores=[]), filename, "exec"), namespace)
    return namespace


def test_actual_user_input_is_registered_native_fixture(reference, tmp_path):
    package, _ = reference
    functions = _production_functions()
    args = SimpleNamespace(input_size=640, native_tensor_input=True, input_payload_file="")
    loaded, payload = functions["_prepare_yolo_input"](args, package)
    tensors = functions["_decode_native_tensor_bundle"](payload)
    assert set(tensors) == {"images"}
    np.testing.assert_array_equal(tensors["images"], loaded.input_tensor)
    path = tmp_path / "supplied-input.bin"
    path.write_bytes(payload)
    args.input_payload_file = str(path)
    assert functions["_prepare_yolo_input"](args, package)[1] == payload
    path.write_bytes(b"unregistered input")
    with pytest.raises(ValueError, match="REGISTERED_FIXTURE_PAYLOAD_MISMATCH"):
        functions["_prepare_yolo_input"](args, package)
    args.input_payload_file = ""
    args.native_tensor_input = False
    with pytest.raises(ValueError, match="REQUIRES_NATIVE_TENSOR"):
        functions["_prepare_yolo_input"](args, package)


@pytest.mark.parametrize("matched,remote_status,exit_code", [(True, True, 0), (False, True, 4), (True, False, 3)])
def test_actual_user_terminal_branch_cannot_print_false_success(matched, remote_status, exit_code, capsys):
    # Execute the unmodified production tail, with transport/journal doubles.
    tree = ast.parse((EXAMPLE / "user.py").read_text())
    node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "_load_yolo_ack_driven")
    start = next(i for i, n in enumerate(node.body)
                 if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "response" for t in n.targets))
    node.body = node.body[start:]
    node.returns = None
    for argument in node.args.args:
        argument.annotation = None
    calls = []
    journal = SimpleNamespace(attempt_id="attempt-1", append=lambda *a, **k: None, validate_complete=lambda: None)
    namespace = {"hashlib": hashlib, "journal": journal, "numerical_reference": object(),
                 "handle": SimpleNamespace(response=lambda timeout: SimpleNamespace(status=remote_status, payload=b"real response", error="secret"),
                                           sealed_plan=SimpleNamespace(plan_digest="bound-plan")),
                 "_record_yolo_numerical_result": lambda *args: calls.append(args) or matched}
    exec(compile(ast.Module(body=[node], type_ignores=[]), "user-terminal", "exec"), namespace)
    assert namespace["_load_yolo_ack_driven"](None, SimpleNamespace(timeout_ms=10)) == exit_code
    stdout = capsys.readouterr().out
    assert ("status=true" in stdout) is (exit_code == 0)
    assert "secret" not in stdout
    assert len(calls) == int(remote_status)
    if calls:
        assert calls[0][2:] == (b"real response", "bound-plan", "attempt-1")


@pytest.mark.parametrize("mode", ["pass", "mismatch", "wrong_name", "extra_tensor", "duplicate", "npz"])
def test_actual_user_evidence_function_checks_received_response(reference, tmp_path, mode):
    package, expected = reference
    functions = _production_functions()
    if mode == "mismatch":
        expected[0, 0, 0] += .1
    name = "output" if mode == "wrong_name" else "predictions"
    values = {name: expected}
    if mode == "extra_tensor":
        values["unexpected"] = expected
    payload = functions["encode_native_tensor_bundle"](values)
    if mode == "duplicate":
        payload = payload[:8] + struct.pack("<I", 2) + payload[12:] * 2
    if mode == "npz":
        payload = b"PK-not-a-native-response"
    args = SimpleNamespace(lifecycle_output_dir=str(tmp_path), lifecycle_case="Y-B", request_id="/test/request")
    matched = functions["_record_yolo_numerical_result"](
        args, load_reference(package, ROOT, 640), payload, "sha256:" + "a" * 64, "attempt-1")
    assert matched is (mode == "pass")
    record = json.loads((tmp_path / "yolo-numerical.json").read_text())
    assert record["matched"] is matched and record["requestId"] == args.request_id
    assert record["attemptId"] == "attempt-1"
    assert record["responseDigest"] == "sha256:" + hashlib.sha256(payload).hexdigest()
    assert "payload" not in record and "predictions" not in record
    # Freshness: never overwrite a previous case record.
    with pytest.raises(FileExistsError):
        functions["_record_yolo_numerical_result"](
            args, load_reference(package, ROOT, 640), payload, "sha256:" + "a" * 64, "attempt-1")


@pytest.fixture
def native_host(reference, monkeypatch):
    """Only the host scheduler is fake; real encoding and numerical checks run."""
    package, expected = reference
    manifest = json.loads((package / "manifest.json").read_text())
    manifest["source"] = {"checkpointSha256": "a" * 64}
    (package / "manifest.json").write_text(json.dumps(manifest))
    functions = _production_functions()
    rows, handles, configured = [], [], []
    clock = SimpleNamespace(now=0.0)
    def sleep(delay):
        assert delay >= 0
        clock.now += delay
    functions.update(
        time=SimpleNamespace(monotonic=lambda: clock.now, sleep=sleep),
        build_yolo26n_adapter=lambda *a, **kw: SimpleNamespace(
            descriptor=SimpleNamespace(name="fixture", input_schema_digest="sha256:" + "b" * 64)),
        yolo_inference_service=lambda deployment: "/Inference/Fixture",
        _emit_native_request_result=rows.append)
    monkeypatch.setitem(sys.modules, "ndnsf_distributed_inference", SimpleNamespace(
        api=SimpleNamespace(RequestOptions=SimpleNamespace, Input=SimpleNamespace(
            repository=lambda reference, options: SimpleNamespace(
                reference=reference, application_options=bytes(options))))))
    configuration = package.parent / "native.json"
    provider_names = ["/fixture/provider-a", "/fixture/provider-b"]
    configuration.write_text(json.dumps({"request": {"provider_names": provider_names}}))
    args = SimpleNamespace(
        native_requester_config=str(configuration), canonical_package=str(package),
        catalogue_registry="registry.json", native_tensor_input=True,
        lifecycle_output_dir="", lifecycle_case="", request_id="",
        async_requests=1, sequential_requests=0, sequential_duration_s=0.0,
        preflight_requests=0, sequential_interval_ms=0, timeout_ms=1000,
        ack_timeout_ms=100, native_restart_gate="", native_restart_ticket="",
        native_restart_after=0, input_size=640, input_payload_file="")
    state = SimpleNamespace(error_index=None, error_type=RuntimeError,
                            expected_submitted_before_result=None,
                            subscription_error_index=None, deferred=False,
                            missing_completion_index=None, published=0)
    payload = functions["encode_native_tensor_bundle"]({"predictions": expected})
    callbacks = []

    class ControlledEvent(Event):
        def wait(self, timeout):
            assert timeout == 2
            return self.is_set()

    functions["Event"] = ControlledEvent

    class Handle:
        def __init__(self, index):
            self.index = index
            self.id = str(index)
            self.status_name = "RUNNING"
            self.cancels = self.unsubscribes = 0
            self.started = clock.now
        def on_completion(self, callback):
            if self.index == state.subscription_error_index:
                raise RuntimeError("observer registration failed")
            if self.index != state.missing_completion_index:
                if state.deferred:
                    callbacks.append(callback)
                    if len(callbacks) == args.async_requests:
                        for ready in reversed(callbacks):
                            clock.now += .01
                            ready(None, None)
                else:
                    callback(None, None)
            def unsubscribe():
                self.unsubscribes += 1
            return SimpleNamespace(cancel=unsubscribe)
        def result(self, *, timeout_s):
            assert timeout_s == 0
            if state.expected_submitted_before_result is not None:
                assert len(handles) == state.expected_submitted_before_result
            clock.now += .1
            if self.index == state.error_index:
                raise state.error_type("native failure")
            self.status_name = "SUCCEEDED"
            return SimpleNamespace(payload=payload)
        def cancel(self):
            self.cancels += 1
            self.status_name = "CANCELLED"

    model = SimpleNamespace(
        manifest=SimpleNamespace(model_name="YOLO26n", model_digest="sha256:" + "a" * 64,
                                 task_name="detect"),
        capabilities=SimpleNamespace(input_schema_json=json.dumps(
            {"schema": "native-input-v1", "digest": "sha256:" + "b" * 64})))

    def publish_input(prepared, value, *, timeout_s):
        assert prepared is model
        assert value and all(isinstance(item, int) for item in value)
        assert timeout_s == args.timeout_ms / 1000
        state.published += 1
        return object()

    def submit(prepared, value, *, options):
        assert prepared is model
        assert value.application_options == b"{}"
        assert options.timeout_s == args.timeout_ms / 1000
        assert options.ack_timeout_s == args.ack_timeout_ms / 1000
        assert options.provider_names == provider_names
        handle = Handle(len(handles))
        handles.append(handle)
        return handle
    def prepare():
        configured.append(args.native_requester_config)
        return model
    user = SimpleNamespace(prepare=prepare, publish_input=publish_input, request=submit)
    runtime = SimpleNamespace(user=lambda: user)
    return SimpleNamespace(run=lambda: functions["_load_yolo_native_payload"](runtime, args),
                           args=args, state=state, handles=handles, rows=rows,
                           clock=clock, configured=configured, model=model)


def test_native_host_submits_the_whole_async_batch_before_waiting(native_host):
    host = native_host
    host.args.async_requests = 3
    host.state.deferred = True
    host.state.expected_submitted_before_result = 3
    assert host.run() == 0
    assert len(host.handles) == 3
    assert all(handle.unsubscribes == 1 and handle.cancels == 0 for handle in host.handles)
    assert sum("REQUEST_RESULT status=true" in row for row in host.rows) == 3
    elapsed = [float(row.split("elapsed_ms=")[1]) for row in host.rows
               if row.startswith("YOLO_NATIVE_REQUEST_RESULT")]
    assert elapsed == pytest.approx([30, 20, 10])


@pytest.mark.parametrize("field,value", [
    ("model_name", "OtherModel"), ("model_digest", "sha256:" + "0" * 64),
    ("input_schema_json", '{"digest":"sha256:wrong"}'),
])
def test_native_host_rejects_public_model_identity_drift_before_publication(native_host, field, value):
    host = native_host
    target = host.model.capabilities if field == "input_schema_json" else host.model.manifest
    setattr(target, field, value)
    with pytest.raises(RuntimeError, match="model identity does not match package"):
        host.run()
    assert host.state.published == 0 and host.handles == []


def test_native_host_warmup_is_separate_from_paced_measured_requests(native_host):
    host = native_host
    host.args.preflight_requests = 2
    host.args.sequential_requests = 3
    host.args.sequential_interval_ms = 200
    assert host.run() == 0
    assert [h.started for h in host.handles] == pytest.approx([0, .1, .2, .4, .6])
    assert sum(row.startswith("YOLO_NATIVE_PREFLIGHT_REQUEST_RESULT") for row in host.rows) == 2
    assert sum(row.startswith("YOLO_NATIVE_REQUEST_RESULT") for row in host.rows) == 3
    assert all(h.unsubscribes == 1 and h.cancels == 0 for h in host.handles)


def test_native_host_duration_stops_admission_at_the_requested_deadline(native_host):
    host = native_host
    host.args.sequential_duration_s = .5
    host.args.sequential_interval_ms = 200
    assert host.run() == 0
    assert [h.started for h in host.handles] == pytest.approx([0, .2, .4])
    assert host.clock.now == pytest.approx(.5)


@pytest.mark.parametrize("error_type", [RuntimeError, KeyboardInterrupt])
def test_native_host_early_failure_cancels_every_remaining_native_handle(native_host, error_type):
    host = native_host
    host.args.async_requests = 3
    host.state.error_index = 0
    host.state.error_type = error_type
    if error_type is KeyboardInterrupt:
        with pytest.raises(KeyboardInterrupt):
            host.run()
    else:
        assert host.run() == 3
    assert len(host.handles) == 3
    assert all(h.cancels == 1 and h.unsubscribes == 1 for h in host.handles)


def test_native_host_registration_failure_also_cancels_unobserved_handle(native_host):
    host = native_host
    host.args.async_requests = 3
    host.state.subscription_error_index = 1
    with pytest.raises(RuntimeError, match="observer registration failed"):
        host.run()
    assert len(host.handles) == 2
    assert [h.cancels for h in host.handles] == [1, 1]
    assert [h.unsubscribes for h in host.handles] == [1, 0]


def test_native_host_missing_completion_cancels_and_unsubscribes_the_batch(native_host):
    host = native_host
    host.args.async_requests = 3
    host.state.missing_completion_index = 0
    assert host.run() == 3
    assert len(host.handles) == 3
    assert all(h.cancels == 1 and h.unsubscribes == 1 for h in host.handles)


@pytest.mark.parametrize("field,value", [
    ("async_requests", 0), ("sequential_requests", -1), ("preflight_requests", -1),
    ("sequential_interval_ms", -1), ("sequential_duration_s", float("nan")),
    ("sequential_duration_s", float("inf")), ("sequential_duration_s", -1.0),
])
def test_native_host_rejects_invalid_budgets_before_configuration_or_submission(native_host, field, value):
    host = native_host
    setattr(host.args, field, value)
    with pytest.raises(ValueError, match="counts, duration and interval"):
        host.run()
    assert host.configured == [] and host.handles == []
