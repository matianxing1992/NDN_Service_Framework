from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/run_spec180_case.py"


def load_dispatcher():
    spec = importlib.util.spec_from_file_location("spec180_dispatcher", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _yolo_environment() -> dict[str, str]:
    return {
        "SPEC180_YOLO_CANONICAL_PACKAGE": "/bundle/candidate/yolo26n",
        "SPEC180_YOLO_CATALOGUE_REGISTRY": "/bundle/candidate/registry.json",
        "SPEC180_YOLO_CATALOG_DATA_NAME": "/example/controller/NDNSF/DI/catalogue/v1",
        "SPEC180_YOLO_CATALOG_SIGNER": "/example/controller",
        "SPEC180_YOLO_OFFER_TRUST_ROOT": "/bundle/candidate/trust-root.json",
        "SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP": "/bundle/candidate/key-map.json",
        "SPEC180_YOLO_TOPOLOGY": "/bundle/candidate/topology.conf",
        "SPEC180_YOLO_CONFIG": "/bundle/candidate/config.json",
    }


def _workload(module, *, gate: str = "yolo-functional") -> dict[str, object]:
    registered = module._REGISTERED[gate]
    return {
        "schema": module.WORKLOAD_SCHEMA,
        "gate": gate,
        "case": registered["case"],
        "entrypoint": registered["entrypoint"],
        "args": list(registered["args"]),
        "environment": (_yolo_environment() if gate == "yolo-functional" else {
            "SPEC180_QWEN_MODEL": "Qwen/Qwen3.6-27B",
            "SPEC180_QWEN_REVISION": "model-revision-1",
            "SPEC180_QWEN_MODEL_IDENTITY_DIGEST": "sha256:" + "a" * 64,
            "SPEC180_QWEN_PROMPT_DIGEST": "sha256:" + "b" * 64,
        }),
        "evidenceSchema": module.EVIDENCE_SCHEMA,
    }


def test_yolo_dispatch_workload_is_closed_and_normalized():
    module = load_dispatcher()
    value = module.validate_workload(_workload(module), "yolo-functional")
    assert value["case"] == "Y-B"
    assert value["entrypoint"] == "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"
    assert value["args"] == ["--case", "Y-B"]
    assert set(value["environment"]) == set(module.YOLO_ENVIRONMENT_FIELDS)


def test_qwen_dispatch_workload_keeps_formal_gate_identity():
    module = load_dispatcher()
    value = module.validate_workload(
        _workload(module, gate="qwen-functional"), "qwen-functional")
    assert value["case"] == "QWEN-F"
    assert value["entrypoint"] == "Experiments/NDNSF_DI_QwenAckDriven_Minindn.py"
    assert value["args"] == ["--case", "QWEN-F"]
    assert "SPEC180_QWEN_MODEL_MANIFEST" not in value["environment"]


def test_qwen_dispatch_never_falls_back_to_spec175_wrapper(tmp_path: Path):
    module = load_dispatcher()
    workload = _workload(module, gate="qwen-functional")
    assert "StreamedGeneration" not in workload["entrypoint"]
    with pytest.raises(module.DispatcherError, match="ENTRYPOINT_MISSING"):
        module.build_dispatch_command(workload, tmp_path)


def test_qwen_identity_metadata_requires_content_digest():
    module = load_dispatcher()
    workload = _workload(module, gate="qwen-functional")
    workload["environment"]["SPEC180_QWEN_PROMPT_DIGEST"] = "prompt-v1"
    with pytest.raises(module.DispatcherError, match="INVALID_DIGEST"):
        module.validate_workload(workload, "qwen-functional")


@pytest.mark.parametrize("mutation, error", [
    (lambda value: value.update(extra="x"), "WORKLOAD_FIELD_UNKNOWN"),
    (lambda value: value.update(case="Y-A"), "WORKLOAD_CASE_MISMATCH"),
    (lambda value: value.update(entrypoint="/tmp/runner.py"),
     "ENTRYPOINT_PATH_UNSAFE"),
    (lambda value: value.update(entrypoint="../Experiments/runner.py"),
     "ENTRYPOINT_PATH_UNSAFE"),
    (lambda value: value.update(args=["--case", "Y-B; touch /tmp/x"]),
     "ARGS_SHELL_FRAGMENT"),
    (lambda value: value["environment"].update(SPEC180_BAD="1"),
     "ENVIRONMENT_FIELD_FORBIDDEN"),
    (lambda value: value["environment"].update(
        SPEC180_YOLO_CONFIG="/home/tianxing/config.json"),
     "ENVIRONMENT_PATH_UNSAFE"),
    (lambda value: value["environment"].update(
        SPEC180_YOLO_CONFIG="/bundle/private-key.pem"),
     "ENVIRONMENT_VALUE_FORBIDDEN"),
])
def test_dispatch_workload_rejects_unsafe_mutations(mutation, error):
    module = load_dispatcher()
    value = _workload(module)
    mutation(value)
    with pytest.raises(module.DispatcherError, match=error):
        module.validate_workload(value, "yolo-functional")


def test_duplicate_json_fields_and_digest_mismatch_fail_before_dispatch(tmp_path: Path):
    module = load_dispatcher()
    workload_path = tmp_path / "workload.json"
    workload_path.write_text(
        '{"schema":"spec180-dispatch-workload-v1",'
        '"schema":"spec180-dispatch-workload-v1"}', encoding="utf-8")
    with pytest.raises(module.DispatcherError, match="WORKLOAD_NOT_JSON"):
        module.load_and_validate_workload(
            workload_path, _digest(workload_path.read_bytes()), "yolo-functional")

    valid = json.dumps(_workload(module), sort_keys=True).encode("utf-8")
    workload_path.write_bytes(valid)
    with pytest.raises(module.DispatcherError, match="WORKLOAD_DIGEST_MISMATCH"):
        module.load_and_validate_workload(
            workload_path, "sha256:" + "c" * 64, "yolo-functional")


def test_entrypoint_must_be_under_bundle_and_command_is_fixed(tmp_path: Path):
    module = load_dispatcher()
    entrypoint = tmp_path / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"
    entrypoint.parent.mkdir(parents=True)
    entrypoint.write_text("#!/usr/bin/env python3\n", encoding="utf-8")
    workload = _workload(module)
    command = module.build_dispatch_command(
        workload, tmp_path, python_executable="/bundle/usr/bin/python3")
    assert command == [
        "/bundle/usr/bin/python3", str(entrypoint), "--case", "Y-B",
    ]
    entrypoint.unlink()
    with pytest.raises(module.DispatcherError, match="ENTRYPOINT_MISSING"):
        module.build_dispatch_command(_workload(module), tmp_path)


def test_child_environment_drops_ambient_values_and_binds_output(tmp_path: Path):
    module = load_dispatcher()
    runtime = {
        "SPEC180_GATE": "yolo-functional",
        "SPEC180_PROFILE_ID": "spec180-fixed-v1",
        "SPEC180_PROFILE_SHA256": "sha256:" + "a" * 64,
        "SPEC180_RUN_ID": "run-1",
        "SPEC180_CANDIDATE_ID": "candidate-1",
        "SPEC180_CANDIDATE_DIGEST": "sha256:" + "b" * 64,
        "SPEC180_SIF": "/inputs/candidate.sif",
        "SPEC180_SIF_SHA256": "sha256:" + "c" * 64,
        "SPEC180_MODEL_MANIFEST": "/inputs/model-manifest.json",
        "SPEC180_MODEL_MANIFEST_SHA256": "sha256:" + "d" * 64,
        "SPEC180_MODEL_ROOT": "/models",
        "SPEC180_WORKLOAD": "/inputs/workload.json",
        "SPEC180_WORKLOAD_SHA256": "sha256:" + "e" * 64,
        "SPEC180_OUTPUT_ROOT": str(tmp_path),
    }
    result = module.build_child_environment(
        _yolo_environment(), runtime, tmp_path)
    assert "LD_PRELOAD" not in result
    assert result["PATH"] == "/usr/local/bin:/usr/bin:/bin"
    assert result["SPEC180_CASE_OUTPUT_DIR"] == str(tmp_path)
    with pytest.raises(module.DispatcherError, match="ENVIRONMENT_RUNTIME_OVERRIDE"):
        module.build_child_environment(
            {**_yolo_environment(), "SPEC180_GATE": "bad"}, runtime, tmp_path)
