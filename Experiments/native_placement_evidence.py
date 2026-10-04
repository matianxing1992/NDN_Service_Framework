"""Offline evidence for targeted probing and placement, independent of models."""

from __future__ import annotations

import json
import hashlib
import math
import re
from typing import Any


def _evidence_fields(line: str) -> dict[str, str]:
    fields_text, marker, manifest_json = line.partition(" manifestJson=")
    fields = dict(re.findall(r"([A-Za-z][A-Za-z0-9_]*)=([^\s]+)", fields_text))
    if marker:
        fields["manifestJson"] = manifest_json
    return fields


def _evidence_records(text: str, marker: str) -> list[str]:
    return [line[line.index(marker):] for line in text.splitlines() if marker in line]


def validate_native_probe_evidence(user_text: str, request_id: str) -> dict[str, Any]:
    """Validate targeted dispatch, authenticated response and fresh snapshot."""
    if not request_id:
        raise RuntimeError("SPEC196_NATIVE_REQUEST_ID_MISSING")
    dispatch = [_evidence_fields(line) for line in _evidence_records(
        user_text, "NDNSF_DI_NATIVE_PLACEMENT_PROBE_DISPATCH ")]
    dispatch = [row for row in dispatch if row.get("requestId") == request_id]
    if (not dispatch or any(not row.get("observer") or
                            row.get("invocation") != "NDNSF_Targeted" for row in dispatch) or
            len({row.get("observer") for row in dispatch}) != len(dispatch)):
        raise RuntimeError("SPEC196_NATIVE_TARGETED_DISPATCH_MISSING")
    requested_rtt: set[tuple[str, str, str]] = set()
    requested_goodput: set[tuple[str, str, str]] = set()
    for row in dispatch:
        try:
            rtt_pairs = json.loads(row["rttPairs"])
            goodput_edges = json.loads(row["goodputEdges"])
            counts_match = (int(row["rttCount"]) == len(rtt_pairs) and
                            int(row["goodputEdgeCount"]) == len(goodput_edges))
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            raise RuntimeError("SPEC196_NATIVE_DISPATCH_EDGES_MISSING") from error
        if (not isinstance(rtt_pairs, list) or not isinstance(goodput_edges, list) or
                not counts_match):
            raise RuntimeError("SPEC196_NATIVE_DISPATCH_EDGE_COUNTS_MISMATCH")
        for pair in rtt_pairs:
            if not isinstance(pair, dict):
                raise RuntimeError("SPEC196_NATIVE_DISPATCH_RTT_PAIR_INVALID")
            requested_rtt.add((row["observer"], str(pair.get("src_provider", "")),
                               str(pair.get("dst_provider", ""))))
        for edge in goodput_edges:
            if not isinstance(edge, dict):
                raise RuntimeError("SPEC196_NATIVE_DISPATCH_GOODPUT_EDGE_INVALID")
            requested_goodput.add((row["observer"], str(edge.get("src_provider", "")),
                                   str(edge.get("dst_provider", ""))))
    if (any(not observer or not source or not target
            for observer, source, target in requested_rtt | requested_goodput) or
            not requested_rtt and not requested_goodput):
        raise RuntimeError("SPEC196_NATIVE_DISPATCH_HAS_NO_VALID_EDGES")
    responses = [_evidence_fields(line) for line in _evidence_records(
        user_text, "NDNSF_DI_NATIVE_PLACEMENT_PROBE_RESPONSE ")]
    responses = [row for row in responses if row.get("requestId") == request_id]
    if (len(responses) != len(dispatch) or
            any(row.get("status") != "accepted" for row in responses) or
            {row.get("observer") for row in responses} !=
            {row.get("observer") for row in dispatch}):
        raise RuntimeError("SPEC196_NATIVE_TARGETED_RESPONSE_INCOMPLETE")
    snapshot = [_evidence_fields(line) for line in _evidence_records(
        user_text, "NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT ")]
    accepted_snapshots = [row for row in snapshot
                          if row.get("requestId") == request_id and
                          row.get("status") == "accepted" and
                          row.get("snapshotDigest", "").startswith("sha256:")]
    if len(accepted_snapshots) != 1:
        raise RuntimeError("SPEC196_NATIVE_SNAPSHOT_MISSING")
    snapshot_row = accepted_snapshots[0]
    try:
        if int(snapshot_row["ageMs"]) < 0 or int(snapshot_row["expiresInMs"]) <= 0:
            raise ValueError("snapshot is stale")
        observed_rtt = json.loads(snapshot_row["activeProbeRttRows"])
        observed_goodput = json.loads(snapshot_row["activeProbeGoodputRows"])
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise RuntimeError("SPEC196_NATIVE_SNAPSHOT_FRESHNESS_OR_ROWS_INVALID") from error
    if (not isinstance(observed_rtt, list) or not isinstance(observed_goodput, list) or
            any(not isinstance(row, dict) for row in observed_rtt + observed_goodput)):
        raise RuntimeError("SPEC196_NATIVE_SNAPSHOT_ROWS_INVALID")
    rtt_rows = {(str(row.get("reported_by", "")),
                 str(row.get("src_provider", "")),
                 str(row.get("dst_provider", ""))) for row in observed_rtt}
    goodput_rows = {(str(row.get("reported_by", "")),
                     str(row.get("src_provider", "")),
                     str(row.get("dst_provider", ""))) for row in observed_goodput}
    goodput_samples = {(str(row.get("reported_by", "")),
                        str(row.get("src_provider", "")),
                        str(row.get("dst_provider", "")),
                        str(row.get("sample_digest", ""))) for row in observed_goodput}
    if (not rtt_rows and not goodput_rows or
            not rtt_rows.issubset(requested_rtt) or
            not goodput_rows.issubset(requested_goodput)):
        raise RuntimeError("SPEC196_NATIVE_PROBE_DATA_NOT_CONSUMED")
    return {
        "requestId": request_id,
        "probeDispatches": dispatch,
        "probeResponses": responses,
        "requestedRttPairs": sorted(requested_rtt),
        "requestedGoodputEdges": sorted(requested_goodput),
        "activeProbeRttRows": sorted(rtt_rows),
        "activeProbeGoodputRows": sorted(goodput_rows),
        "activeProbeGoodputSamples": sorted(goodput_samples),
        "snapshotDigest": snapshot_row["snapshotDigest"],
    }


