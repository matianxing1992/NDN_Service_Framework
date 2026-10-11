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



_PLACEMENT_REQUEST_MARKERS = (
    "NDNSF_DI_NATIVE_PLACEMENT_PROBE_DISPATCH ",
    "NDNSF_DI_NATIVE_PLACEMENT_CACHE_DISPATCH ",
    "NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT ",
    "NDNSF_DI_NATIVE_PLACEMENT_DECISION ",
)


class _EvidenceSnapshot:
    """Owned field parsing for one collection; JSON validation stays lazy."""

    def __init__(self, text: str):
        self._lines = tuple(text.splitlines())
        self._records = {}
        self._requests = None
        self._models = None

    def rows(self, marker: str, request_id: str) -> list[dict[str, str]]:
        if marker not in self._records:
            grouped = {}
            for line in self._lines:
                if marker in line:
                    row = _evidence_fields(line[line.index(marker):])
                    grouped.setdefault(row.get("requestId"), []).append(row)
            self._records[marker] = {
                key: tuple(rows) for key, rows in grouped.items()}
        return [dict(row) for row in self._records[marker].get(request_id, ())]

    def request_ids(self) -> list[str]:
        if self._requests is None:
            self._requests = tuple(dict.fromkeys(
                request for line in self._lines
                if any(marker in line for marker in _PLACEMENT_REQUEST_MARKERS)
                for request in [_evidence_fields(line).get("requestId", "")]
                if request))
        return list(self._requests)

    def model_rows(self, request_id: str | None = None) -> list[dict[str, str]]:
        if self._models is None:
            self._models = tuple(
                _evidence_fields(line) for line in self._lines
                if line.startswith("YOLO_NATIVE_REQUEST_RESULT "))
        return [dict(row) for row in self._models
                if request_id is None or row.get("request") == request_id]


def validate_native_probe_evidence(user_text: str, request_id: str) -> dict[str, Any]:
    return _validate_native_probe_evidence(_EvidenceSnapshot(user_text), request_id)


