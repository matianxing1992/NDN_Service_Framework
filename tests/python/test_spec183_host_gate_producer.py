import importlib.util
import json
import shutil
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
HOST_TEST = ROOT / "tests/python/test_spec183_yolo_host_gate.py"
PRODUCER = ROOT / "Experiments/TigerCluster/tools/spec183_host_gate.py"


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


host_tests = _load(HOST_TEST, "spec183_host_gate_fixture")
producer = _load(PRODUCER, "spec183_host_gate_producer")


def _copy_case_fixture(tmp_path, fixture_root, owner, case, kinds):
    output = tmp_path / owner / case / "host-minindn" / "output"
    output.mkdir(parents=True)
    for kind in kinds:
        row = fixture_root / (case + "-" + kind + ".json")
        if not row.is_file():
            raise AssertionError("missing fixture: " + str(row))
        target_name = {
            "lifecycle": "lifecycle.jsonl",
            "numeric": "yolo-numerical.json",
            "cleanup": "cleanup-attempt-001.json",
            "failure": "negative-evidence.json",
        }[kind]
        shutil.copyfile(row, output / target_name)
    if case == "normal":
        source = fixture_root / "normal-execution.json"
        # The fixture's execution file is line-oriented despite its suffix.
        lines = source.read_text(encoding="utf-8").splitlines()
        for line in lines:
            role = json.loads(line.split("NDNSF_DI_EXECUTION_EVIDENCE_UPDATE ", 1)[1])[
                "providerName"
            ].rsplit("/", 1)[-1]
            (output / ("provider-" + role + ".log")).write_text(
                line + "\n", encoding="utf-8"
            )
    return output


def _prepare_fixture(tmp_path):
    source, _receipt, _value = host_tests.write_receipt(tmp_path)
    # write_receipt keeps the execution fixture in a JSON file; expose it under
    # the producer's expected four provider log names.
    normal_execution = tmp_path / "normal-execution.json"
    rows = []
    for role in ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge"):
        rows.append(
            "NDNSF_DI_EXECUTION_EVIDENCE_UPDATE "
            + json.dumps({
                "providerName": "/example/yolo/run-1/" + role,
                "requestId": "/example/yolo/run-1/normal",
                "runnerKind": "native-yolo-postprocess" if role == "Merge" else "onnxruntime-cpu",
                "realCompute": "false" if role == "Merge" else "true",
                "loadCompleted": "false" if role == "Merge" else "true",
                "warmupCompleted": "false" if role == "Merge" else "true",
                "cpuFallbackUsed": "false",
                "executionCompleted": "true",
            })
        )
    normal_execution.write_text("\n".join(rows) + "\n", encoding="utf-8")
    owner = tmp_path / "run-1"
    (owner / "public").mkdir(parents=True)
    (owner / "public" / "preparation.json").write_text("{}", encoding="utf-8")
    normal = _copy_case_fixture(tmp_path, tmp_path, "run-1", "normal",
                                ("lifecycle", "numeric", "cleanup"))
    permission = _copy_case_fixture(tmp_path, tmp_path, "run-1", "permission-rejection",
                                    ("lifecycle", "failure", "cleanup"))
    negative = _copy_case_fixture(tmp_path, tmp_path, "run-1", "negative-dependency",
                                  ("lifecycle", "failure", "cleanup"))
    return source, normal, permission, negative


def test_producer_binds_three_retained_case_outputs(tmp_path):
    source, normal, permission, negative = _prepare_fixture(tmp_path)
    base = tmp_path / "base.sif"
    base.write_bytes(b"base")
    app = tmp_path / "application-manifest.json"
    app.write_text("{}", encoding="utf-8")
    receipt = tmp_path / "host-gate.json"
    value = producer.build_manifest(
        receipt=receipt,
        source_seal=source,
        base_sif=base,
        application_manifest=app,
        run_id="campaign-1",
        application_name="/example/yolo",
        normal=normal,
        permission=permission,
        negative=negative,
    )
    assert value["schema"] == "tiger-yolo-host-minindn-manifest-v2"
    assert {row["case"] for row in value["cases"]} == {
        "normal", "permission-rejection", "negative-dependency"
    }
    assert receipt.is_file()


def test_producer_rejects_placement_failure_as_dependency_case(tmp_path):
    source, normal, permission, negative = _prepare_fixture(tmp_path)
    failure = negative / "negative-evidence.json"
    body = json.loads(failure.read_text(encoding="utf-8"))
    body.update(boundary="PLACEMENT_DECISION", reason="NO_FEASIBLE_CANDIDATE")
    failure.write_text(json.dumps(body), encoding="utf-8")
    base = tmp_path / "base.sif"
    base.write_bytes(b"base")
    app = tmp_path / "application-manifest.json"
    app.write_text("{}", encoding="utf-8")
    with pytest.raises(ValueError, match="HOST_GATE_FAILURE_BOUNDARY"):
        producer.build_manifest(
            receipt=tmp_path / "host-gate.json",
            source_seal=source,
            base_sif=base,
            application_manifest=app,
            run_id="campaign-1",
            application_name="/example/yolo",
            normal=normal,
            permission=permission,
            negative=negative,
        )