def validate_native_placement_evidence(
        user_text: str, provider_text: str, expected_roles: list[str], *,
        expected_manifest_digest: str, probe_evidence: dict[str, Any],
        expected_score_mode: str | None = None) -> dict[str, Any]:
    """Require the committed assignment to consume a verified targeted sample."""
    request_id = probe_evidence["requestId"]
    snapshot_row = {"snapshotDigest": probe_evidence.get("snapshotDigest", "")}
    requested_goodput = set(map(tuple, probe_evidence.get("requestedGoodputEdges", [])))
    goodput_samples = set(map(tuple, probe_evidence.get("activeProbeGoodputSamples", [])))
    commits = [_evidence_fields(line) for line in _evidence_records(
        user_text, "NDNSF_DI_NATIVE_SELECTION_COMMITTED ")]
    commit = next((row for row in commits if row.get("requestId") == request_id), None)
    if commit is None:
        raise RuntimeError("SPEC196_NATIVE_SELECTION_COMMIT_MISSING")
    if not commit.get("attemptEpoch") or not commit.get("planDigest"):
        raise RuntimeError("SPEC196_NATIVE_SELECTION_COMMIT_INVALID")
    committed_manifest_digest = expected_manifest_digest
    if "manifestJson" in commit:
        try:
            manifest_bytes = commit["manifestJson"].encode("utf-8")
            manifest = json.loads(manifest_bytes)
            committed_manifest_digest = "sha256:" + hashlib.sha256(manifest_bytes).hexdigest()
            if (committed_manifest_digest != commit.get("manifestDigest") or
                    manifest.get("schema") != "ndnsf-di-canonical-model-manifest-v1" or
                    manifest.get("state") != "ACTIVE" or
                    manifest.get("metadata", {}).get("packageManifestDigest") != expected_manifest_digest):
                raise ValueError("manifest binding mismatch")
        except (ValueError, TypeError, AttributeError) as error:
            raise RuntimeError("SPEC196_NATIVE_COMMITTED_MANIFEST_INVALID") from error
    decisions = [_evidence_fields(line) for line in _evidence_records(
        user_text, "NDNSF_DI_NATIVE_PLACEMENT_DECISION ")]
    decision_line = next((row for row in decisions if row.get("requestId") == request_id), None)
    if (decision_line is None or
            decision_line.get("strategy") != "native-observation-aware-v4"):
        raise RuntimeError("SPEC196_NATIVE_OBSERVATION_STRATEGY_MISSING")
    try:
        decision = json.loads(decision_line["decisionJson"])
    except (KeyError, TypeError, json.JSONDecodeError) as error:
        raise RuntimeError("SPEC196_NATIVE_DECISION_INVALID") from error
    if not isinstance(decision, dict):
        raise RuntimeError("SPEC196_NATIVE_DECISION_INVALID")
    if (decision.get("status") != "SCORED" or
            decision.get("snapshot_digest") != snapshot_row["snapshotDigest"] or
            decision.get("fallback_reason") != ""):
        raise RuntimeError("SPEC196_NATIVE_OBSERVATION_STRATEGY_FELL_BACK")
    if (expected_score_mode is not None and
            (decision.get("score_mode") != expected_score_mode or
             decision.get("score_is_end_to_end_latency") is not False)):
        raise RuntimeError("SPEC196_NATIVE_PLACEMENT_SCORE_MODE_MISMATCH")
    decision_placement = {
        str(row.get("role_key", "")): str(row.get("provider", ""))
        for row in decision.get("selected_assignments", []) if isinstance(row, dict)
    }
    if (set(decision_placement) != set(expected_roles) or
            any(not provider for provider in decision_placement.values())):
        raise RuntimeError("SPEC196_NATIVE_DECISION_PLACEMENT_INCOMPLETE")
    edge_costs = decision.get("edge_costs", [])
    probe_costs = []
    if isinstance(edge_costs, list):
        for cost in edge_costs:
            if not isinstance(cost, dict) or cost.get("rate_method") != "ACTIVE_PROBE_GOODPUT":
                continue
            source = str(cost.get("src_peer", ""))
            destination = str(cost.get("dst_peer", ""))
            sample_digest = str(cost.get("sample_digest", ""))
            producer_role = str(cost.get("producer_role", ""))
            consumer_role = str(cost.get("consumer_role", ""))
            try:
                goodput_mbps = float(cost.get("goodput_mbps"))
            except (TypeError, ValueError):
                continue
            if (not math.isfinite(goodput_mbps) or goodput_mbps <= 0 or
                    not any((observer, source, destination, sample_digest) in goodput_samples
                            for observer, src, dst in requested_goodput
                            if observer and src == source and dst == destination) or
                    decision_placement.get(producer_role) != source or
                    decision_placement.get(consumer_role) != destination):
                continue
            probe_costs.append(cost)
    if not probe_costs:
        raise RuntimeError("SPEC196_NATIVE_TARGETED_SAMPLE_NOT_USED_IN_SELECTED_COST")
    accepted = [_evidence_fields(line) for line in _evidence_records(
        provider_text, "NDNSF_DI_NATIVE_SELECTION_ACCEPTED ")]
    accepted = [row for row in accepted
                if row.get("requestId") == request_id and
                row.get("attemptEpoch") == commit.get("attemptEpoch") and
                row.get("planDigest") == commit.get("planDigest")]
    role_map = {row.get("role", ""): row.get("provider", "") for row in accepted}
    if (set(role_map) != set(expected_roles) or
            any(not provider for provider in role_map.values()) or
            role_map != decision_placement or
            any(row.get("manifestDigest") != committed_manifest_digest for row in accepted)):
        raise RuntimeError("SPEC196_NATIVE_FINAL_PLACEMENT_INCOMPLETE")
    return {
        "requestId": request_id,
        "attemptEpoch": commit["attemptEpoch"],
        "planDigest": commit["planDigest"],
        "placementDecision": decision,
        "targetedProbeCosts": probe_costs,
        "placementByRole": role_map,
    }


