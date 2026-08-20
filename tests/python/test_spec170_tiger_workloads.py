from __future__ import annotations

import importlib.util
import hashlib
import json
from pathlib import Path

import pytest

from ndnsf.service import CollaborationContext


ROOT = Path(__file__).resolve().parents[2]
WORKLOADS = ROOT / "specs/170-reusable-layer-artifacts/jobs/workloads"
ARTIFACTS = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/native_di_tracer/artifacts"
)


def test_python_collaboration_assignment_preserves_native_role_provider_map() -> None:
    class NativeAssignment:
        role = "/D2A/Independent/0"
        service = "/Inference/Spec170LocalTwoGpu"
        assigned_artifact = ""
        artifact_data_name = ""
        requires_provisioning = False
        provisioning_timeout_ms = 0
        selection_digest = "sha256:" + "1" * 64
        assignment_payload = b'{"schema":"DI_PLACEMENT_V3"}'
        role_providers = {
            "/D2A/Independent/0": "/NDNSF-DI/Tracer/provider/local-two-gpu",
            "/D2A/LocalGroup#0": "/NDNSF-DI/Tracer/provider/local-two-gpu",
        }

    class NativeContext:
        assignment = NativeAssignment()

    assignment = CollaborationContext(NativeContext()).assignment

    assert assignment.assignment_payload.startswith(b'{"schema"')
    assert assignment.role_providers == NativeAssignment.role_providers


def test_d2_network_workloads_are_immutable_bundle_local_inputs() -> None:
    expected = (
        "d2a-local-two-gpu.sh",
        "d2b-cross-provider.sh",
        "d2h-hybrid.sh",
        "spec170_v3_local_two_gpu_provider.py",
        "spec170_v3_local_two_gpu_user.py",
        "spec170_v3_cross_provider_provider.py",
        "spec170_v3_cross_provider_user.py",
        "spec170_v3_hybrid_provider.py",
        "spec170_v3_hybrid_user.py",
    )
    missing = [name for name in expected if not (WORKLOADS / name).is_file()]
    assert missing == [], f"missing tracked Tiger workload inputs: {missing}"

    for name in ("d2a-local-two-gpu.sh", "d2b-cross-provider.sh", "d2h-hybrid.sh"):
        text = (WORKLOADS / name).read_text(encoding="utf-8")
        assert 'dirname -- "$0"' in text
        assert "/release/spec170-runtime-*" not in text
        assert 'cd "$BUNDLE"' in text
        assert "nfd_pid=$!" in text
        assert "kill -0 \"$nfd_pid\"" in text
        assert "seq 1 600" in text

    for name in (
        "spec170_v3_local_two_gpu_user.py",
        "spec170_v3_cross_provider_user.py",
        "spec170_v3_hybrid_user.py",
    ):
        text = (WORKLOADS / name).read_text(encoding="utf-8")
        assert text.index("user.start()") < text.index(
            "deadline_ms = int(time.time() * 1000) + args.timeout_ms")
        assert "payloadBytes={len(bytes(candidate.payload))}" in text

    hybrid = (WORKLOADS / "d2h-hybrid.sh").read_text(encoding="utf-8")
    assert "--artifact-root /artifacts" not in hybrid
    assert 'artifact_root="$BUNDLE/artifacts"' in hybrid
    assert 'test -d "$artifact_root"' in hybrid
    assert 'policy_file="$BUNDLE/controller.policies"' in hybrid
    assert 'controller-$mapping.policies' not in hybrid


