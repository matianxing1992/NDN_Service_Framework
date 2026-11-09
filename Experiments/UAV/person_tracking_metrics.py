"""Independent offline metrics for GS-admitted indoor person tracking results.

This module reads evaluation labels only after execution. It never allocates
production tracking identities or substitutes for native signature admission.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
from typing import Any, Mapping

CAMERAS = ("UAV1", "UAV2", "UAV3")
INTERVAL_US = 500000


def _decode(raw):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError(f"duplicate JSON key: {key}")
            value[key] = item
        return value
    def invalid_constant(value):
        raise ValueError(f"invalid JSON constant: {value}")
    value = json.loads(raw, object_pairs_hook=unique, parse_constant=invalid_constant)
    if not isinstance(value, dict):
        raise ValueError("evaluation artifact must be an object")
    return value


def _load_with_digest(path: Path, limit: int) -> tuple[dict[str, Any], str]:
    with path.open("rb") as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise ValueError(f"oversize evaluation artifact: {path}")
    return _decode(raw), "sha256:" + hashlib.sha256(raw).hexdigest()


def _load(path: Path, limit: int) -> dict[str, Any]:
    return _load_with_digest(path, limit)[0]


def _is_digest(value):
    return (isinstance(value, str) and len(value) == 71 and value.startswith("sha256:") and
            all(c in "0123456789abcdef" for c in value[7:]))


def _assess_admitted_run(receipt, metrics, evaluation_digest, admission_path):
    """Bind explicit local data review without rewriting runtime evidence.

    This record is operator-controlled offline evidence, not authorization for
    execution or a substitute for the native admission of the received results.
    """
    admission, admission_digest = _load_with_digest(admission_path, 65536)
    if (admission.get("schema") != "uav-person-data-admission-v1" or
            admission.get("dataQualified") is not True or
            admission.get("datasetOrigin") not in ("real-captured", "synthetic-rendered")):
        raise ValueError("invalid independent data admission")
    for key in ("sourceManifestDigest", "profileDigest"):
        value = admission.get(key)
        if (not _is_digest(value) or
                value != metrics.get(key) or value != receipt.get(key)):
            raise ValueError("data admission binding mismatch: " + key)
    if (admission.get("evaluationManifestDigest") != evaluation_digest or
            admission["datasetOrigin"] != metrics.get("datasetOrigin") or
            admission["datasetOrigin"] != receipt.get("datasetOrigin")):
        raise ValueError("data admission evaluator/origin mismatch")
    evidence_path = admission.get("evidencePath")
    if not isinstance(evidence_path, str) or not evidence_path or len(evidence_path) > 4096:
        raise ValueError("data admission requires preparation audit path")
    evidence_path = Path(evidence_path)
    if not evidence_path.is_absolute():
        evidence_path = admission_path.parent / evidence_path
    evidence, evidence_digest = _load_with_digest(evidence_path, 16 * 1024 * 1024)
    if (evidence_digest != admission.get("evidenceDigest") or
            evidence.get("status") != "VERIFIED_ACTUAL_360_SOURCE_INPUTS_ONLY" or
            evidence.get("sourceDigest") != admission["sourceManifestDigest"] or
            evidence.get("evaluatorDigest") != evaluation_digest or
            not isinstance(evidence.get("frames"), list) or len(evidence["frames"]) != 360):
        raise ValueError("data admission preparation audit mismatch")
    observed = set()
    for frame in evidence["frames"]:
        if (not isinstance(frame, dict) or
                set(frame) != {"camera", "window", "path", "digest", "jpegBytes"} or
                frame["camera"] not in CAMERAS or type(frame["window"]) is not int or
                not 0 <= frame["window"] < 120 or
                not isinstance(frame["path"], str) or not 0 < len(frame["path"]) <= 4096 or
                "\0" in frame["path"] or not Path(frame["path"]).is_absolute() or
                not _is_digest(frame["digest"]) or
                type(frame["jpegBytes"]) is not int or not 0 < frame["jpegBytes"] <= 8 * 1024 * 1024):
            raise ValueError("invalid preparation audit frame record")
        key = frame["camera"], frame["window"]
        if key in observed:
            raise ValueError("duplicate preparation audit frame")
        observed.add(key)
    if observed != {(camera, window) for camera in CAMERAS for window in range(120)}:
        raise ValueError("incomplete preparation audit frame coverage")
    assessed = dict(receipt, dataQualified=True)
    # Installed production closure and native CPU telemetry establish execution
    # classification. An explicitly declared mock can never be reclassified.
    if receipt.get("synthetic") is None and receipt.get("installedClosureVerified") is True:
        candidate = dict(assessed, synthetic=False)
        gates = validate_person_run(candidate, metrics)["gates"]
        if all(gates[key] for key in ("scope", "datasetOrigin", "cpu")):
            assessed = candidate
    return assessed, {"dataAdmissionDigest": admission_digest,
                      "preparationAuditDigest": evidence_digest,
                      "dataQualified": assessed["dataQualified"],
                      "synthetic": assessed.get("synthetic"),
                      "basis": "Independent local data review; installed closure and native CPU telemetry"}


def _integer(value, minimum=0):
    if type(value) is not int or value < minimum or value > 2**63 - 1:
        raise ValueError("invalid nonnegative integer")
    return value


def _text(value):
    if not isinstance(value, str) or not value or len(value) > 1024:
        raise ValueError("invalid evaluation identifier")
    return value


def _box(value, width, height):
    if (not isinstance(value, list) or len(value) != 4 or
            any(type(x) not in (int, float) or not math.isfinite(x) for x in value)):
        raise ValueError("invalid finite box")
    x1, y1, x2, y2 = value
    if not (0 <= x1 < x2 <= width and 0 <= y1 < y2 <= height):
        raise ValueError("box outside frame or without area")
    return tuple(value)


def _intersection(a, b):
    return max(0, min(a[2], b[2]) - max(a[0], b[0])) * max(
        0, min(a[3], b[3]) - max(a[1], b[1]))


def _area(a):
    return (a[2] - a[0]) * (a[3] - a[1])


def _iou(a, b):
    intersection = _intersection(a, b)
    return intersection / (_area(a) + _area(b) - intersection)


def _assignment(matrix):
    # Rectangular assignment avoids a square allocation for thousands of new IDs.
    if not matrix or not matrix[0]:
        return []
    from scipy.optimize import linear_sum_assignment
    rows, columns = linear_sum_assignment(matrix, maximize=True)
    return [(int(i), int(j)) for i, j in zip(rows, columns) if matrix[i][j] > 0]


def _local_key(value):
    fields = ("cameraId", "sourceId", "sourceEpoch", "trackerGeneration", "byteTrackId")
    if not isinstance(value, dict) or set(value) != set(fields):
        raise ValueError("invalid full local key")
    if value["cameraId"] not in CAMERAS:
        raise ValueError("invalid local camera")
    return (_text(value["cameraId"]), _text(value["sourceId"]),
            *(_integer(value[k], 1) for k in fields[2:]))


def _validate_transitions(events, frames) -> set[str]:
    """Admit distinct departure/arrival endpoints from the frozen sampled GT."""
    if not isinstance(events, list) or len(events) > 20000:
        raise ValueError("invalid transition list")
    visibility = defaultdict(list)
    for (camera, sequence), frame in sorted(frames.items()):
        for person in frame["persons"]:
            visibility[camera, person["gtId"]].append((sequence, frame["commonPtsUs"]))

    def resolve(camera, identity, declared_pts):
        observations = visibility.get((camera, identity), ())
        if not observations:
            raise ValueError("transition has no sampled GT endpoint")
        distances = [abs(pts - declared_pts) for _, pts in observations]
        distance = min(distances)
        if distance > INTERVAL_US:
            raise ValueError("transition has no sampled GT endpoint")
        indices = [index for index, value in enumerate(distances) if value == distance]
        if len(indices) != 1:
            raise ValueError("ambiguous sampled GT transition endpoint")
        return observations, indices[0]

    event_ids, source_exits, target_entries = set(), set(), set()
    for event in events:
        eid = _text(event["eventId"])
        source, target = event["sourceCamera"], event["targetCamera"]
        if eid in event_ids or source not in CAMERAS or target not in CAMERAS or source == target:
            raise ValueError("duplicate/invalid transition")
        event_ids.add(eid)
        a, b = (_integer(event[k]) for k in ("sourceLastVisiblePts", "targetFirstVisiblePts"))
        if a > b or type(event["nonOverlap"]) is not bool:
            raise ValueError("invalid transition time/relation")
        gt = _text(event["globalGtId"])
        source_rows, source_index = resolve(source, gt, a)
        target_rows, target_index = resolve(target, gt, b)
        source_sequence, source_pts = source_rows[source_index]
        target_sequence, target_pts = target_rows[target_index]
        next_source = source_rows[source_index + 1] if source_index + 1 < len(source_rows) else None
        prior_target = target_rows[target_index - 1] if target_index else None
        if next_source and next_source[0] == source_sequence + 1:
            raise ValueError("transition source endpoint is not a visibility episode end")
        if prior_target and prior_target[0] == target_sequence - 1:
            raise ValueError("transition target endpoint is not a visibility episode start")
        if source_pts >= target_pts:
            raise ValueError("transition GT endpoints are simultaneous or reversed")
        if next_source and next_source[1] <= target_pts:
            raise ValueError("transition source reappears before target entry")
        if prior_target and prior_target[1] >= source_pts:
            raise ValueError("transition target is already visible at or after source exit")
        source_key, target_key = (source, gt, source_sequence), (target, gt, target_sequence)
        if source_key in source_exits or target_key in target_entries:
            raise ValueError("transition reuses a source exit or target entry")
        source_exits.add(source_key)
        target_entries.add(target_key)
    return event_ids


def _schedule(manifest):
    if manifest.get("schema") != "uav-person-evaluation-v1" or manifest.get("sampleHz") != 2:
        raise ValueError("unsupported evaluation schema/cadence")
    _text(manifest["dataset"])
    if ("datasetOrigin" in manifest and
            manifest["datasetOrigin"] not in ("real-captured", "synthetic-rendered")):
        raise ValueError("unsupported evaluation dataset origin")
    _text(manifest["sourceManifestDigest"])
    split = manifest["split"]
    if (split["name"] not in ("development", "evaluation") or
            type(split["frameDisjoint"]) is not bool or
            type(split["sharedActorsDisclosed"]) is not bool):
        raise ValueError("invalid frozen development/evaluation split")
    windows = manifest["windows"]
    if not isinstance(windows, list) or not 1 <= len(windows) <= 120:
        raise ValueError("evaluation requires 1..120 frozen windows")
    if _integer(manifest["durationUs"], 1) != len(windows) * INTERVAL_US:
        raise ValueError("duration does not match all scheduled windows")
    frames, identities, names = {}, set(), set()
    first_target = None
    for index, window in enumerate(windows):
        name = _text(window["windowId"])
        sequence = _integer(window["sequence"], 1)
        if name in names or sequence != index + 1:
            raise ValueError("duplicate/noncontiguous scheduled window")
        names.add(name)
        target = _integer(window["targetTimeUs"])
        if first_target is None:
            first_target = target
        if target != first_target + index * INTERVAL_US:
            raise ValueError("GT target sampling cadence mismatch")
        if len(window["frames"]) != 3:
            raise ValueError("three GT frames required for every window")
        for frame in window["frames"]:
            camera = frame["cameraId"]
            if camera not in CAMERAS or (camera, sequence) in frames:
                raise ValueError("duplicate/unknown scheduled camera")
            pts = _integer(frame["commonPtsUs"])
            if not target <= pts <= target + 100000:
                raise ValueError("GT actual frame PTS outside target sampling tolerance")
            width, height = _integer(frame["width"], 1), _integer(frame["height"], 1)
            if width > 1920 or height > 1080 or len(frame["persons"]) > 64:
                raise ValueError("evaluation frame bound exceeded")
            seen = set()
            for person in frame["persons"]:
                identity = _text(person["gtId"])
                if identity in seen:
                    raise ValueError("duplicate GT identity in observation")
                seen.add(identity)
                identities.add(identity)
                _box(person["box"], width, height)
            for box in frame["ignoreRegions"]:
                _box(box, width, height)
            frames[camera, sequence] = frame
    if len(identities) > 256:
        raise ValueError("GT identity bound exceeded")
    event_ids = _validate_transitions(manifest["transitions"], frames)
    for excluded in manifest["excludedEvents"]:
        eid = _text(excluded["eventId"])
        _text(excluded["reason"])
        if eid in event_ids:
            raise ValueError("excluded transition also included or duplicated")
        event_ids.add(eid)
    return frames, sorted(identities)


def _received(root, manifest, scheduled):
    context = _load(root / "context.json", 65536)
    if context.get("schema") != "uav-person-display-context-v1":
        raise ValueError("missing frozen GS context")
    if context["windowCount"] != len(manifest["windows"]):
        raise ValueError("GS schedule count mismatch")
    generations = context["generations"]
    if (not generations or generations[0]["firstWindowIndex"] != 0 or
            any(a["firstWindowIndex"] >= b["firstWindowIndex"]
                for a, b in zip(generations, generations[1:]))):
        raise ValueError("invalid frozen generation schedule")
    expected_windows = {w["windowId"]: w for w in manifest["windows"]}
    predictions, links, received_windows, seen_paths = {}, [], [], set()
    devices = []
    paths = list(root.glob("*/presentation.json"))
    if len(paths) > 120:
        raise ValueError("received window bound exceeded")
    for path in sorted(paths):
        projection = _load(path, 8 * 1024 * 1024)
        if projection.get("schema") != "uav-person-display-v1" or projection.get("state") != "ready":
            raise ValueError("not a ready GS presentation")
        for field in ("runId", "missionId", "profileDigest", "computationDigest"):
            if projection[field] != context[field]:
                raise ValueError("GS context/projection mismatch")
        if projection["responseProvider"] != "/example/uav/compute":
            raise ValueError("unexpected compute provider")
        raw = projection["resultJson"]
        if (not isinstance(raw, str) or projection["resultDigest"] !=
                "sha256:" + hashlib.sha256(raw.encode()).hexdigest()):
            raise ValueError("GS original result digest mismatch")
        result = _decode(raw)
        wid = result["windowId"]
        if wid not in expected_windows or wid in seen_paths or path.parent.name != wid:
            raise ValueError("unexpected/duplicate received window")
        seen_paths.add(wid)
        schedule = expected_windows[wid]
        seq = schedule["sequence"]
        if result.get("schema") != "uav-person-tracking-result-v1" or result["sequence"] != seq:
            raise ValueError("wrong result schema/sequence")
        for field in ("runId", "missionId", "profileDigest", "computationDigest"):
            if result[field] != context[field]:
                raise ValueError("result scope mismatch")
        if result.get("terminal") is not True or result.get("trackingMode") != "indoor-person-reid":
            raise ValueError("nonterminal/wrong-mode result")
        generation = max((g for g in generations if g["firstWindowIndex"] <= seq - 1),
                         key=lambda g: g["firstWindowIndex"])
        if len(result["frames"]) != 3:
            raise ValueError("three received frames required")
        event_rows = []
        seen_cameras = set()
        for frame in result["frames"]:
            camera = frame["cameraId"]
            if camera not in CAMERAS or camera in seen_cameras:
                raise ValueError("duplicate/unknown received camera")
            seen_cameras.add(camera)
            gt_frame = scheduled[camera, seq]
            for field in ("commonPtsUs", "width", "height"):
                if frame[field] != gt_frame[field]:
                    raise ValueError("received/GT sampled frame mismatch")
            if (frame["sequence"] != seq or frame["sourceEpoch"] != generation["sourceEpoch"] or
                    frame["sourceId"] != generation["sourceIds"][CAMERAS.index(camera)]):
                raise ValueError("received source generation mismatch")
            if len(frame["tracks"]) > context["maxPersonsPerFrame"]:
                raise ValueError("received track bound exceeded")
            rows, gids, locals_seen = [], set(), set()
            for track in frame["tracks"]:
                box = _box(track["box"], frame["width"], frame["height"])
                key = None
                if track["byteTrackId"] is not None:
                    key = _local_key({k: track[k] for k in (
                        "cameraId", "sourceId", "sourceEpoch", "trackerGeneration", "byteTrackId")})
                    if (key[:3] != (camera, frame["sourceId"], frame["sourceEpoch"]) or
                            key[3] != frame["trackerGeneration"] or key in locals_seen):
                        raise ValueError("duplicate/mismatched local track")
                    locals_seen.add(key)
                gid = track["globalId"]
                if gid is not None:
                    _integer(gid, 1)
                    if gid in gids or key is None:
                        raise ValueError("duplicate global identity in observation")
                    gids.add(gid)
                if track["ptsUs"] != frame["commonPtsUs"]:
                    raise ValueError("track sample PTS mismatch")
                origin = track["associationOrigin"]
                if (track["associationStatus"] == "MATCHED") != (origin is not None):
                    raise ValueError("match origin missing or extraneous")
                if origin is not None:
                    origin_key = _local_key(origin["localKey"])
                    origin_index = CAMERAS.index(origin_key[0])
                    if origin_key[1:3] != (generation["sourceIds"][origin_index], generation["sourceEpoch"]):
                        raise ValueError("origin source generation mismatch")
                    origin_pts = _integer(origin["ptsUs"])
                    if type(origin["active"]) is not bool or origin_pts > track["ptsUs"]:
                        raise ValueError("invalid origin time/activity")
                    if key is None or gid is None or origin_key == key:
                        raise ValueError("invalid matched identity")
                    links.append({"sourceKey": origin_key, "targetKey": key,
                                  "sourcePts": origin_pts, "targetPts": track["ptsUs"],
                                  "globalId": gid, "active": origin["active"]})
                event_rows.append({"cameraId": camera, "byteTrackId": track["byteTrackId"],
                                   "globalId": gid, "status": track["associationStatus"],
                                   "distance": track["appearanceDistance"],
                                   "targetLocalKey": {k: track[k] for k in (
                                       "cameraId", "sourceId", "sourceEpoch", "trackerGeneration",
                                       "byteTrackId")}, "ptsUs": track["ptsUs"], "origin": origin})
                rows.append({"box": box, "globalId": gid, "key": key, "pts": track["ptsUs"]})
            predictions[camera, seq] = rows
        if result["associationEvents"] != event_rows:
            raise ValueError("association event/track mirror mismatch")
        devices.append({k: result[k] for k in ("requestedDevice", "actualDetectorDevice",
                                              "actualReidDevice", "reidInferenceBatches")})
        received_windows.append(wid)
    return context, predictions, links, received_windows, devices


def evaluate_person_tracking(received: Path, evaluation_manifest: Path, *,
                             run_receipt: Path | None = None,
                             data_admission: Path | None = None) -> dict[str, Any]:
    """Score every scheduled observation, including absent GS windows as misses."""
    if data_admission is not None and run_receipt is None:
        raise ValueError("data admission assessment requires an execution receipt")
    manifest, evaluation_digest = _load_with_digest(evaluation_manifest, 16 * 1024 * 1024)
    frames, gt_ids = _schedule(manifest)
    context, predictions, links, received_windows, devices = _received(received, manifest, frames)
    counts, endpoint_gt, previous = defaultdict(int), {}, {}
    n_gt = n_pred = detection_tp = switches = ignored = 0
    for (camera, seq), frame in sorted(frames.items(), key=lambda item: (item[0][1], item[0][0])):
        gt = frame["persons"]
        rows = []
        for row in predictions.get((camera, seq), []):
            candidate = any(_iou(row["box"], p["box"]) >= .5 for p in gt)
            if not candidate and any(_intersection(row["box"], box) / _area(row["box"]) >= .5
                                     for box in frame["ignoreRegions"]):
                ignored += 1
                continue
            rows.append(row)
        n_gt += len(gt)
        n_pred += len(rows)
        weights = [[(1 + _iou(p["box"], r["box"]) / (min(len(gt), len(rows)) + 1)
                     if _iou(p["box"], r["box"]) >= .5 else 0) for r in rows] for p in gt]
        pairs = _assignment(weights)
        detection_tp += len(pairs)
        for i, j in pairs:
            gid, pts, gt_id = rows[j]["globalId"], frame["commonPtsUs"], gt[i]["gtId"]
            old = previous.get((camera, gt_id))
            if gid is not None:
                if old and pts - old[0] <= 30000000 and gid != old[1]:
                    switches += 1
                previous[camera, gt_id] = pts, gid
        for p in gt:
            for row in rows:
                if _iou(p["box"], row["box"]) >= .5:
                    if row["globalId"] is not None:
                        counts[p["gtId"], row["globalId"]] += 1
                    if row["key"] is not None:
                        endpoint_gt.setdefault((row["key"], row["pts"], row["globalId"]), set()).add(p["gtId"])
    pred_ids = sorted({p for _, p in counts})
    matrix = [[counts.get((g, p), 0) for p in pred_ids] for g in gt_ids]
    chosen = _assignment(matrix)
    idtp = sum(matrix[i][j] for i, j in chosen)
    claimed, scored_links, coview, dedup = set(), [], [], set()
    for link in sorted(links, key=lambda x: (x["targetPts"], x["sourceKey"], x["targetKey"])):
        source, target = link["sourceKey"], link["targetKey"]
        if source[0] == target[0]:
            continue
        key = source, target
        if key in dedup:
            continue
        dedup.add(key)
        if link["active"] or link["sourcePts"] == link["targetPts"]:
            coview.append(link)
            continue
        origin_gt = endpoint_gt.get((source, link["sourcePts"], link["globalId"]), set())
        target_gt = endpoint_gt.get((target, link["targetPts"], link["globalId"]), set())
        candidates = []
        for index, event in enumerate(manifest["transitions"]):
            a = abs(event["sourceLastVisiblePts"] - link["sourcePts"])
            b = abs(event["targetFirstVisiblePts"] - link["targetPts"])
            if (index not in claimed and event["sourceCamera"] == source[0] and
                    event["targetCamera"] == target[0] and a <= INTERVAL_US and b <= INTERVAL_US and
                    event["globalGtId"] in origin_gt & target_gt):
                candidates.append((a + b, index))
        matched = min(candidates)[1] if candidates else None
        if matched is not None:
            claimed.add(matched)
        scored_links.append({**link, "eventId": manifest["transitions"][matched]["eventId"]
                             if matched is not None else None})
    tp, fp, fn = len(claimed), len(scored_links) - len(claimed), len(manifest["transitions"]) - len(claimed)
    ratio = lambda a, b: a / b if b else 0.0
    result = {"schema": "uav-person-metrics-v1", "runId": context["runId"],
            "missionId": context["missionId"], "dataset": manifest["dataset"],
            "datasetOrigin": manifest.get("datasetOrigin", "unspecified"),
            "profileDigest": context["profileDigest"], "computationDigest": context["computationDigest"],
            "sourceManifestDigest": manifest["sourceManifestDigest"], "split": manifest["split"],
            "durationUs": manifest["durationUs"], "gtIdentityCount": len(gt_ids),
            "plannedWindows": len(manifest["windows"]), "receivedWindows": len(received_windows),
            "plannedFrames": len(frames), "receivedFrames": len(predictions),
            "missingWindows": [w["windowId"] for w in manifest["windows"] if w["windowId"] not in received_windows],
            "Ngt": n_gt, "Npred": n_pred, "IDTP": idtp, "IDFP": n_pred - idtp,
            "IDFN": n_gt - idtp, "IDF1": ratio(2 * idtp, n_gt + n_pred),
            "detectionTP": detection_tp, "detectionFP": n_pred - detection_tp,
            "detectionFN": n_gt - detection_tp,
            "detectionPrecision": ratio(detection_tp, n_pred), "detectionRecall": ratio(detection_tp, n_gt),
            "idSwitches": switches, "ignoredPredictions": ignored,
            "identityCounts": [{"gtId": g, "globalId": p, "count": c}
                               for (g, p), c in sorted(counts.items()) if c],
            "identityAssignment": [{"gtId": gt_ids[i], "globalId": pred_ids[j], "count": matrix[i][j]}
                                   for i, j in chosen],
            "crossCameraTP": tp, "crossCameraFP": fp, "crossCameraFN": fn,
            "crossCameraPrecision": ratio(tp, tp + fp), "crossCameraRecall": ratio(tp, tp + fn),
            "eligibleTransitions": len(manifest["transitions"]),
            "nonOverlapTransitions": sum(e["nonOverlap"] for e in manifest["transitions"]),
            "links": scored_links, "coViewLinks": coview, "devices": devices,
            "unmatchedEvents": [e["eventId"] for i, e in enumerate(manifest["transitions"]) if i not in claimed],
            "qualification": "not-assessed; requires maintained execution receipt"}
    if run_receipt is not None:
        receipt, receipt_digest = _load_with_digest(run_receipt, 16 * 1024 * 1024)
        if data_admission is not None:
            receipt, assessment = _assess_admitted_run(
                receipt, result, evaluation_digest, data_admission)
            result["qualificationEvidence"] = dict(assessment, rawRunReceiptDigest=receipt_digest)
        qualification = validate_person_run(receipt, result)
        if not qualification["gates"]["scope"]:
            raise ValueError("execution receipt does not bind these metrics")
        result["runtimeTimingSummary"] = receipt.get("timing", {}).get("summary")
        result["runtimeResourceSummary"] = receipt.get("resourceMeasurements")
        result["runtimeCounts"] = receipt.get("counts")
        result["effectiveProcessedFramesPerSecond"] = receipt.get("effectiveProcessedFramesPerSecond")
        result["qualification"] = qualification
    return result


def validate_person_run(receipt: Mapping[str, Any], metrics: Mapping[str, Any]) -> dict[str, Any]:
    """Fail closed on missing run evidence; scores alone never qualify a run."""
    gates = {
        "schema": metrics.get("schema") == "uav-person-metrics-v1" and
                  receipt.get("schema") == "uav-person-run-receipt-v1",
        "scope": all(receipt.get(k) == metrics.get(k) and metrics.get(k) is not None for k in
                     ("runId", "missionId", "profileDigest", "computationDigest", "sourceManifestDigest")),
        "datasetOrigin": metrics.get("datasetOrigin") in ("real-captured", "synthetic-rendered") and
                         receipt.get("datasetOrigin") == metrics.get("datasetOrigin"),
        "data": receipt.get("dataQualified") is True and metrics.get("durationUs") == 60000000 and
                metrics.get("gtIdentityCount", 0) >= 3 and metrics.get("eligibleTransitions", 0) >= 20 and
                metrics.get("nonOverlapTransitions", 0) >= 5 and metrics.get("Ngt", 0) > 0,
        "split": metrics.get("split", {}).get("name") == "evaluation" and
                 metrics.get("split", {}).get("frameDisjoint") is True and
                 metrics.get("split", {}).get("sharedActorsDisclosed") is True,
        "windows": all(metrics.get(k) == v for k, v in
                       (("plannedWindows", 120), ("receivedWindows", 120),
                        ("plannedFrames", 360), ("receivedFrames", 360))) and not metrics.get("missingWindows"),
        "accuracy": metrics.get("IDF1", 0) >= .8 and metrics.get("crossCameraPrecision", 0) >= .95 and
                    metrics.get("crossCameraRecall", 0) >= .8,
        "closure": receipt.get("installedClosureVerified") is True and receipt.get("synthetic") is False,
        "cpu": len(metrics.get("devices", [])) == 120 and all(
            d.get("requestedDevice") == d.get("actualDetectorDevice") == "cpu" and
            type(d.get("reidInferenceBatches")) is int and d["reidInferenceBatches"] >= 0 and
            d.get("actualReidDevice") == ("cpu" if d["reidInferenceBatches"] else "not-run")
            for d in metrics.get("devices", [])) and
            any(d.get("reidInferenceBatches", 0) > 0 for d in metrics.get("devices", [])),
        "timing": receipt.get("completeFrameTimingRecords") == 360 and
                  receipt.get("completeWindowTimingRecords") == 120 and
                  receipt.get("timingQualified") is True,
        "resources": receipt.get("resourceMeasurementsComplete") is True,
        "deadline": type(receipt.get("elapsedSeconds")) in (int, float) and
                    0 < receipt["elapsedSeconds"] <= 900,
        "cleanup": receipt.get("remainingOwnedProcesses") == 0 and
                   receipt.get("cleanupErrors") == [] and
                   type(receipt.get("cleanupSeconds")) in (int, float) and
                   0 <= receipt["cleanupSeconds"] <= 10,
        "terminal": receipt.get("terminal") is True and receipt.get("failedWindows") == 0,
    }
    return {"qualified": all(gates.values()), "gates": gates,
            "failedGates": [k for k, passed in gates.items() if not passed]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--received", type=Path, required=True)
    parser.add_argument("--evaluation-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-receipt", type=Path)
    parser.add_argument("--data-admission", type=Path,
                        help="independently reviewed local data record; used only for offline assessment")
    args = parser.parse_args()
    result = evaluate_person_tracking(args.received, args.evaluation_manifest,
                                      run_receipt=args.run_receipt, data_admission=args.data_admission)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