def _validate_native_probe_evidence(user_text: _EvidenceSnapshot, request_id: str) -> dict[str, Any]:
    """Validate targeted responses or qualified historical samples in fresh ACKs."""
    if not request_id:
        raise RuntimeError("SPEC196_NATIVE_REQUEST_ID_MISSING")
    dispatch = user_text.rows("NDNSF_DI_NATIVE_PLACEMENT_PROBE_DISPATCH ", request_id)
    dispatch = [row for row in dispatch if row.get("requestId") == request_id]
    cache_queries = user_text.rows("NDNSF_DI_NATIVE_PLACEMENT_CACHE_DISPATCH ", request_id)
    cache_responses = user_text.rows("NDNSF_DI_NATIVE_PLACEMENT_CACHE_RESPONSE ", request_id)
    cache_results = user_text.rows("NDNSF_DI_NATIVE_PLACEMENT_CACHE_RESULT ", request_id)
    query_by_observer = {row.get("observer"): row for row in cache_queries}
    result_by_observer = {row.get("observer"): row for row in cache_results}
    if (len(query_by_observer) != len(cache_queries) or
            len(result_by_observer) != len(cache_results) or
            set(query_by_observer) != set(result_by_observer) or
            any(not row.get("observer") or row.get("invocation") != "NDNSF_Targeted"
                for row in cache_queries) or
            any(row.get("status") not in ("hit", "miss") for row in cache_results) or
            any(row.get("observer") not in query_by_observer for row in cache_responses)):
        raise RuntimeError("SPEC199_NATIVE_CACHE_QUERY_INCOMPLETE")
    cache_hits = {observer for observer, row in result_by_observer.items()
                  if row["status"] == "hit"}
    for observer, query in query_by_observer.items():
        measurements = [row for row in dispatch if row.get("observer") == observer]
        replies = [row for row in cache_responses if row.get("observer") == observer]
        if (len(replies) > 1 or observer in cache_hits and
                (measurements or len(replies) != 1 or replies[0].get("status") != "accepted")):
            raise RuntimeError("SPEC199_NATIVE_CACHE_HIT_BINDING_INVALID")
        if observer not in cache_hits and (len(measurements) != 1 or any(
                measurements[0].get(field) != query.get(field)
                for field in ("attempt", "rttCount", "goodputEdgeCount", "rttPairs", "goodputEdges"))):
            raise RuntimeError("SPEC199_NATIVE_CACHE_FALLBACK_INVALID")
    if (any(not row.get("observer") or
                            row.get("invocation") != "NDNSF_Targeted" for row in dispatch) or
            len({row.get("observer") for row in dispatch}) != len(dispatch)):
        raise RuntimeError("SPEC196_NATIVE_TARGETED_DISPATCH_MISSING")
    requested_rtt: set[tuple[str, str, str]] = set()
    requested_goodput: set[tuple[str, str, str]] = set()
    service_rtt: set[tuple[str, str, str]] = set()
    service_goodput: set[tuple[str, str, str]] = set()
    for row in dispatch + [query_by_observer[observer] for observer in sorted(cache_hits)]:
        target_rtt = service_rtt if row["observer"] in cache_hits else requested_rtt
        target_goodput = service_goodput if row["observer"] in cache_hits else requested_goodput
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
            target_rtt.add((row["observer"], str(pair.get("src_provider", "")),
                               str(pair.get("dst_provider", ""))))
        for edge in goodput_edges:
            if not isinstance(edge, dict):
                raise RuntimeError("SPEC196_NATIVE_DISPATCH_GOODPUT_EDGE_INVALID")
            target_goodput.add((row["observer"], str(edge.get("src_provider", "")),
                                   str(edge.get("dst_provider", ""))))
    if (any(not observer or not source or not target
            for observer, source, target in requested_rtt | requested_goodput | service_rtt | service_goodput) or
            (dispatch or cache_hits) and not (requested_rtt or requested_goodput or service_rtt or service_goodput)):
        raise RuntimeError("SPEC196_NATIVE_DISPATCH_HAS_NO_VALID_EDGES")
    responses = user_text.rows("NDNSF_DI_NATIVE_PLACEMENT_PROBE_RESPONSE ", request_id)
    responses = [row for row in responses if row.get("requestId") == request_id]
    if (len(responses) != len(dispatch) or
            any(row.get("status") != "accepted" for row in responses) or
            {row.get("observer") for row in responses} !=
            {row.get("observer") for row in dispatch}):
        raise RuntimeError("SPEC196_NATIVE_TARGETED_RESPONSE_INCOMPLETE")
    snapshot = user_text.rows("NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT ", request_id)
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
    cache_rtt = set()
    cache_goodput = set()
    cached_rows = [row for row in observed_rtt + observed_goodput
                   if row.get("source_kind") == "ACK_CACHE"]
    bindings = {}
    allowed_rtt = set()
    allowed_goodput = set()
    if cached_rows or cache_hits:
        try:
            checked_at = int(snapshot_row["checkedAtMs"])
            offers = json.loads(snapshot_row["providerOfferBindings"])
            roster = json.loads(snapshot_row["providerRoster"])
            rtt_pairs = json.loads(snapshot_row["rttPairs"])
            goodput_edges = json.loads(snapshot_row["goodputEdges"])
            if (not isinstance(offers, list) or not isinstance(roster, list) or
                    len(set(roster)) != len(roster) or not isinstance(rtt_pairs, list) or
                    not isinstance(goodput_edges, list)):
                raise ValueError("invalid ACK roster")
            for offer in offers:
                provider = offer["provider"]
                if (provider in bindings or provider not in roster or
                        not offer["boot_epoch"] or offer["signer_identity"] != provider or
                        not offer["offer_digest"].startswith("sha256:") or
                        not offer["ack_wire_digest"].startswith("sha256:") or
                        not 0 < int(offer["captured_at_ms"]) <= checked_at < int(offer["expires_at_ms"]) or
                        checked_at - int(offer["captured_at_ms"]) > 5000):
                    raise ValueError("invalid fresh authenticated ACK binding")
                bindings[provider] = offer
            if set(bindings) != set(roster):
                raise ValueError("incomplete current boot roster")
            allowed_rtt = {(pair["src_provider"], pair["dst_provider"]) for pair in rtt_pairs}
            allowed_goodput = {(pair["src_provider"], pair["dst_provider"]) for pair in goodput_edges}
        except (KeyError, TypeError, ValueError, AttributeError) as error:
            raise RuntimeError("SPEC199_NATIVE_CACHED_ACK_BINDING_INVALID") from error
    consumed_service_rtt = set()
    consumed_service_goodput = set()
    for rows, requested, service_requested, consumed_service, allowed, cached in (
            (observed_rtt, requested_rtt, service_rtt, consumed_service_rtt, allowed_rtt, cache_rtt),
            (observed_goodput, requested_goodput, service_goodput, consumed_service_goodput, allowed_goodput, cache_goodput)):
        for row in rows:
            edge = (str(row.get("reported_by", "")), str(row.get("src_provider", "")),
                    str(row.get("dst_provider", "")))
            source_kind = row.get("source_kind", "TARGETED_RESPONSE")
            service_cached = source_kind == "TARGETED_RESPONSE" and edge in service_requested
            if source_kind == "TARGETED_RESPONSE" and not service_cached:
                if edge not in requested:
                    raise RuntimeError("SPEC196_NATIVE_PROBE_DATA_NOT_CONSUMED")
                continue
            if source_kind != "ACK_CACHE" and not service_cached:
                raise RuntimeError("SPEC199_NATIVE_SAMPLE_SOURCE_UNKNOWN")
            try:
                observer, source, target = edge
                captured = int(row["captured_at_ms"])
                expires = int(row["expires_at_ms"])
                offer = bindings[observer]
                if (not all(edge) or (source, target) not in allowed or source == target or
                        (not service_cached and (row["observer_peer"] != observer or
                         row["observer_boot_epoch"] != offer["boot_epoch"])) or
                        row["src_boot_epoch"] != bindings[source]["boot_epoch"] or
                        row["dst_boot_epoch"] != bindings[target]["boot_epoch"] or
                        (not service_cached and (not isinstance(row["topology_digest"], str) or
                         not row["topology_digest"].startswith("sha256:") or
                         row["topology_digest"] != offer["topology_digest"])) or
                        not 0 < captured <= checked_at < expires or
                        expires > (captured + 5000 if service_cached else
                                   min(captured + 5000, int(offer["expires_at_ms"]))) or
                        checked_at - captured > 5000 or
                        (rows is observed_rtt and observer != source) or
                        (rows is observed_goodput and observer != target)):
                    raise ValueError("cached sample binding or lifetime mismatch")
                if rows is observed_goodput:
                    train_rate = float(row["probe_train_goodput_mbps"])
                    if (row["probe_complete"] is not True or
                            row["probe_signature_valid"] is not True or
                            row["probe_source_timing_validated"] is not True or
                            row["probe_segments_sent"] != 16 or
                            row["probe_rate_status"] != "MEASURED_GOODPUT" or
                            not row["sample_digest"].startswith("sha256:") or
                            not math.isfinite(train_rate) or train_rate <= 0):
                        raise ValueError("unqualified dedicated train")
            except (KeyError, TypeError, ValueError, AttributeError) as error:
                raise RuntimeError("SPEC199_NATIVE_CACHED_SAMPLE_INVALID") from error
            (consumed_service if service_cached else cached).add(edge)
    if consumed_service_rtt != service_rtt or consumed_service_goodput != service_goodput:
        raise RuntimeError("SPEC199_NATIVE_CACHE_DATA_NOT_CONSUMED")
    if not rtt_rows and not goodput_rows or not dispatch and not cached_rows and not cache_hits:
        raise RuntimeError("SPEC196_NATIVE_PROBE_DATA_NOT_CONSUMED")
    return {
        "requestId": request_id,
        "probeDispatches": dispatch,
        "probeResponses": responses,
        "cacheQueryDispatches": cache_queries,
        "cacheQueryResponses": cache_responses,
        "cacheQueryResults": cache_results,
        "cachedServiceRttPairs": sorted(service_rtt),
        "cachedServiceGoodputEdges": sorted(service_goodput),
        "requestedRttPairs": sorted(requested_rtt),
        "requestedGoodputEdges": sorted(requested_goodput),
        "cachedAckRttPairs": sorted(cache_rtt),
        "cachedAckGoodputEdges": sorted(cache_goodput),
        "qualifiedGoodputEdges": sorted(requested_goodput | cache_goodput | service_goodput),
        "sampleSources": sorted({row.get("source_kind", "TARGETED_RESPONSE")
                                 for row in observed_rtt + observed_goodput}),
        "activeProbeRttRows": sorted(rtt_rows),
        "activeProbeGoodputRows": sorted(goodput_rows),
        "activeProbeGoodputSamples": sorted(goodput_samples),
        "activeCachedAckGoodputSamples": sorted({
            (str(row.get("reported_by", "")), str(row.get("src_provider", "")),
             str(row.get("dst_provider", "")), str(row.get("sample_digest", "")))
            for row in observed_goodput if row.get("source_kind") == "ACK_CACHE"}),
        "activeCachedServiceGoodputSamples": sorted({
            (str(row.get("reported_by", "")), str(row.get("src_provider", "")),
             str(row.get("dst_provider", "")), str(row.get("sample_digest", "")))
            for row in observed_goodput if row.get("source_kind") == "TARGETED_RESPONSE" and
            (str(row.get("reported_by", "")), str(row.get("src_provider", "")),
             str(row.get("dst_provider", ""))) in service_goodput}),
        "snapshotDigest": snapshot_row["snapshotDigest"],
    }