def test_hybrid_user_requires_the_final_prediction_to_match_the_onnx_oracle() -> None:
    path = WORKLOADS / "spec170_v3_hybrid_user.py"
    spec = importlib.util.spec_from_file_location("spec170_v3_hybrid_user", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    expected = [-0.00012, 0.00002, -0.00004, 0.0]
    passed = module.evaluate_numeric_oracle(ARTIFACTS, "121", expected)
    failed = module.evaluate_numeric_oracle(
        ARTIFACTS, "212", [expected[0] + 0.01, *expected[1:]]
    )

    assert passed["status"] == "PASS"
    assert passed["maxAbsoluteError"] <= passed["absoluteTolerance"]
    assert failed["status"] == "FAIL"
    assert failed["maxAbsoluteError"] > failed["absoluteTolerance"]


def test_d2h_provider_uses_current_request_scoped_collaboration_api() -> None:
    provider = (WORKLOADS / "spec170_v3_hybrid_provider.py").read_text(
        encoding="utf-8")

    assert "context.allow_data(" in provider
    assert "context.publish(" in provider
    assert "context.wait_one(" in provider
    assert "context.publish_ndnsf_data_v1" not in provider
    assert "context.fetch_ndnsf_data_v1" not in provider
    assert "D2H_DATA_BINDING_MISMATCH" in provider
    assert "manifestDigest=" in provider
    assert "local_payloads" in provider
    assert "source={source}" in provider


def test_d2a_plan_binds_independent_roles_and_two_local_ranks_to_two_gpus() -> None:
    path = WORKLOADS / "spec170_v3_local_two_gpu_user.py"
    spec = importlib.util.spec_from_file_location("spec170_v3_local_two_gpu_user", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    provider = "/NDNSF-DI/Tracer/provider/local-two-gpu"
    roles, provider_by_role = module.build_d2a_role_contract(provider)
    role_counts = {
        role.role: sum(other.role == role.role for other in roles)
        for role in roles
    }
    keyed = {
        (role.role if role_counts[role.role] == 1 else f"{role.role}#{role.rank}"): role
        for role in roles
    }

    assert tuple(keyed) == (
        "/D2A/Independent/0",
        "/D2A/Independent/1",
        "/D2A/LocalGroup#0",
        "/D2A/LocalGroup#1",
    )
    assert keyed["/D2A/Independent/0"].device_set == ("cuda:0",)
    assert keyed["/D2A/Independent/1"].device_set == ("cuda:1",)
    assert keyed["/D2A/LocalGroup#0"].device_set == ("cuda:0",)
    assert keyed["/D2A/LocalGroup#1"].device_set == ("cuda:1",)
    assert keyed["/D2A/LocalGroup#0"].role == keyed["/D2A/LocalGroup#1"].role
    assert {keyed[key].rank for key in (
        "/D2A/LocalGroup#0", "/D2A/LocalGroup#1"
    )} == {0, 1}
    assert provider_by_role == {key: provider for key in keyed}


def test_d2a_provider_never_publishes_a_partial_local_group() -> None:
    path = WORKLOADS / "spec170_v3_local_two_gpu_provider.py"
    spec = importlib.util.spec_from_file_location(
        "spec170_v3_local_two_gpu_provider", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    independent = {
        "/D2A/Independent/0": {"device": "cuda:0", "values": [1.0, 2.0]},
        "/D2A/Independent/1": {"device": "cuda:1", "values": [3.0, 4.0]},
    }
    completed = module.build_d2a_final_result(
        provider="/NDNSF-DI/Tracer/provider/local-two-gpu",
        independent=independent,
        local_rank_results={
            0: {"device": "cuda:0", "values": [11.0, 22.0, 33.0, 44.0]},
            1: {"device": "cuda:1", "values": [11.0, 22.0, 33.0, 44.0]},
        },
    )

    assert completed["complete"] is True
    assert completed["collectiveBackend"] == "nccl"
    assert completed["localGroup"]["unsplitOracle"] == [11.0, 22.0, 33.0, 44.0]
    assert completed["localGroup"]["maxAbsoluteError"] == 0.0

    with pytest.raises(module.D2aIncompleteGroupError, match="missing local rank 1"):
        module.build_d2a_final_result(
            provider="/NDNSF-DI/Tracer/provider/local-two-gpu",
            independent=independent,
            local_rank_results={
                0: {"device": "cuda:0", "values": [11.0, 22.0, 33.0, 44.0]},
            },
        )


def test_d2a_single_provider_assignment_executes_every_local_role_once() -> None:
    """One grouped Selection invokes one handler, not one callback per role."""
    path = WORKLOADS / "spec170_v3_local_two_gpu_provider.py"
    spec = importlib.util.spec_from_file_location(
        "spec170_v3_local_two_gpu_provider_grouped", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    class FakeRuntime:
        def __init__(self) -> None:
            self.independent_calls: list[int] = []
            self.collective_calls = 0

        def run_independent(self, index: int) -> list[float]:
            self.independent_calls.append(index)
            return [float(index)]

        def run_local_collective(self) -> dict[int, list[float]]:
            self.collective_calls += 1
            values = list(module.UNSPLIT_ORACLE)
            return {0: values, 1: values}

    runtime = FakeRuntime()
    coordinator = module.D2aSessionCoordinator("/provider", runtime)
    completed_roles = coordinator.run_assigned_roles(
        "/request", module.ROLE_KEYS)
    result = coordinator.take_final_result("/request")

    assert completed_roles == module.ROLE_KEYS
    assert runtime.independent_calls == [0, 1]
    assert runtime.collective_calls == 1
    assert result is not None and result["complete"] is True


def test_d2a_workload_runs_real_two_gpu_ndnsf_positive_and_rank_loss_cases() -> None:
    workload = (WORKLOADS / "d2a-local-two-gpu.sh").read_text(encoding="utf-8")
    provider = (WORKLOADS / "spec170_v3_local_two_gpu_provider.py").read_text(
        encoding="utf-8")
    user = (WORKLOADS / "spec170_v3_local_two_gpu_user.py").read_text(
        encoding="utf-8")

    assert "SPEC170_D1_" not in workload
    assert "di-native-provider" not in workload
    assert "user_driver.py" not in workload
    assert "spec170_v3_local_two_gpu_provider.py" in workload
    assert "spec170_v3_local_two_gpu_user.py" in workload
    assert "run_user_case positive" in workload
    assert "run_user_case missing-rank" in workload
    assert '--case "$case_name"' in workload
    assert "SPEC170_D2A_NETWORK_PASS" in workload

    assert "torch.cuda.device_count() != 2" in provider
    assert "torch.cuda.nccl.all_reduce" in provider
    assert 'session.disable_cpu_ep_fallback", "1"' in provider
    assert "build_d2a_final_result" in provider
    assert "publish_final_response" in provider
    assert "SPEC170_D2A_GROUP_FAILURE_PASS" in user
    assert "assignment_payloads_by_role" in user


def test_d2b_workload_uses_production_native_data_v1_path() -> None:
    workload = (WORKLOADS / "d2b-cross-provider.sh").read_text(encoding="utf-8")

    assert "/opt/ndnsf-di/current/bin/di-native-provider" in workload
    assert '"$BUNDLE/user_driver.py"' in workload
    assert '--plan "$BUNDLE/native-execution-plan.json"' in workload
    assert '--manifest "$BUNDLE/service-manifest.json"' in workload
    assert "spec170_v3_cross_provider_provider.py" not in workload
    assert "spec170_v3_cross_provider_user.py" not in workload
    assert "publish_ndnsf_data_v1" not in workload
    assert "fetch_ndnsf_data_v1" not in workload
    assert "NDNSF_DI_NATIVE_TRACER_USER_EXECUTION" in workload
    assert "SPEC170_D2B_POSITIVE_PASS" in workload
    assert "peer-mismatch|replay|partial" in workload
    assert "di-native-fault-provider" in workload
    assert "fault-type missing-segment" in workload
    assert "negative-peer-mismatch.pass" in workload
    assert "negative-replay.pass" in workload
    assert "negative-partial.pass" in workload
    assert "--fixed-request-id" in workload
    assert "provider-$rank.permission-ready" in workload
    assert "NDNSF_DI_NATIVE_PROVIDER_PERMISSION_READY" in workload
    bundle_validator = (
        ROOT / "specs/170-reusable-layer-artifacts/jobs/validate_spec170_d2_bundle.py"
    ).read_text(encoding="utf-8")
    assert "D2B_PROVIDER_PERMISSION_BARRIER_MISSING" in bundle_validator

    gate = (ROOT / "specs/170-reusable-layer-artifacts/jobs/gate-d2b-cross-provider.sbatch").read_text(
        encoding="utf-8")
    assert 'd2b_case="${SPEC170_D2B_CASE:-positive}"' in gate
    assert 'printf "export NDNSF_D2B_CASE=%q\\\\n" "$d2b_case"' in gate


def test_cross_node_gates_stage_and_verify_sif_before_peer_workloads_start() -> None:
    run_container = (
        ROOT
        / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh"
    ).read_text(encoding="utf-8")
    assert "stat -Lc '%s' \"$sif\"" in run_container
    assert 'flock 9' in run_container
    assert "-name 'runtime.sif.tmp.*' -delete" in run_container
    assert "SIF_CACHE_REPAIR" in run_container
    assert "--build-record" in run_container
    assert "--metadata-only" in run_container
    assert "SPEC170_BUILD_RECORD_METADATA_PASS" in run_container

    for gate, marker, launcher in (
        ("gate-d2b-cross-provider.sbatch",
         "SPEC170_D2B_SIF_STAGE_BARRIER_PASS", "launcher=\"$scratch/launch-d2b.sh\""),
        ("gate-d2h-hybrid.sbatch",
         "SPEC170_D2H_SIF_STAGE_BARRIER_PASS", "launcher=\"$scratch/launch-d2h.sh\""),
    ):
        script = (
            ROOT / "specs/170-reusable-layer-artifacts/jobs" / gate
        ).read_text(encoding="utf-8")
        assert marker in script
        assert script.index(marker) < script.index(launcher)
        assert '"$run_container" --sif "$SPEC170_RUNTIME_SIF"' in script
        assert '"$SPEC170_RUNTIME_BUILD_RECORD"' in script
        assert '-- "/bin/true"' in script
    d2b_gate = (ROOT / "specs/170-reusable-layer-artifacts/jobs/gate-d2b-cross-provider.sbatch").read_text(
        encoding="utf-8")
    d2h_gate = (ROOT / "specs/170-reusable-layer-artifacts/jobs/gate-d2h-hybrid.sbatch").read_text(
        encoding="utf-8")
    for gate, marker in (
        (d2b_gate, "SPEC170_D2B_WORKLOAD_BIND_PASS"),
        (d2h_gate, "SPEC170_D2H_WORKLOAD_BIND_PASS"),
    ):
        assert 'container_workload="/release/${SPEC170_WORKLOAD#"$SPEC170_PROJECT_ROOT/releases/"}"' in gate
        assert marker in gate


def test_d2_bundle_builder_materializes_minimal_hash_bound_gate_inputs(tmp_path) -> None:
    builder_path = (
        ROOT / "specs/170-reusable-layer-artifacts/jobs/build_spec170_d2_bundles.py")
    spec = importlib.util.spec_from_file_location(
        "build_spec170_d2_bundles", builder_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    base = tmp_path / "base"
    artifacts = base / "artifacts"
    artifacts.mkdir(parents=True)
    (base / "trust-schema.conf").write_text("validator\n{\n}\n", encoding="utf-8")
    (base / "nfd.conf.in").write_text(
        "face_system\n{\n"
        "  unix\n  {\n    path @@NFD_SOCKET@@\n  }\n"
        "  tcp\n  {\n    listen yes\n    port @@TCP_PORT@@\n  }\n"
        "}\n; rank @@NODE_RANK@@\n",
        encoding="utf-8",
    )
    for name in (
        "qwen-native-tracer-backbone.onnx",
        "qwen-native-tracer-head0.onnx",
        "qwen-native-tracer-head1.onnx",
        "qwen-native-tracer-merge.onnx",
    ):
        (artifacts / name).write_bytes(("fixture:" + name).encode("utf-8"))

    output = tmp_path / "bundles"
    sif_sha = "0" * 64
    built = module.build_bundles(
        base_bundle=base, workloads=WORKLOADS, output_root=output,
        sif_sha256=sif_sha, release_id="spec170-test-r18",
    )
    assert set(built) == {"d2a", "d2b", "d2h"}

    validator_path = (
        ROOT / "specs/170-reusable-layer-artifacts/jobs/validate_spec170_d2_bundle.py")
    validator_spec = importlib.util.spec_from_file_location(
        "validate_spec170_d2_bundle", validator_path)
    assert validator_spec is not None and validator_spec.loader is not None
    validator = importlib.util.module_from_spec(validator_spec)
    validator_spec.loader.exec_module(validator)

    expected = {
        "d2a": ("/Inference/Spec170LocalTwoGpu", "local-two-gpu"),
        "d2b": ("/Inference/Spec170Collective", "rank0"),
        "d2h": ("/Inference/Spec170Hybrid", "p0"),
    }
    for gate, (service, identity) in expected.items():
        bundle = output / gate
        manifest = json.loads((bundle / "bundle-manifest.json").read_text())
        assert manifest["status"] == "PASS"
        assert manifest["gate"] == gate
        assert manifest["service"] == service
        assert manifest["sifSha256"] == sif_sha
        assert "@@" not in (bundle / "nfd.conf").read_text()
        policy = (bundle / "controller.policies").read_text()
        assert service in policy
        assert f"/NDNSF-DI/Tracer/provider/{identity}" in policy
        for provider, roles in validator.GATE_CONTRACTS[gate]["providerRoles"].items():
            assert provider in policy
            for role in roles:
                assert service + "/ROLE" + role.replace("#", "%23") in policy
        for relative, recorded in manifest["files"].items():
            actual = hashlib.sha256((bundle / relative).read_bytes()).hexdigest()
            assert actual == recorded["sha256"]
            assert (bundle / relative).stat().st_size == recorded["bytes"]
        report = validator.validate_bundle(
            bundle=bundle, workload=bundle / manifest["workload"],
            expected_sif_sha256=sif_sha, expected_gate=gate,
            expected_service=service,
        )
        assert report["status"] == "PASS", report["errors"]

    d2b = output / "d2b"
    d2b_manifest = json.loads((d2b / "service-manifest.json").read_text())
    d2b_plan = json.loads((d2b / "native-execution-plan.json").read_text())
    service_plan = d2b_plan["services"][0]
    service_manifest = d2b_manifest["services"][0]

    assert service_plan["service"] == "/Inference/Spec170Collective"
    assert service_plan["executionPolicy"] == "DATA_DRIVEN_V2"
    assert service_plan["roles"] == ["/Backbone", "/Head/Shard/0"]
    assert service_plan["dependencies"][0]["transportProfile"] == "NDNSF_DATA_V1"
    assert service_plan["dependencies"][0]["expectedSegments"] == 0
    assert service_plan["dependencies"][0]["collectiveOperationIndex"] == 0
    assert service_manifest["artifacts"][0]["metadata"]["executionProvider"] == "cuda"
    assert service_manifest["artifacts"][1]["metadata"]["executionProvider"] == "cuda"
    assert service_manifest["artifacts"][1]["metadata"]["outputScope.0"] == "final-response"
    assert (d2b / "user_driver.py").is_file()
    assert (d2b / "artifacts/qwen-native-tracer-backbone.onnx").is_file()
    assert (d2b / "artifacts/qwen-native-tracer-head0.onnx").is_file()

    assert (output / "d2a/artifacts/qwen-native-tracer-backbone.onnx").is_file()
    assert not (output / "d2a/artifacts/qwen-native-tracer-head0.onnx").exists()
    assert (output / "d2b/artifacts/qwen-native-tracer-head0.onnx").is_file()
    assert (output / "d2h/artifacts/qwen-native-tracer-merge.onnx").is_file()
    d2a_nfd = (output / "d2a/nfd.conf").read_text(encoding="utf-8")
    d2b_nfd = (output / "d2b/nfd.conf").read_text(encoding="utf-8")
    d2a_tcp = d2a_nfd.split("\n  tcp\n  {", 1)[1].split("\n  }", 1)[0]
    d2b_tcp = d2b_nfd.split("\n  tcp\n  {", 1)[1].split("\n  }", 1)[0]
    assert "listen no" in d2a_tcp
    assert "listen yes" in d2b_tcp
    assert "/Inference/Spec170LocalTwoGpu/ROLE/D2A/LocalGroup%230" in (
        output / "d2a/controller.policies").read_text(encoding="utf-8")

    tampered = output / "d2a/spec170_v3_local_two_gpu_user.py"
    tampered.write_text(tampered.read_text() + "\n# tampered\n", encoding="utf-8")
    rejected = validator.validate_bundle(
        bundle=output / "d2a", workload=output / "d2a/d2a-local-two-gpu.sh",
        expected_sif_sha256=sif_sha, expected_gate="d2a",
        expected_service="/Inference/Spec170LocalTwoGpu",
    )
    assert rejected["status"] == "FAIL"
    assert "FILE_HASH_MISMATCH:spec170_v3_local_two_gpu_user.py" in rejected["errors"]


def test_all_spec170_slurm_gates_require_prefixed_sif_digest():
    jobs = ROOT / "specs/170-reusable-layer-artifacts/jobs"
    for name in (
        "gate-d0-cpu.sbatch",
        "gate-d1-single.sbatch",
        "gate-d2a-local-two-gpu.sbatch",
        "gate-d2b-cross-provider.sbatch",
        "gate-d2h-hybrid.sbatch",
    ):
        text = (jobs / name).read_text(encoding="utf-8")
        assert "^sha256:[0-9a-f]{64}$" in text
        assert "SPEC170_RUNTIME_SIF_SHA256_MUST_BE_PREFIXED" in text
        assert 'SPEC170_RUNTIME_BUILD_RECORD' in text
