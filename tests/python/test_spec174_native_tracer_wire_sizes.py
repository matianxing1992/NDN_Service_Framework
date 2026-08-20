from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import time


REPO = Path(__file__).resolve().parents[2]
PLAN_TRACER = (
    REPO
    / "examples/python/NDNSF-DistributedInference/native_di_tracer/plan_tracer.py"
)

sys.path.insert(
    0,
    str(REPO / "examples/python/NDNSF-DistributedInference/native_di_tracer"),
)

from wire_sizes import encoded_tensor_bundle_size  # noqa: E402
from generate_spec170_hybrid_native_bundle import (  # noqa: E402
    generate_hybrid_native_bundle,
)
from user_driver import _static_v3_dataflow_contracts  # noqa: E402
from user_driver import execution_lease_plan_digest  # noqa: E402


def test_native_tracer_plan_seals_actual_encoded_bundle_sizes(tmp_path):
    output = tmp_path / "policy"
    subprocess.run(
        [sys.executable, str(PLAN_TRACER), "--out", str(output)],
        cwd=REPO,
        check=True,
    )
    plan = json.loads(
        (output / "native-execution-plan.json").read_text(encoding="utf-8")
    )
    service = next(
        item for item in plan["services"] if item["service"] == "/Inference/NativeTracer"
    )
    expected = {
        "backbone-to-head0": encoded_tensor_bundle_size("features", (1, 16), 16 * 4),
        "backbone-to-head1": encoded_tensor_bundle_size("features", (1, 16), 16 * 4),
        # Head artifacts intentionally publish one raw ONNX tensor rather than
        # a multi-tensor bundle, so the sealed size is the payload size.
        "head0-to-merge": 8 * 4,
        "head1-to-merge": 8 * 4,
    }
    observed = {item["keyScope"]: item["expectedBytes"] for item in service["dependencies"]}
    assert observed == expected


def test_native_tracer_padding_accounts_for_padding_tensor_wire_overhead(tmp_path):
    output = tmp_path / "policy"
    subprocess.run(
        [
            sys.executable,
            str(PLAN_TRACER),
            "--out",
            str(output),
            "--activation-pad-bytes",
            "16",
        ],
        cwd=REPO,
        check=True,
    )
    plan = json.loads(
        (output / "native-execution-plan.json").read_text(encoding="utf-8")
    )
    service = next(
        item for item in plan["services"] if item["service"] == "/Inference/NativeTracer"
    )
    observed = {
        item["keyScope"]: item["expectedBytes"] for item in service["dependencies"]
    }
    padding_wire_bytes = encoded_tensor_bundle_size(
        "__ndnsf_padding", (4,), 16
    )
    assert observed["backbone-to-head0"] == 120 + padding_wire_bytes
    assert observed["backbone-to-head1"] == 120 + padding_wire_bytes


def test_static_hybrid_v3_projection_carries_role_dataflow(tmp_path):
    bundle = tmp_path / "hybrid"
    generate_hybrid_native_bundle(bundle, "121")
    plan = json.loads(
        (bundle / "native-execution-plan.json").read_text(encoding="utf-8")
    )["services"][0]
    providers = {
        role: f"/NDNSF-DI/Tracer/provider/test-{index}"
        for index, role in enumerate(plan["roles"])
    }
    contracts = _static_v3_dataflow_contracts(
        service_plan=plan,
        provider_by_role=providers,
        request_id="/spec174-test",
        attempt=1,
        plan_digest="sha256:" + "1" * 64,
        deadline_ms=int(time.time() * 1000) + 60_000,
        no_progress_ms=2_000,
    )
    assert (len(contracts["S0R0"].may_publish),
            len(contracts["S0R0"].must_fetch)) == (1, 0)
    assert (len(contracts["S1R0"].may_publish),
            len(contracts["S1R0"].must_fetch)) == (1, 1)
    assert (len(contracts["S1R1"].may_publish),
            len(contracts["S1R1"].must_fetch)) == (1, 1)
    assert (len(contracts["S2R0"].may_publish),
            len(contracts["S2R0"].must_fetch)) == (0, 2)
    assert contracts["S2R0"].terminal_response_owner is True

    hybrid_212 = tmp_path / "hybrid-212"
    generate_hybrid_native_bundle(hybrid_212, "212")
    plan_212 = json.loads(
        (hybrid_212 / "native-execution-plan.json").read_text(encoding="utf-8")
    )["services"][0]
    providers_212 = {
        role: f"/NDNSF-DI/Tracer/provider/test-212-{index}"
        for index, role in enumerate(plan_212["roles"])
    }
    contracts_212 = _static_v3_dataflow_contracts(
        service_plan=plan_212,
        provider_by_role=providers_212,
        request_id="/spec174-test-212",
        attempt=1,
        plan_digest="sha256:" + "2" * 64,
        deadline_ms=int(time.time() * 1000) + 60_000,
        no_progress_ms=2_000,
    )
    assert [item.tensor_id for item in contracts_212["S2R1"].may_publish] == [
        "partial-sum"
    ]
    assert [item.tensor_id for item in contracts_212["S2R0"].must_fetch] == [
        "activation-1", "partial-sum"
    ]
    assert contracts_212["S2R0"].terminal_response_owner is True


def test_native_plan_digest_uses_manifest_canonical_lowercase(tmp_path):
    plan = tmp_path / "native-execution-plan.json"
    plan.write_bytes(b"spec174-plan")
    digest = execution_lease_plan_digest(str(plan))
    assert digest == digest.lower()
    assert digest.startswith("sha256:")