def validate_native_placement_evidence(
        user_text: str, provider_text: str, expected_roles: list[str], *,
        expected_manifest_digest: str, probe_evidence: dict[str, Any],
        expected_score_mode: str | None = None) -> dict[str, Any]:
    return _validate_native_placement_evidence(
        _EvidenceSnapshot(user_text), _EvidenceSnapshot(provider_text), expected_roles,
        expected_manifest_digest=expected_manifest_digest, probe_evidence=probe_evidence,
        expected_score_mode=expected_score_mode)


def _validate_native_placement_evidence(
        user_text: _EvidenceSnapshot, provider_text: _EvidenceSnapshot, expected_roles: list[str], *,
        expected_manifest_digest: str, probe_evidence: dict[str, Any],
        expected_score_mode: str | None = None) -> dict[str, Any]:
    """Require the committed assignment to consume a verified targeted sample."""
    request_id = probe_evidence["requestId"]
    snapshot_row = {"snapshotDigest": probe_evidence.get("snapshotDigest", "")}
    requested_goodput = set(map(tuple, probe_evidence.get("qualifiedGoodputEdges",
                                                        probe_evidence.get("requestedGoodputEdges", []))))
    goodput_samples = set(map(tuple, probe_evidence.get("activeProbeGoodputSamples", [])))
    commits = user_text.rows("NDNSF_DI_NATIVE_SELECTION_COMMITTED ", request_id)
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
    decisions = user_text.rows("NDNSF_DI_NATIVE_PLACEMENT_DECISION ", request_id)
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
    accepted = provider_text.rows("NDNSF_DI_NATIVE_SELECTION_ACCEPTED ", request_id)
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
        "qualifiedProbeCosts": probe_costs,
        "cachedAckProbeCosts": [cost for cost in probe_costs if any(
            (source, target, digest) == (cost.get("src_peer"), cost.get("dst_peer"), cost.get("sample_digest"))
            for observer, source, target, digest in probe_evidence.get("activeCachedAckGoodputSamples", []))],
        "cachedServiceProbeCosts": [cost for cost in probe_costs if any(
            (source, target, digest) == (cost.get("src_peer"), cost.get("dst_peer"), cost.get("sample_digest"))
            for observer, source, target, digest in probe_evidence.get("activeCachedServiceGoodputSamples", []))],
        "targetedProbeCosts": [cost for cost in probe_costs if not any(
            (source, target, digest) == (cost.get("src_peer"), cost.get("dst_peer"), cost.get("sample_digest"))
            for observer, source, target, digest in
            probe_evidence.get("activeCachedAckGoodputSamples", []) +
            probe_evidence.get("activeCachedServiceGoodputSamples", []))],
        "placementByRole": role_map,
    }


