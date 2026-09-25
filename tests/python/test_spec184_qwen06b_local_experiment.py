from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import struct
import sys
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py"


def load_module():
    spec = importlib.util.spec_from_file_location("spec184_qwen06b_local", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def load_native_module():
    spec = importlib.util.spec_from_file_location(
        "spec187_qwen06b_native_minindn",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("configured,expected", [
    (None, "*=WARN"), ("", "*=WARN"), ("*=ERROR:ndnsf.di.RuntimeEvidence=WARN",
                                           "*=ERROR:ndnsf.di.RuntimeEvidence=WARN"),
])
def test_native_environment_enables_required_evidence(monkeypatch, tmp_path,
                                                      configured, expected):
    module = load_native_module()
    monkeypatch.delenv("NDNSF_NDN_LOG", raising=False)
    if configured is not None:
        monkeypatch.setenv("NDNSF_NDN_LOG", configured)
    assert module.env_for(tmp_path, "ucla")["NDN_LOG"] == expected


def test_native_environment_collects_phase_timing_by_default(monkeypatch, tmp_path):
    module = load_native_module()
    monkeypatch.delenv("NDNSF_PHASE_TIMING", raising=False)
    assert module.env_for(tmp_path, "ucla")["NDNSF_PHASE_TIMING"] == "1"


def test_native_environment_honors_explicit_phase_timing_override(monkeypatch, tmp_path):
    module = load_native_module()
    monkeypatch.setenv("NDNSF_PHASE_TIMING", "0")
    assert module.env_for(tmp_path, "ucla")["NDNSF_PHASE_TIMING"] == "0"


def test_native_wait_aborts_on_prefixed_provider_terminal_record(tmp_path):
    module = load_native_module()
    provider_log = tmp_path / "provider.log"
    provider_log.write_text(
        "1790000000.000 WARN: [ndnsf.di.RuntimeEvidence] "
        "NDNSF_DI_PROVIDER_STAGE stage=TERMINAL status=failed reason=test-boundary\n")
    requester_log = tmp_path / "requester.log"
    requester_log.write_text("")
    process = SimpleNamespace(poll=lambda: None)
    with pytest.raises(RuntimeError, match="test-boundary"):
        module.wait_for_native_round(process, requester_log, [provider_log], 1)


@pytest.mark.parametrize("exit_code", [0, 7])
def test_native_wait_reaps_before_accepting_success_marker(monkeypatch, tmp_path, exit_code):
    module = load_native_module()
    request_log = tmp_path / "requester.log"
    request_log.write_text("NATIVE_REQUEST_SUCCEEDED\n")
    process = SimpleNamespace(returncode=None, polls=0)

    def poll():
        process.polls += 1
        if process.polls == 2:
            request_log.write_text(
                "NATIVE_REQUEST_SUCCEEDED\nNATIVE_CONVERSATION_CHECKPOINT_WRITTEN\n")
            process.returncode = exit_code
        return process.returncode

    process.poll = poll
    monkeypatch.setattr(module.time, "sleep", lambda _: None)
    text = module.wait_for_native_round(process, request_log, [], 1)
    assert process.polls == 2
    assert process.returncode == exit_code
    assert "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN" in text


def test_native_wait_does_not_hide_provider_failure_behind_success_marker(tmp_path):
    module = load_native_module()
    request_log = tmp_path / "requester.log"
    request_log.write_text("NATIVE_REQUEST_SUCCEEDED\n")
    provider_log = tmp_path / "provider.log"
    provider_log.write_text("NDNSF_DI_NATIVE_FAILURE terminal-finalize-failed\n")
    process = SimpleNamespace(poll=lambda: 0)
    with pytest.raises(RuntimeError, match="terminal-finalize-failed"):
        module.wait_for_native_round(process, request_log, [provider_log], 1)


def test_example_profile_resolves_two_nodes_and_installed_native_boundary():
    module = load_module()
    profile, profile_sha = module.load_profile(
        ROOT / "Experiments/profiles/ndnsf-di-qwen06b-local.example.json"
    )
    assert profile["buildDir"].endswith("build")
    assert profile["stageNodes"] == ["ucla", "arizona"]
    for key, binary in (
        ("controllerBinary", "App_ServiceController"),
        ("authorityBinary", "DI_NativeArtifactAuthority"),
        ("requesterBinary", "DI_NativeRequester"),
        ("providerBinary", "di-native-provider"),
        ("oracleBinary", "spec190-multiturn-oracle"),
    ):
        assert profile[key] == "/usr/local/bin/" + binary
    assert profile["assemblyWorkerBinary"] == (
        "/usr/local/libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker")
    assert profile_sha.startswith("sha256:")
    assert len(profile_sha) == len("sha256:") + 64


def test_generated_command_preserves_installed_paths_and_digests(tmp_path: Path):
    module = load_module()
    profile, _ = module.load_profile(
        ROOT / "Experiments/profiles/ndnsf-di-qwen06b-local.example.json")
    args = SimpleNamespace(
        stage_manifest=tmp_path / "manifest.json", stage_root=tmp_path,
        rounds=1, max_new_tokens=1025, require_multi_token=True,
        nlsr_wait_s=8, startup_timeout_s=60,
        input_token_ids="", delta_token_ids="", negative_parent=False,
        cache_dir=tmp_path / "shared-cache")
    names = ("controller", "authority", "requester", "provider", "assemblyWorker", "oracle")
    digest = "sha256:" + "a" * 64
    candidate = {
        "modelManifest": {"sha256": digest}, "tokenizer": {"sha256": digest},
        "topology": {"sha256": digest},
        "binaries": {name: {"sha256": "sha256:" + format(index, "064x")}
                     for index, name in enumerate(names, 1)},
    }
    command = module.command_for(args, profile, tmp_path / "run", candidate)
    for field, option in (
        ("controllerBinary", "--controller-binary"),
        ("authorityBinary", "--authority-binary"),
        ("requesterBinary", "--requester-binary"),
        ("providerBinary", "--provider-binary"),
        ("assemblyWorkerBinary", "--assembly-worker-binary"),
        ("oracleBinary", "--oracle-binary"),
    ):
        assert command.count(option) == 1
        assert command[command.index(option) + 1] == profile[field]
        name = field[:-len("Binary")]
        assert command[command.index(option + "-sha256") + 1] == candidate["binaries"][name]["sha256"]
    assert command[command.index("--stage-nodes") + 1] == "ucla,arizona"
    assert command[command.index("--model-family") + 1] == "qwen"
    assert command[command.index("--model-name") + 1] == ""
    assert command[command.index("--artifact-cache-root") + 1] == str(
        (tmp_path / "shared-cache").resolve())
    assert command[command.index("--max-new-tokens") + 1] == "1025"
    assert "--require-multi-token" in command
    assert "--direct-start" in command


def test_qwen_token_budget_contract_is_1025():
    module = load_module()
    native = load_native_module()
    assert module.MAX_NEW_TOKENS == 1025
    assert native.MAX_NEW_TOKENS == 1025


def test_artifact_cache_root_cannot_be_run_scoped(tmp_path: Path):
    module = load_module()
    run_root = tmp_path / "run" / "workload"
    with pytest.raises(ValueError, match="CACHE_ROOT_MUST_BE_OUTSIDE_RUN_ROOT"):
        module.resolve_artifact_cache_root(run_root / "cache", run_root)


def test_native_model_source_cache_reuses_exact_identity_only(tmp_path: Path):
    module = load_native_module()
    identity = module.model_source_cache_identity(
        "Qwen/Qwen3-0.6B", "sha256:" + "1" * 64, "sha256:" + "2" * 64,
        "sha256:" + "3" * 64, "sha256:" + "4" * 64,
        "sha256:" + "5" * 64, "sha256:" + "6" * 64)
    cache_root = tmp_path / "shared-cache"
    first, first_namespace = module.resolve_model_source_repository(cache_root, identity)
    second, second_namespace = module.resolve_model_source_repository(cache_root, identity)
    assert second == first
    assert second_namespace == first_namespace
    assert json.loads((first / "cache-identity.json").read_text()) == identity

    source = b"immutable canonical source"
    source_digest = module.digest_bytes(source)
    payload = first / "payloads" / "sha256" / source_digest[7:9] / source_digest[7:]
    payload.parent.mkdir(parents=True)
    payload.write_bytes(source)
    assert module.validate_model_source_repository(first, [(source_digest, len(source))])

    changed = dict(identity, initializerDigest="sha256:" + "7" * 64)
    changed_repo, changed_namespace = module.resolve_model_source_repository(
        cache_root, changed)
    assert changed_repo != first
    assert changed_namespace != first_namespace
    assert not module.validate_model_source_repository(
        changed_repo, [(source_digest, len(source))])


def test_profile_rejects_unknown_fields_before_execution(tmp_path: Path):
    module = load_module()
    profile = {
        "schema": module.PROFILE_SCHEMA,
        "profileId": "local-test",
        "topologyFile": "topology.conf",
        "stageNodes": ["one", "two"],
        "controllerNode": "controller",
        "userNode": "user",
        "buildDir": "build",
        "unexpected": True,
    }
    path = tmp_path / "profile.json"
    path.write_text(json.dumps(profile), encoding="utf-8")
    with pytest.raises(ValueError, match="PROFILE_UNKNOWN_FIELDS:unexpected"):
        module.load_profile(path)


def test_missing_binary_is_a_machine_preflight_failure(tmp_path: Path):
    module = load_module()
    result = module.binary_check(tmp_path / "missing-binary")
    assert result["status"] == "FAIL"
    assert result["reason"] == "MISSING_OR_NOT_EXECUTABLE"


def test_process_census_ignores_the_orchestrator_itself(tmp_path: Path):
    module = load_module()
    result = module.process_census(tmp_path / "isolated-run")
    assert result["status"] == "PASS"
    assert result["alive"] == []


def test_candidate_digest_is_canonical_and_changes_with_inputs():
    module = load_module()
    first = module.canonical_digest({"b": 2, "a": 1})
    same = module.canonical_digest({"a": 1, "b": 2})
    changed = module.canonical_digest({"a": 1, "b": 3})
    assert first == same
    assert first != changed
    assert first.startswith("sha256:")


def test_model_identity_normalizes_int8_without_hiding_activation_contract():
    module = load_module()
    identity = module.normalized_model_identity({
        "modelFamily": "qwen", "model": "Qwen/Qwen3-0.6B",
        "dtype": "float32", "modelFormat": "onnx", "quantization": "INT8",
    })
    assert identity["quantization"] == "int8"
    assert identity["quantizationSubtype"] == "weight_only_int8"
    assert identity["dtype"] == "float32"
    assert identity["modelFormat"] == "onnx"


def test_conversation_journal_quota_matches_native_limit():
    module = load_native_module()
    assert module.NATIVE_CONVERSATION_JOURNAL_MAX_BYTES == 64 * 1024 * 1024


def test_model_layer_requires_explicit_canonical_source():
    module = load_module()
    result = module.canonical_source_info(None, None)
    assert result == {
        "status": "WAITING_EXTERNAL_INPUT",
        "reason": "MODEL_CANONICAL_SOURCE_REQUIRED",
    }


def test_model_layer_rejects_non_onnx_source(tmp_path: Path):
    module = load_module()
    source = tmp_path / "source.onnx"
    source.write_text("stage metadata is not an ONNX protobuf", encoding="utf-8")
    result = module.canonical_source_info(source, None)
    assert result["status"] == "FAIL"
    assert result["reason"] == "MODEL_CANONICAL_SOURCE_NOT_ONNX"


def test_model_layer_accepts_small_canonical_onnx_fixture():
    module = load_module()
    result = module.canonical_source_info(
        ROOT / "tests/fixtures/spec182/qwen-native-config.onnx", None)
    assert result["status"] == "PASS"
    assert result["nodeCount"] > 0


def test_canonical_source_summary_cache_avoids_reparsing_same_content(monkeypatch, tmp_path):
    module = load_module()
    source = ROOT / "tests/fixtures/spec182/qwen-native-config.onnx"
    cache_root = tmp_path / "cache"
    first = module.canonical_source_info(source, None, cache_root)
    assert first["status"] == "PASS"

    class FailingOnnx:
        @staticmethod
        def load(*_args, **_kwargs):
            raise AssertionError("cached canonical summary should avoid ONNX parsing")

    monkeypatch.setitem(sys.modules, "onnx", FailingOnnx)
    second = module.canonical_source_info(source, None, cache_root)
    assert second == first


def test_real_initializer_budget_matches_local_qwen_artifact():
    module = load_module()
    assert module.MODEL_SOURCE_MAX_BYTES >= 2 * 1024 * 1024 * 1024


def test_node_mapping_requires_complete_semantic_keys(tmp_path: Path):
    module = load_module()
    mapping = tmp_path / "mapping.json"
    mapping.write_text(json.dumps({"mapping": {"embedding": [0]}}), encoding="utf-8")
    stages = [{"layerRange": {"start": 0, "endExclusive": 1}}]
    result = module.node_mapping_info(mapping, stages)
    assert result["status"] == "FAIL"
    assert result["reason"] == "MODEL_NODE_MAPPING_COVER_INVALID"


def test_native_qwen_state_successors_are_name_bound_not_positional():
    module = load_native_module()
    stages = [{
        "layerRange": {"start": 0, "endExclusive": 1},
        "cacheInputs": ["past_value.0", "past_key.0"],
        "cacheOutputs": ["present_key.0", "present_value.0"],
    }]
    assert module.qwen_state_successor_pairs(stages) == [
        ("past_key.0", "present_key.0"),
        ("past_value.0", "present_value.0"),
    ]


def test_native_qwen_state_contract_maps_semantic_names_to_exported_onnx_names():
    module = load_native_module()
    assert module.canonical_qwen_state_successor_pairs([{
        "layerRange": {"start": 0, "endExclusive": 1},
        "cacheInputs": ["past_key.0", "past_value.0"],
        "cacheOutputs": ["present_key.0", "present_value.0"],
    }]) == [
        ("past_key_values.0.key", "present.0.key"),
        ("past_key_values.0.value", "present.0.value"),
    ]
    assert module.canonical_qwen_state_name("past_key_values.0.key") == (
        "past_key_values.0.key")


@pytest.mark.parametrize("name", ["past_key_values.0", "present_key.0.tensor", "hidden_states"])
def test_native_qwen_state_contract_rejects_unknown_source_mapping_name(name):
    module = load_native_module()
    with pytest.raises(ValueError, match="QWEN_CANONICAL_STATE_NAME_INVALID"):
        module.canonical_qwen_state_name(name)


def test_native_qwen_state_successors_reject_mismatched_layer():
    module = load_native_module()
    stages = [{
        "layerRange": {"start": 0, "endExclusive": 1},
        "cacheInputs": ["past_key.0", "past_value.0"],
        "cacheOutputs": ["present_key.0", "present_value.1"],
    }]
    with pytest.raises(ValueError, match="QWEN_STATE_LAYER_COVER_INVALID"):
        module.qwen_state_successor_pairs(stages)


def test_native_qwen_tensor_bundle_declares_one_int64_tensor():
    module = load_native_module()
    payload = module.tensor_bundle([7, 8])
    type_offset = 8 + 4 + 4 + len(b"input_ids")
    assert struct.unpack_from("<I", payload, type_offset)[0] == 3  # Int64
    rank_offset = type_offset + 4
    assert struct.unpack_from("<I", payload, rank_offset)[0] == 2
    assert struct.unpack_from("<q", payload, rank_offset + 4)[0] == 1
    assert struct.unpack_from("<q", payload, rank_offset + 12)[0] == 2
    size_offset = rank_offset + 20
    assert struct.unpack_from("<Q", payload, size_offset)[0] == 16
    assert payload[size_offset + 8:] == struct.pack("<qq", 7, 8)


def test_native_qwen_default_inputs_use_the_pinned_chat_template_fixture():
    module = load_native_module()
    assert module.DEFAULT_QWEN_INPUT_TOKEN_IDS == (
        151644, 872, 198, 9707, 151645, 198, 151644, 77091, 198,
        151667, 271, 151668, 271,
    )
    assert module.DEFAULT_QWEN_DELTA_TOKEN_IDS == (
        198, 151644, 872, 198, 45764, 23811, 1549, 13, 151645, 198,
        151644, 77091, 198, 151667, 271, 151668, 271,
    )
    assert module.DEFAULT_QWEN_INPUT_TOKEN_IDS != (1,)
    assert module.DEFAULT_QWEN_DELTA_TOKEN_IDS != (0,)


def test_native_materialize_input_uses_hardlink_for_readonly_source(tmp_path: Path):
    module = load_native_module()
    source = tmp_path / "source.bin"
    destination = tmp_path / "run" / "source.bin"
    source.write_bytes(b"immutable model source")
    source.chmod(0o444)
    result = module.materialize_input(source, destination, module.digest_file(source), "TEST")
    assert result["method"] == "hardlink"
    assert destination.stat().st_ino == source.stat().st_ino


def test_native_materialize_input_rejects_mutable_source(tmp_path: Path):
    module = load_native_module()
    source = tmp_path / "source.bin"
    source.write_bytes(b"mutable model source")
    with pytest.raises(RuntimeError, match="TEST_SOURCE_NOT_IMMUTABLE"):
        module.materialize_input(source, tmp_path / "run" / "source.bin", None, "TEST")


def test_native_encrypted_repository_path_is_empty_and_external(tmp_path: Path):
    module = load_native_module()
    run_root = tmp_path / "run"
    run_root.mkdir()
    external = tmp_path / "tmpfs" / "encrypted-repo"
    resolved = module.resolve_encrypted_repository_path(run_root, external)
    assert resolved == external.resolve()
    assert resolved.is_dir()
    assert resolved.stat().st_mode & 0o777 == 0o700

    (resolved / "leftover").write_bytes(b"old run")
    with pytest.raises(RuntimeError, match="ENCRYPTED_REPOSITORY_PATH_BUSY"):
        module.resolve_encrypted_repository_path(run_root, external)


def test_native_encrypted_repository_path_cannot_be_inside_run_root(tmp_path: Path):
    module = load_native_module()
    run_root = tmp_path / "run"
    run_root.mkdir()
    with pytest.raises(RuntimeError, match="ENCRYPTED_REPOSITORY_PATH_MUST_BE_OUTSIDE_RUN_ROOT"):
        module.resolve_encrypted_repository_path(run_root, run_root / "external")


def test_native_digest_gate_requires_expected_identity(tmp_path: Path):
    module = load_native_module()
    source = tmp_path / "source.bin"
    source.write_bytes(b"candidate")
    with pytest.raises(SystemExit, match="TEST_DIGEST_REQUIRED"):
        module.require_file_digest(source, None, "TEST")


def test_runtime_failure_is_classified_after_startup_markers(tmp_path: Path):
    module = load_module()
    (tmp_path / "controller.log").write_text(
        "ServiceController started...\n", encoding="utf-8")
    (tmp_path / "authority.log").write_text(
        "NATIVE_GRANT_AUTHORITY_READY\n", encoding="utf-8")
    for index in range(3):
        (tmp_path / f"provider-{index}.log").write_text(
            "NDNSF_DI_NATIVE_PROVIDER_READY\n", encoding="utf-8")
    (tmp_path / "requester-0.log").write_text(
        "NATIVE_REQUESTER_FAILED: DI_NATIVE_ONNX_PARSE\n", encoding="utf-8")
    assert module.startup_markers_observed(tmp_path)
    assert module.first_failure_marker(tmp_path) == "DI_NATIVE_ONNX_PARSE"


def test_runtime_failure_without_controller_marker_is_not_startup_complete(tmp_path: Path):
    module = load_module()
    (tmp_path / "authority.log").write_text(
        "NATIVE_GRANT_AUTHORITY_READY\n", encoding="utf-8")
    for index in range(3):
        (tmp_path / f"provider-{index}.log").write_text(
            "NDNSF_DI_NATIVE_PROVIDER_READY\n", encoding="utf-8")
    assert not module.startup_markers_observed(tmp_path)


def test_prepared_bundle_rejects_app_manifest_mutation(tmp_path: Path):
    module = load_module()
    run_dir = tmp_path / "run"
    run_dir.mkdir()
    app_manifest = run_dir / "app-manifest.json"
    app_manifest.write_text('{"candidate": "one"}\n', encoding="utf-8")
    command = ["runner", "--fixed"]
    bundle = {
        "candidateDigest": "sha256:candidate",
        "commandDigest": module.canonical_digest(command),
        "appManifest": {"path": str(app_manifest),
                         "sha256": module.sha256_file(app_manifest)},
        "status": "PASS",
    }
    (run_dir / "bundle-manifest.json").write_text(
        json.dumps(bundle, sort_keys=True) + "\n", encoding="utf-8")
    launch = {"candidateDigest": bundle["candidateDigest"],
              "bundleDigest": module.canonical_digest(bundle)}
    app_manifest.write_text('{"candidate": "two"}\n', encoding="utf-8")
    with pytest.raises(SystemExit, match="BUNDLE_APP_MANIFEST_CHANGED"):
        module.validate_prepared_bundle(run_dir, launch, command)
