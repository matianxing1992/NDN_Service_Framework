"""Reject incomplete or misleading Controller-to-Provider grant evidence."""

import csv
import importlib.util
from pathlib import Path

import pytest


def evaluator():
    path = Path(__file__).with_name("spec179_scenario_checks.py")
    spec = importlib.util.spec_from_file_location("provider_grant_checks", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fixture(root, mutation=""):
    attribute = "/PERMISSION/HELLO" if mutation == "wrong-role" else "/SERVICE/HELLO"
    (root / "controller-1.log").write_text(
        "10.000 INFO: [controller] NDNSF_CONTROLLER_GRANT "
        "identity=/example/hello/provider/B service=/HELLO attribute=" + attribute +
        " generation=100 epoch=2 abeParametersUnchanged=true\n"
        "NDNSF_GRANT_ONLY_APPLIED success=1 identity=/example/hello/provider/B "
        "service=/HELLO generation=100 epoch=2\n")
    lines = ["1.000 INFO: [provider] NDNSF_NAC_BOOTSTRAP_PENDING role=provider"]
    if mutation != "missing-timeout":
        lines.append("2.000 INFO: [provider] PermissionResponse timeout: final=1")
    if mutation != "missing-renewal":
        lines.append("12.000 INFO: [provider] NDNSF_APP_PERMISSION_REFETCH")
    lines += [
        "13.000 INFO: [provider] NDNSF_NAC_DKEY_REFRESH_REQUESTED role=provider serviceName=/HELLO epoch=2 reason=grant-only",
        "13.500 INFO: [provider] Installed PolicyStatus service=/HELLO generation=100 epoch=2",
        "16.000 DEBUG: [provider] event=HYBRID_PUBLISH messageName=/response messageType=RESPONSE",
    ]
    if mutation == "pre-grant-service":
        lines.append("5.000 DEBUG: [provider] event=HYBRID_PUBLISH messageName=/early messageType=ACK")
    (root / "provider-B.log").write_text("\n".join(lines) + "\n")
    (root / "provider-A.log").write_text(
        "15.000 INFO: [provider] NDNSF_NAC_DKEY_REFRESH_REQUESTED epoch=2 reason=grant-only\n"
        if mutation == "unaffected-refresh" else "")
    (root / "user-B.log").write_text("14.000 INFO: [user] NDNSF_APP_PERMISSION_REFETCH\n")
    for role, provider in (("A", "A"), ("B", "B")):
        directory = root / ("user-" + role)
        directory.mkdir()
        selected = "/example/hello/provider/" + provider
        if role == "B" and mutation == "wrong-provider":
            selected = "/example/hello/provider/A"
        success = int(not ((role == "B" and mutation == "target-failure") or
                           (role == "A" and mutation == "control-failure")))
        times = [5, 15] if role == "A" else [15]
        if role == "B" and mutation == "empty-target":
            times = []
        with (directory / "request-results.csv").open("w") as f:
            writer = csv.DictWriter(f, fieldnames=["request_id", "success"])
            writer.writeheader()
            writer.writerows({"request_id": role + str(t), "success": success} for t in times)
        with (directory / "request_lifecycle.csv").open("w") as f:
            fields = ["request_id", "enqueue_timestamp_us", "end_to_end_latency_ms",
                      "state", "selected_provider", "final_cleanup_reason"]
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            writer.writerows({"request_id": role + str(t), "enqueue_timestamp_us": t * 1000000,
                              "end_to_end_latency_ms": 1000, "state": "completed",
                              "selected_provider": selected, "final_cleanup_reason": "response_callback"}
                             for t in times)


@pytest.mark.parametrize("late", [False, True])
def test_provider_grant_requires_complete_positive_evidence(tmp_path, late):
    fixture(tmp_path)
    result = evaluator().evaluate("provider-grant-only-advance", tmp_path,
                                  {"initialProviderPermissionTransportLoss": late})
    assert result["passed"] is True, result


@pytest.mark.parametrize("mutation", ["wrong-role", "pre-grant-service", "missing-renewal",
    "empty-target", "target-failure", "control-failure", "wrong-provider", "unaffected-refresh",
    "missing-timeout"])
def test_provider_grant_rejects_false_acceptance(tmp_path, mutation):
    fixture(tmp_path, mutation)
    result = evaluator().evaluate("provider-grant-only-advance", tmp_path,
                                  {"initialProviderPermissionTransportLoss": True})
    assert result["passed"] is False, result
