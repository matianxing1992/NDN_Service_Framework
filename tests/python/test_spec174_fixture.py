from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys

import jsonschema
import pytest


REPO = Path(__file__).resolve().parents[2]
GENERATOR = REPO / "tests/fixtures/spec174/generate_fixture.py"
SCHEMA = REPO / "tests/fixtures/spec174/evidence-schema.json"


def _digest_tree(root: Path) -> dict[str, str]:
    result = {}
    for path in sorted(path for path in root.rglob("*") if path.is_file()):
        result[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def _generate(root: Path) -> None:
    subprocess.run([sys.executable, str(GENERATOR), str(root)], check=True)


def test_spec174_fixture_regeneration_is_byte_deterministic(tmp_path):
    first = tmp_path / "first"
    second = tmp_path / "second"
    _generate(first)
    _generate(second)
    assert _digest_tree(first) == _digest_tree(second)
    manifest = json.loads((first / "fixture-manifest.json").read_text())
    assert manifest["schemaVersion"] == "spec174-fixture-v1"
    assert manifest["hybrid"]["cuts"] == [1, 2, 1]
    assert len(manifest["faultSchedule"]) == 10


def test_spec174_unsplit_cpu_oracle_passes_in_three_fresh_processes(tmp_path):
    root = tmp_path / "fixture"
    _generate(root)
    program = """
import numpy as np
import onnxruntime as ort
import sys
session = ort.InferenceSession(sys.argv[1], providers=["CPUExecutionProvider"])
actual = session.run(None, {"x": np.load(sys.argv[2])})[0]
expected = np.load(sys.argv[3])
assert np.array_equal(actual, expected), (actual, expected)
"""
    for _ in range(3):
        subprocess.run([
            sys.executable, "-c", program,
            str(root / "canonical/unsplit.onnx"),
            str(root / "input.npy"),
            str(root / "oracle.npy"),
        ], check=True)


def test_spec174_evidence_schema_rejects_secrets_and_hidden_transport():
    schema = json.loads(SCHEMA.read_text())
    jsonschema.Draft202012Validator.check_schema(schema)
    valid = {
        "schemaVersion": "spec174-evidence-v1",
        "source": {
            "revision": "a" * 40,
            "tree": "b" * 40,
            "manifestSha256": "sha256:" + "c" * 64,
        },
        "fixture": {
            "manifestSha256": "sha256:" + "d" * 64,
            "oracleSha256": "sha256:" + "e" * 64,
            "configSha256": "sha256:" + "f" * 64,
        },
        "run": {"case": "pipeline", "processId": "p0", "status": "PASS", "firstFailure": None},
        "lifecycle": [{"event": "RESPONSE_ACCEPTED"}],
        "completeRows": [],
        "negativeRows": [],
        "transport": {"hiddenTransport": False, "cpuFallback": False, "namesObserved": True},
    }
    jsonschema.validate(valid, schema)
    secret = dict(valid)
    secret["plaintext"] = "must-not-appear"
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(secret, schema)
    hidden = json.loads(json.dumps(valid))
    hidden["transport"]["hiddenTransport"] = True
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(hidden, schema)
