"""Focused oracle/production-User tests; no model, NFD or qualification run."""
from __future__ import annotations

import ast
import hashlib
import json
import os
from pathlib import Path
import struct
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
                 "load_reference": load_reference, "_REPO_ROOT": ROOT}
    for filename, names in (
        ("yolo_2x2_lib.py", {"encode_native_tensor_bundle", "_decode_native_tensor_bundle", "decode_yolo_output"}),
        ("user.py", {"_record_yolo_numerical_result", "_prepare_yolo_input"}),
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
                                           execution_plan_digest="bound-plan",
                                           sealed_plan=SimpleNamespace(plan_digest="carrier-plan")),
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
