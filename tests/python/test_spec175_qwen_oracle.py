from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import types


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = (
    ROOT / "specs/175-ndnsf-di-streamed-invocation/jobs/"
    "generate-qwen-onnx-reference.py"
)
WORKLOAD = ROOT / "packaging/ndnsf-di-container/jobs/spec175/workload.json"


def _load():
    spec = importlib.util.spec_from_file_location("spec175_qwen_oracle", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_aggregate_oracle_binds_frozen_p1_p2_workload(monkeypatch, tmp_path: Path) -> None:
    module = _load()

    class Session:
        @staticmethod
        def get_providers():
            return ["CUDAExecutionProvider", "CPUExecutionProvider"]

    monkeypatch.setattr(module, "make_session", lambda *_args, **_kwargs: Session())
    calls = []

    def generate(_sessions, _stages, **kwargs):
        calls.append(list(kwargs["prompt_ids"]))
        return [100 + len(calls), 248046], [1.0, 2.0]

    monkeypatch.setattr(module, "generate_sequence", generate)

    class Tokenizer:
        @staticmethod
        def from_file(_path):
            return Tokenizer()

        @staticmethod
        def decode(values, skip_special_tokens=True):
            assert skip_special_tokens
            return "reference " + " ".join(str(value) for value in values)

    monkeypatch.setitem(
        sys.modules, "tokenizers", types.SimpleNamespace(Tokenizer=Tokenizer))

    artifact_root = tmp_path / "model"
    tokenizer_dir = artifact_root / "qwen-onnx-tokenizer"
    tokenizer_dir.mkdir(parents=True)
    (tokenizer_dir / "tokenizer.json").write_text("{}", encoding="utf-8")
    service = {
        "model": "Qwen/Qwen3.6-27B",
        "modelRevision": "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        "modelType": "qwen3_5_text",
        "dtype": "float16",
        "contextLength": 256,
        "stages": [
            {"stageIndex": index, "path": f"stage-{index}.onnx"}
            for index in range(3)
        ],
    }
    service_path = tmp_path / "service.json"
    service_path.write_text(json.dumps(service), encoding="utf-8")
    output = tmp_path / "oracle.json"
    monkeypatch.setattr(sys, "argv", [
        str(SCRIPT),
        "--service-manifest", str(service_path),
        "--artifact-root", str(artifact_root),
        "--workload", str(WORKLOAD),
        "--output", str(output),
        "--max-new-tokens", "64",
        "--execution-provider", "cuda",
    ])

    assert module.main() == 0
    document = json.loads(output.read_text(encoding="utf-8"))
    assert document["schemaVersion"] == "ndnsf-di-spec175-g5-oracle-v1"
    assert document["executionProvider"] == "CUDAExecutionProvider"
    assert [item["promptId"] for item in document["prompts"]] == ["P1", "P2"]
    assert [item["referenceGeneratedTokenIds"] for item in document["prompts"]] == [
        [101, 248046], [102, 248046]
    ]
    assert all(item["decodedTextBytes"] > 0 for item in document["prompts"])
    workload = json.loads(WORKLOAD.read_text(encoding="utf-8"))
    assert calls == [prompt["inputTokenIds"] for prompt in workload["prompts"]]