def native_placement_request_ids(user_text: str) -> list[str]:
    """Return network request identities in their original log order."""
    markers = ("NDNSF_DI_NATIVE_PLACEMENT_PROBE_DISPATCH ",
               "NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT ",
               "NDNSF_DI_NATIVE_PLACEMENT_DECISION ")
    return list(dict.fromkeys(
        request for line in user_text.splitlines() if any(marker in line for marker in markers)
        for request in [_evidence_fields(line).get("requestId", "")] if request))


def collect_native_placement_evidence(
        user_text: str, provider_text: str, expected_roles: list[str], *,
        expected_manifest_digest: str, expected_score_mode: str | None = None,
        request_id: str | None = None
        ) -> dict[str, Any]:
    """Report probe and placement boundaries without running a model oracle."""
    requests = native_placement_request_ids(user_text)
    report: dict[str, Any] = {"boundaries": {}}
    if ((request_id is None and len(requests) != 1) or
            (request_id is not None and request_id not in requests)):
        for boundary in ("probe", "placement"):
            report["boundaries"][boundary] = {
                "status": "FAIL", "reason": "SPEC196_NATIVE_REQUEST_ID_AMBIGUOUS"}
        report["status"] = "FAIL"
        return report
    request_id = request_id if request_id is not None else requests[0]
    report["requestId"] = request_id
    probes: dict[str, Any] = {"requestId": request_id}
    try:
        probes = validate_native_probe_evidence(user_text, request_id)
        report.update(probes)
        missing_rtt = (set(map(tuple, probes["requestedRttPairs"])) -
                       set(map(tuple, probes["activeProbeRttRows"])))
        missing_goodput = (set(map(tuple, probes["requestedGoodputEdges"])) -
                           set(map(tuple, probes["activeProbeGoodputRows"])))
        probe_boundary = {
            "status": "FAIL" if missing_rtt or missing_goodput else "PASS",
            "missingRttRows": len(missing_rtt),
            "missingGoodputRows": len(missing_goodput),
        }
        if missing_rtt or missing_goodput:
            probe_boundary["reason"] = "SPEC196_NATIVE_REQUESTED_PROBE_SAMPLES_MISSING"
        report["boundaries"]["probe"] = probe_boundary
    except (RuntimeError, TypeError, ValueError, KeyError) as error:
        report["boundaries"]["probe"] = {"status": "FAIL", "reason": str(error)}
    try:
        placement = validate_native_placement_evidence(
            user_text, provider_text, expected_roles,
            expected_manifest_digest=expected_manifest_digest,
            probe_evidence=probes, expected_score_mode=expected_score_mode)
        report.update(placement)
        report["boundaries"]["placement"] = {"status": "PASS"}
    except (RuntimeError, TypeError, ValueError, KeyError) as error:
        report["boundaries"]["placement"] = {"status": "FAIL", "reason": str(error)}
        # Preserve the actual score mode/fallback even when qualification fails.
        for line in _evidence_records(user_text, "NDNSF_DI_NATIVE_PLACEMENT_DECISION "):
            row = _evidence_fields(line)
            if row.get("requestId") == request_id:
                try:
                    report["placementDecision"] = json.loads(row["decisionJson"])
                except (KeyError, TypeError, ValueError):
                    pass
    report["status"] = ("PASS" if all(row["status"] == "PASS"
                                    for row in report["boundaries"].values()) else "FAIL")
    return report
