from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
PLAN_DIGEST = "sha256:" + "a" * 64


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_role_mode_is_frozen_into_native_runner_command():
    module = load_module(
        "spec194_local_experiment",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py")
    args = SimpleNamespace(
        stage_manifest=Path("/tmp/stages.json"), stage_root=Path("/tmp/stages"),
        rounds=4, max_new_tokens=1, nlsr_wait_s=1, startup_timeout_s=2,
        placement_strategy="native-observation-aware-v4", provider_role_mode="all",
        require_multi_token=False, cache_dir=None, canonical_source=None,
        canonical_initializer=None, node_mapping=None, input_token_ids="",
        delta_token_ids="", negative_parent=False)
    profile = {
        "topologyFile": "/tmp/topology.conf", "stageNodes": ["n0", "n1"],
        "controllerNode": "n2", "userNode": "n3", "buildDir": "/tmp/build",
        "controllerBinary": "/tmp/build/controller", "resourceLimits": {},
        "authorityBinary": "/tmp/build/authority", "requesterBinary": "/tmp/build/requester",
        "providerBinary": "/tmp/build/provider", "assemblyWorkerBinary": "/tmp/build/worker",
        "oracleBinary": "/tmp/build/oracle",
    }

    command = module.command_for(args, profile, Path("/tmp/run"))
    option = command.index("--provider-role-mode")
    assert command[option + 1] == "all"


def test_ack_timeout_override_is_frozen_into_native_runner_command():
    module = load_module(
        "spec194_local_experiment_ack_timeout",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py")
    args = SimpleNamespace(
        stage_manifest=Path("/tmp/stages.json"), stage_root=Path("/tmp/stages"),
        rounds=1, max_new_tokens=4, nlsr_wait_s=8, startup_timeout_s=90,
        placement_strategy="native-observation-aware-v4", provider_role_mode="stage",
        ack_timeout_ms=3000, require_multi_token=True, cache_dir=None,
        canonical_source=None, canonical_initializer=None, node_mapping=None,
        input_token_ids="", delta_token_ids="", negative_parent=False)
    profile = {
        "topologyFile": "/tmp/topology.conf", "stageNodes": ["n0", "n1"],
        "controllerNode": "n2", "userNode": "n3", "buildDir": "/tmp/build",
        "controllerBinary": "/tmp/build/controller", "resourceLimits": {},
        "authorityBinary": "/tmp/build/authority", "requesterBinary": "/tmp/build/requester",
        "providerBinary": "/tmp/build/provider", "assemblyWorkerBinary": "/tmp/build/worker",
        "oracleBinary": "/tmp/build/oracle",
    }

    command = module.command_for(args, profile, Path("/tmp/run"))
    option = command.index("--ack-timeout-ms")
    assert command[option + 1] == "3000"


def test_provider_role_argument_keeps_legacy_default_and_all_role_mode():
    module = load_module(
        "spec194_native_minindn_roles",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py")

    assert module.provider_role_argument("/stage/0", "stage") == "/stage/0"
    assert module.provider_role_argument("/stage/0", "all") == "all"
    with pytest.raises(ValueError, match="invalid provider role mode"):
        module.provider_role_argument("/stage/0", "unknown")


def test_controller_policy_grants_roles_consistent_with_provider_role_mode():
    module = load_module(
        "spec194_native_minindn_policy",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py")
    providers = ["/provider/0", "/provider/1"]
    stage_roles = ["/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1"]

    def provider_rules(policy: str, provider: str) -> str:
        return policy.split(f"  for {provider}\n  allow {{", 1)[1].split("\n }\n", 1)[0]

    stage_policy = module.policy_text(providers, stage_roles, "stage")
    all_policy = module.policy_text(providers, stage_roles, "all")

    assert "/AI/LLM/Pipeline/QwenNative/ROLE/LLM/Pipeline/Stage/0" in provider_rules(
        stage_policy, "/provider/0")
    assert "/AI/LLM/Pipeline/QwenNative/ROLE/LLM/Pipeline/Stage/1" not in provider_rules(
        stage_policy, "/provider/0")
    for provider in providers:
        rules = provider_rules(all_policy, provider)
        assert "/AI/LLM/Pipeline/QwenNative/ROLE/LLM/Pipeline/Stage/0" in rules
        assert "/AI/LLM/Pipeline/QwenNative/ROLE/LLM/Pipeline/Stage/1" in rules


def test_flexible_provider_barrier_records_actual_swapped_roles(tmp_path: Path, monkeypatch):
    module = load_module(
        "spec194_native_minindn_barrier",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py")
    paths = [tmp_path / "provider-0.log", tmp_path / "provider-1.log"]
    for path in paths:
        path.write_text("provider ready\n", encoding="utf-8")
    offsets = module.snapshot_provider_logs(paths)
    placement = {path: (f"/provider/{index}", f"/stage/{index}")
                 for index, path in enumerate(paths)}
    prefix = "1790044168.187586 WARN: [ndnsf.di.RuntimeEvidence] "

    def selection(provider: int, role: int, request: str = "/r") -> str:
        return (f"{prefix}NDNSF_DI_NATIVE_SELECTION_ACCEPTED requestId={request} "
                f"attemptEpoch=7 planDigest={PLAN_DIGEST} provider=/provider/{provider} "
                f"role=/stage/{role} manifestDigest=sha256:{'c' * 64}\n")

    def stage(provider: int, role: int, name: str, status: str = "observed") -> str:
        return (f"{prefix}NDNSF_DI_PROVIDER_STAGE stage={name} status={status} "
                f"requestId=/r provider=/provider/{provider} role=/stage/{role} "
                f"planDigest={PLAN_DIGEST} attemptEpoch=7\n")

    # An unrelated earlier turn is present in the captured append window.
    paths[0].write_text(
        "provider ready\n" + selection(0, 0, request="/prior") +
        selection(0, 1) + stage(0, 1, "EXECUTION_COMPLETED") +
        stage(0, 1, "TERMINAL"), encoding="utf-8")
    paths[1].write_text(
        "provider ready\n" + selection(1, 0) +
        stage(1, 0, "EXECUTION_COMPLETED"), encoding="utf-8")
    request = (f"NATIVE_REQUEST_SUCCEEDED request=/r plan={PLAN_DIGEST}\n"
               f"{prefix}NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId=/r attemptEpoch=7 "
               f"planDigest={PLAN_DIGEST}\n")
    monkeypatch.setattr(module.time, "monotonic", lambda: 0.0)

    result = module.wait_for_provider_round_barrier(
        request, offsets, placement, "/stage/1", 1.0,
        allow_role_reassignment=True, expected_roles=["/stage/0", "/stage/1"])

    assert result["roles"]["/stage/0"]["provider"] == "/provider/1"
    assert result["roles"]["/stage/1"] == {
        "provider": "/provider/0", "completionStatus": "observed",
        "terminalObserved": True,
    }


def _requester_log(strategy_line: str, snapshot_line: str = "") -> str:
    return strategy_line + "\n" + snapshot_line + ("\n" if snapshot_line else "")


PROVIDERS = ["/provider/0", "/provider/1"]


def _write_provider_config(log_root: Path, providers: list[str] = PROVIDERS) -> None:
    config = log_root / "requester/config.json"
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text(json.dumps({"request": {"provider_names": providers}}),
                      encoding="utf-8")
    (log_root / "native-qwen-execution-plan.json").write_text(json.dumps({
        "services": [{"roles": ["/stage/0", "/stage/1"],
                      "dependencies": [{"producers": ["/stage/0"],
                                        "consumers": ["/stage/1"]}]}]}),
        encoding="utf-8")


def _pair(source: str, destination: str) -> dict[str, str]:
    return {"src_provider": source, "dst_provider": destination}


def _accepted_snapshot(**overrides: str) -> str:
    fields = {
        "requestId": "/request/0",
        "status": "accepted",
        "snapshotDigest": PLAN_DIGEST,
        "capturedAtMs": "1000",
        "ageMs": "25",
        "expiresAtMs": "2000",
        "expiresInMs": "975",
        "providerOffers": "2",
        "providerRoster": json.dumps(PROVIDERS, separators=(",", ":")),
        "providerOfferProviders": json.dumps(PROVIDERS, separators=(",", ":")),
        "rttPairs": json.dumps([_pair(PROVIDERS[0], PROVIDERS[1]),
                                 _pair(PROVIDERS[1], PROVIDERS[0])],
                                separators=(",", ":")),
        "goodputEdges": json.dumps([_pair(PROVIDERS[0], PROVIDERS[1]),
                                     _pair(PROVIDERS[1], PROVIDERS[0])],
                                    separators=(",", ":")),
        "activeProbeRtt": "2",
        "activeProbeRttRows": json.dumps([
            {"reported_by": PROVIDERS[0], "src_provider": PROVIDERS[0],
             "dst_provider": PROVIDERS[1]},
            {"reported_by": PROVIDERS[1], "src_provider": PROVIDERS[1],
             "dst_provider": PROVIDERS[0]},], separators=(",", ":")),
        "activeProbeGoodput": "2",
        "activeProbeGoodputRows": json.dumps([
            {"reported_by": PROVIDERS[1], "src_provider": PROVIDERS[0],
             "dst_provider": PROVIDERS[1]},
            {"reported_by": PROVIDERS[0], "src_provider": PROVIDERS[1],
             "dst_provider": PROVIDERS[0]},], separators=(",", ":")),
        "ineligibleLinkObservations": "0",
    }
    fields.update(overrides)
    return " ".join(f"{key}={value}" for key, value in fields.items())


def test_v4_strategy_requires_fresh_snapshot_with_roster_and_probe_rows(tmp_path: Path):
    module = load_module(
        "spec194_local_snapshot_evidence",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py")
    log = tmp_path / "requester-0.log"
    _write_provider_config(tmp_path)
    log.write_text(_requester_log(
        "NATIVE_REQUEST_PLACEMENT strategy=native-observation-aware-v4",
        "NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT " + _accepted_snapshot()),
        encoding="utf-8")

    result = module.validate_placement_strategy_identity(
        tmp_path, "native-observation-aware-v4", 1, expected_provider_offers=2)

    assert result["status"] == "PASS"
    assert result["observations"][0]["placementObservationSnapshot"]["ageMs"] == "25"
    assert json.loads(result["observations"][0]["placementObservationSnapshot"][
        "activeProbeGoodputRows"])[0]["dst_provider"] == PROVIDERS[1]


def test_v4_accepts_only_the_requested_goodput_direction(tmp_path: Path):
    module = load_module(
        "spec196_local_snapshot_scoped_goodput",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py")
    _write_provider_config(tmp_path)
    goodput_rows = [{"reported_by": PROVIDERS[1],
                     "src_provider": PROVIDERS[0],
                     "dst_provider": PROVIDERS[1]}]
    log = tmp_path / "requester-0.log"
    log.write_text(_requester_log(
        "NATIVE_REQUEST_PLACEMENT strategy=native-observation-aware-v4",
        "NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT " +
        _accepted_snapshot(
            activeProbeRtt="0", activeProbeRttRows="[]",
            activeProbeGoodput="1",
            activeProbeGoodputRows=json.dumps(goodput_rows,
                                              separators=(",", ":")))),
        encoding="utf-8")

    result = module.validate_placement_strategy_identity(
        tmp_path, "native-observation-aware-v4", 1, expected_provider_offers=2)

    assert result["status"] == "PASS"
    snapshot = result["observations"][0]["placementObservationSnapshot"]
    assert json.loads(snapshot["activeProbeGoodputRows"]) == goodput_rows
    assert json.loads(snapshot["activeProbeRttRows"]) == []


@pytest.mark.parametrize(
    ("snapshot", "reason"),
    [
        ("", "SNAPSHOT_EVENT_COUNT"),
        ("requestId=/r status=unavailable snapshotDigest=none", "SNAPSHOT_NOT_ACCEPTED"),
        ("requestId=/r status=accepted snapshotDigest=sha256:bad "
         "capturedAtMs=1000 ageMs=25 expiresAtMs=2000 expiresInMs=975 "
         "providerOffers=2 activeProbeRtt=1 activeProbeGoodput=1",
         "SNAPSHOT_DIGEST_INVALID"),
    ],
)
def test_v4_strategy_rejects_missing_or_unusable_snapshot(tmp_path: Path,
                                                          snapshot: str,
                                                          reason: str):
    module = load_module(
        "spec194_local_snapshot_reject_" + reason.lower(),
        ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py")
    _write_provider_config(tmp_path)
    log = tmp_path / "requester-0.log"
    snapshot_line = ("NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT " + snapshot
                     if snapshot else "")
    log.write_text(_requester_log(
        "NATIVE_REQUEST_PLACEMENT strategy=native-observation-aware-v4",
        snapshot_line), encoding="utf-8")

    result = module.validate_placement_strategy_identity(
        tmp_path, "native-observation-aware-v4", 1, expected_provider_offers=2)

    assert result["status"] == "FAIL"
    assert result["reason"] == reason


@pytest.mark.parametrize(
    ("overrides", "reason"),
    [
        ({"providerRoster": json.dumps([PROVIDERS[0], "/provider/rogue"],
                                        separators=(",", ":"))},
         "SNAPSHOT_PROVIDER_ROSTER_MISMATCH"),
        ({"providerOfferProviders": json.dumps([PROVIDERS[0], "/provider/rogue"],
                                                separators=(",", ":"))},
         "SNAPSHOT_OFFER_PROVIDER_MISMATCH"),
        ({"providerRoster": "truncated"}, "SNAPSHOT_SUMMARY_INVALID"),
        ({"goodputEdges": json.dumps([_pair(PROVIDERS[1], PROVIDERS[0])],
                                      separators=(",", ":"))},
         "SNAPSHOT_PROBE_ALLOWLIST_INVALID"),
        ({"expiresInMs": "0"}, "SNAPSHOT_FRESHNESS_INVALID"),
        ({"activeProbeRtt": "0", "activeProbeRttRows": "[]",
          "activeProbeGoodput": "0", "activeProbeGoodputRows": "[]"},
         "SNAPSHOT_ACTIVE_PROBE_ROWS_INCOMPLETE"),
        ({"activeProbeRttRows": json.dumps([{
            "reported_by": PROVIDERS[1], "src_provider": PROVIDERS[0],
            "dst_provider": PROVIDERS[1]},], separators=(",", ":"))},
         "SNAPSHOT_ACTIVE_PROBE_ROWS_INCOMPLETE"),
    ],
)
def test_v4_snapshot_validates_roster_graph_rows_and_freshness(
        tmp_path: Path, overrides: dict[str, str], reason: str):
    module = load_module(
        "spec194_snapshot_delta_" + reason.lower() + str(len(overrides)),
        ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py")
    _write_provider_config(tmp_path)
    log = tmp_path / "requester-0.log"
    log.write_text(_requester_log(
        "NATIVE_REQUEST_PLACEMENT strategy=native-observation-aware-v4",
        "NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT " +
        _accepted_snapshot(**overrides)), encoding="utf-8")

    result = module.validate_placement_strategy_identity(
        tmp_path, "native-observation-aware-v4", 1, expected_provider_offers=2)

    assert result["status"] == "FAIL"
    assert result["reason"] == reason


def test_v4_provider_env_forces_runtime_snapshot_evidence(monkeypatch, tmp_path: Path):
    module = load_module(
        "spec194_native_minindn_snapshot_env",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py")
    monkeypatch.setenv("NDNSF_DI_RUNTIME_TIMING", "0")

    v4_env = module.env_for(tmp_path, "requester", require_snapshot_evidence=True)
    diagnostic_env = module.env_for(tmp_path, "requester")

    assert v4_env["NDNSF_DI_RUNTIME_TIMING"] == "1"
    assert diagnostic_env["NDNSF_DI_RUNTIME_TIMING"] == "0"


def test_v4_snapshot_requires_configured_provider_roster(tmp_path: Path):
    module = load_module(
        "spec194_snapshot_missing_provider_config",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py")
    log = tmp_path / "requester-0.log"
    log.write_text(_requester_log(
        "NATIVE_REQUEST_PLACEMENT strategy=native-observation-aware-v4",
        "NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT " + _accepted_snapshot()),
        encoding="utf-8")

    result = module.validate_placement_strategy_identity(
        tmp_path, "native-observation-aware-v4", 1, expected_provider_offers=2)

    assert result["status"] == "FAIL"
    assert result["reason"] == "CONFIGURED_PROVIDER_ROSTER_INVALID"


def test_binary_preflight_records_resolved_library_hashes(monkeypatch, tmp_path: Path):
    module = load_module(
        "spec194_binary_library_identity",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py")
    binary = tmp_path / "requester"
    binary.write_bytes(b"binary")
    binary.chmod(0o755)
    shared_library = tmp_path / "libndnsf-distributed-inference.so"
    shared_library.write_bytes(b"ndnsf-library")
    loader = tmp_path / "ld-linux-x86-64.so.2"
    loader.write_bytes(b"loader")

    def fake_run(command, **kwargs):
        if command[0] == "/usr/bin/readelf":
            return SimpleNamespace(returncode=0, stdout="", stderr="")
        assert command[:2] == ["/usr/bin/ldd", "-r"]
        output = (f"libndnsf-distributed-inference.so => {shared_library} (0x1)\n"
                  f"{loader} (0x2)\n")
        return SimpleNamespace(returncode=0, stdout=output, stderr="")

    monkeypatch.setattr(module.subprocess, "run", fake_run)
    result = module.binary_check(binary)

    assert result["status"] == "PASS"
    libraries = {item["name"]: item for item in result["sharedLibraries"]}
    assert libraries[shared_library.name]["path"] == str(shared_library.resolve())
    assert libraries[shared_library.name]["sha256"] == module.sha256_file(shared_library)
    assert libraries[loader.name]["sha256"] == module.sha256_file(loader)


def test_binary_preflight_rejects_unresolved_shared_library(monkeypatch, tmp_path: Path):
    module = load_module(
        "spec194_binary_library_missing",
        ROOT / "Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py")
    binary = tmp_path / "requester"
    binary.write_bytes(b"binary")
    binary.chmod(0o755)

    def fake_run(command, **kwargs):
        if command[0] == "/usr/bin/readelf":
            return SimpleNamespace(returncode=0, stdout="", stderr="")
        return SimpleNamespace(returncode=0,
                               stdout="libndnsf-distributed-inference.so => not found\n",
                               stderr="")

    monkeypatch.setattr(module.subprocess, "run", fake_run)
    result = module.binary_check(binary)

    assert result["status"] == "FAIL"
    assert result["reason"] == "ABI_OR_LIBRARY_CLOSURE"
    assert result["unresolved"] == ["libndnsf-distributed-inference.so => not found"]