def native_placement_request_ids(user_text: str) -> list[str]:
    """Return network request identities in their original log order."""
    markers = _PLACEMENT_REQUEST_MARKERS
    return list(dict.fromkeys(
        request for line in user_text.splitlines() if any(marker in line for marker in markers)
        for request in [_evidence_fields(line).get("requestId", "")] if request))


def collect_native_placement_evidence(
        user_text: str, provider_text: str, expected_roles: list[str], *,
        expected_manifest_digest: str, expected_score_mode: str | None = None,
        request_id: str | None = None
        ) -> dict[str, Any]:
    return _collect_native_placement_evidence(
        _EvidenceSnapshot(user_text), _EvidenceSnapshot(provider_text), expected_roles,
        expected_manifest_digest=expected_manifest_digest,
        expected_score_mode=expected_score_mode, request_id=request_id)


def _collect_native_placement_evidence(
        user_text: _EvidenceSnapshot, provider_text: _EvidenceSnapshot, expected_roles: list[str], *,
        expected_manifest_digest: str, expected_score_mode: str | None = None,
        request_id: str | None = None
        ) -> dict[str, Any]:
    """Report probe and placement boundaries without running a model oracle."""
    requests = user_text.request_ids()
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
        probes = _validate_native_probe_evidence(user_text, request_id)
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
        placement = _validate_native_placement_evidence(
            user_text, provider_text, expected_roles,
            expected_manifest_digest=expected_manifest_digest,
            probe_evidence=probes, expected_score_mode=expected_score_mode)
        report.update(placement)
        report["boundaries"]["placement"] = {"status": "PASS"}
    except (RuntimeError, TypeError, ValueError, KeyError) as error:
        report["boundaries"]["placement"] = {"status": "FAIL", "reason": str(error)}
        # Preserve the actual score mode/fallback even when qualification fails.
        for row in user_text.rows("NDNSF_DI_NATIVE_PLACEMENT_DECISION ", request_id):
            if row.get("requestId") == request_id:
                try:
                    report["placementDecision"] = json.loads(row["decisionJson"])
                except (KeyError, TypeError, ValueError):
                    pass
    report["status"] = ("PASS" if all(row["status"] == "PASS"
                                    for row in report["boundaries"].values()) else "FAIL")
    return report
