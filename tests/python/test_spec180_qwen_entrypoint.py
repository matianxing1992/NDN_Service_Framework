from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_QwenAckDriven_Minindn.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("spec180_qwen_runner", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def load_pipeline():
    script = ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
    spec = importlib.util.spec_from_file_location("spec182_llm_pipeline", script)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _object(root: Path, name: str, content: bytes) -> dict[str, object]:
    path = root / name
    path.write_bytes(content)
    return {"path": name, "bytes": len(content), "sha256": digest(path)}


def valid_inputs(tmp_path: Path):
    module = load_runner()
    model_root = tmp_path / "models"
    model_root.mkdir()
    graph = _object(model_root, "graph.onnx", b"graph")
    initializer = _object(model_root, "weights.bin", b"weights")
    stages = [
        _object(model_root, f"stage-{index}.onnx", f"stage-{index}".encode())
        for index in range(3)
    ]
    tokenizer = model_root / "tokenizer"
    tokenizer.mkdir()
    (tokenizer / "tokenizer.json").write_text("{}", encoding="utf-8")
    (tokenizer / "tokenizer_config.json").write_text("{}", encoding="utf-8")
    stage_manifest = {
        "schema": "ndnsf-di-qwen36-stage-manifest-v1",
        "stages": [
            {**item, "role": f"/LLM/Pipeline/Stage/{index}"}
            for index, item in enumerate(stages)
        ],
    }
    manifest_path = tmp_path / "model-manifest.json"
    model_digest = "sha256:" + "a" * 64
    prompt = "hello from qwen"
    prompt_digest = "sha256:" + hashlib.sha256(prompt.encode()).hexdigest()
    manifest = {
        "schema": module.MANIFEST_SCHEMA,
        "modelFamily": module.MODEL_FAMILY,
        "model": {"id": module.MODEL_ID},
        "manifestRevision": "manifest-v1",
        "modelRevision": "revision-v1",
        "sourceRevision": "source-v1",
        "modelIdentityDigest": model_digest,
        "graph": graph,
        "initializers": [initializer],
        "stages": stages,
        "stageManifest": stage_manifest,
        "tokenizer": {"path": "tokenizer"},
        "chatTemplate": {"id": "chat-v1"},
        "stopPolicy": {"id": "stop-v1"},
        "runtime": {
            "backend": "onnxruntime-cuda",
            "executionProvider": "cuda",
            "cpuFallback": False,
        },
        "prompt": prompt,
        "signature": {"keyId": "test", "algorithm": "Ed25519", "valueB64": "x"},
    }
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return module, manifest_path, model_root, model_digest, prompt_digest, prompt


def test_missing_manifest_fails_before_model_enumeration(tmp_path: Path):
    module = load_runner()
    with pytest.raises(module.QwenInputError, match="MODEL_MANIFEST_MISSING"):
        module.validate_manifest(
            tmp_path / "missing.json", tmp_path,
            expected_model=module.MODEL_ID,
            expected_revision="revision-v1",
            expected_model_digest="sha256:" + "a" * 64,
            expected_prompt_digest="sha256:" + "b" * 64,
            verifier=lambda path: None,
        )


def test_invalid_backend_is_rejected_before_signature_or_execution(tmp_path: Path):
    module, manifest_path, model_root, model_digest, prompt_digest, _ = valid_inputs(tmp_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["runtime"]["executionProvider"] = "cpu"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    called = []
    with pytest.raises(module.QwenInputError, match="EXECUTION_PROVIDER_INVALID"):
        module.validate_manifest(
            manifest_path, model_root,
            expected_model=module.MODEL_ID,
            expected_revision="revision-v1",
            expected_model_digest=model_digest,
            expected_prompt_digest=prompt_digest,
            verifier=lambda path: called.append(path),
        )
    assert called == []


def test_valid_manifest_checks_objects_and_builds_fixed_delegate(tmp_path: Path):
    module, manifest_path, model_root, model_digest, prompt_digest, prompt = valid_inputs(tmp_path)
    validated = module.validate_manifest(
        manifest_path, model_root,
        expected_model=module.MODEL_ID,
        expected_revision="revision-v1",
        expected_model_digest=model_digest,
        expected_prompt_digest=prompt_digest,
        verifier=lambda path: None,
    )
    output = tmp_path / "evidence"
    output.mkdir()
    stage_manifest, document = module._legacy_stage_manifest(
        validated["manifest"], model_root, output)
    assert len(document["stages"]) == 3
    argv = module.build_delegate_argv(
        stage_manifest=stage_manifest,
        model_root=model_root,
        tokenizer_root=validated["tokenizerRoot"],
        output_root=output,
        model=module.MODEL_ID,
        revision="revision-v1",
        prompt=prompt,
        request_id="run-1",
        workload_digest="sha256:" + "b" * 64,
        model_digest=model_digest,
    )
    assert "--runtime" in argv and argv[argv.index("--runtime") + 1] == "qwen-onnx"
    assert argv[argv.index("--qwen-execution-provider") + 1] == "cuda"
    assert argv[argv.index("--qwen-device-ids") + 1] == "0,1,2"
    assert argv[argv.index("--selection-dataflow-v3") : argv.index("--selection-dataflow-v3") + 1] == [
        "--selection-dataflow-v3"
    ]
    assert "--spec175-case" not in argv
    assert argv[argv.index("--measured-requests") + 1] == "2"


def test_native_config_delegate_contract_selects_native_provider(tmp_path: Path):
    module, manifest_path, model_root, model_digest, prompt_digest, prompt = valid_inputs(tmp_path)
    validated = module.validate_manifest(
        manifest_path, model_root,
        expected_model=module.MODEL_ID,
        expected_revision="revision-v1",
        expected_model_digest=model_digest,
        expected_prompt_digest=prompt_digest,
        verifier=lambda path: None,
    )
    output = tmp_path / "evidence"
    output.mkdir()
    stage_manifest, _ = module._legacy_stage_manifest(
        validated["manifest"], model_root, output)
    config = tmp_path / "native-requester.json"
    config.write_text("{}", encoding="utf-8")
    argv = module.build_delegate_argv(
        stage_manifest=stage_manifest,
        model_root=model_root,
        tokenizer_root=validated["tokenizerRoot"],
        output_root=output,
        model=module.MODEL_ID,
        revision="revision-v1",
        prompt=prompt,
        request_id="run-native",
        workload_digest="sha256:" + "b" * 64,
        model_digest=model_digest,
        native_requester_config=str(config),
    )
    assert argv[argv.index("--runtime") + 1] == "qwen-onnx-cpu-native"
    assert "--selection-dataflow-v3" not in argv
    assert argv[argv.index("--native-requester-config") + 1] == str(config)

    pipeline = load_pipeline()
    parsed = pipeline.build_parser().parse_args([
        "--runtime", "qwen-onnx-cpu-native",
        "--native-requester-config", str(config),
    ])
    assert parsed.native_requester_config == str(config)
    source = (ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py").read_text(
        encoding="utf-8")
    assert "native_user_args +=" in source
    assert "--native-requester-config" in source
    assert "selection_bundle is not None and not args.native_requester_config" in source
    assert "selection_bundle is None or args.native_requester_config" in source


@pytest.mark.parametrize("field, value, error", [
    ("runtime", {"backend": "onnxruntime-cuda", "executionProvider": "cuda", "cpuFallback": True}, "CPU_FALLBACK_ENABLED"),
    ("modelFamily", "Qwen3.5-27B", "MODEL_FAMILY_MISMATCH"),
])
def test_manifest_identity_mutations_fail_closed(tmp_path: Path, field, value, error):
    module, manifest_path, model_root, model_digest, prompt_digest, _ = valid_inputs(tmp_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest[field] = value
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(module.QwenInputError, match=error):
        module.validate_manifest(
            manifest_path, model_root,
            expected_model=module.MODEL_ID,
            expected_revision="revision-v1",
            expected_model_digest=model_digest,
            expected_prompt_digest=prompt_digest,
            verifier=lambda path: None,
        )
