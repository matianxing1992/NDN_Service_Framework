"""Join actual host-clock diagnostics; missing observations remain missing."""
from __future__ import annotations

import json
import hashlib
import math
from bisect import bisect_left
import os
from pathlib import Path
from typing import Any, Mapping

CAMERAS = ("UAV1", "UAV2", "UAV3")
DOMAIN = "CLOCK_MONOTONIC_NS"
FRAME_EVENTS = {"capture": "captureNs", "publication-start": "publicationStartNs",
                "published": "publishedNs", "fetch-start": "fetchStartNs",
                "admitted": "admittedNs", "received": "receivedNs"}
WINDOW_EVENTS = {"window-request": "requestNs", "worker-start": "workerStartNs",
                 "worker-end": "workerEndNs", "association-start": "associationStartNs",
                 "association-end": "associationEndNs", "committed": "committedNs"}
MODEL_POINTS = ("modelInitStartNs", "modelInitEndNs", "inferenceStartNs", "inferenceEndNs")


def _uint(value, maximum=2**63 - 1):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError("invalid host timestamp/index")
    return value


def _decode(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON telemetry field")
            result[key] = value
        return result
    def invalid(value):
        raise ValueError("nonfinite JSON telemetry value")
    return json.loads(raw, object_pairs_hook=unique, parse_constant=invalid)


def _read_bytes(path, bound):
    with path.open("rb") as stream:
        raw = stream.read(bound + 1)
    if len(raw) > bound:
        raise ValueError("oversize runtime artifact")
    return raw


def _read_json(path, bound=65536):
    return _decode(_read_bytes(path, bound))


def _percentiles(values):
    if not values:
        return None
    values = sorted(values)
    def percentile(fraction):
        index = (len(values) - 1) * fraction
        lo, hi = math.floor(index), math.ceil(index)
        return values[lo] + (values[hi] - values[lo]) * (index - lo)
    return {"count": len(values), "p50": percentile(.5), "p95": percentile(.95), "max": values[-1]}


def audit_person_processes(run_root: Path) -> dict[str, Any]:
    """Read-only /proc scope check after cleanup; exclude this launcher and ancestors."""
    excluded, pid = set(), os.getpid()
    while pid and pid not in excluded:
        excluded.add(pid)
        try:
            fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            pid = int(fields[1])
        except (OSError, ValueError, IndexError):
            break
    root = os.fsencode(str(run_root.resolve()))
    remaining, errors = [], []
    for process in Path("/proc").iterdir():
        if not process.name.isdigit() or int(process.name) in excluded:
            continue
        try:
            with (process / "cmdline").open("rb") as stream:
                command = stream.read(65537)
            args = command.split(b"\0")
            if any(arg.split(b"=", 1)[-1] == root or
                   arg.split(b"=", 1)[-1].startswith(root + b"/") for arg in args):
                remaining.append({"pid": int(process.name),
                                  "command": command[:4096].replace(b"\0", b" ").decode(errors="replace")})
        except FileNotFoundError:
            pass  # Process exited during the read.
        except OSError as error:
            if len(errors) < 64:
                errors.append(f"/proc/{process.name}: {type(error).__name__}")
    return {"remainingOwnedProcesses": len(remaining), "remaining": remaining,
            "auditErrors": errors, "scope": "commands with exact run-root argument/path prefix"}


def collect_person_timing(run_root: Path, plan: Mapping[str, Any]) -> dict[str, Any]:
    """Keep every frozen source observation, without reading labels or private model state."""
    prepared = _read_json(run_root / "prepared-person-sources.json")
    count = _uint(prepared["windowCount"])
    if not 1 <= count <= 120 or count != plan["settings"]["windowCount"]:
        raise ValueError("unsupported/inconsistent person timing schedule")
    if prepared["sourceEpoch"] != plan["observation"]["sourceEpoch"]:
        raise ValueError("timing preparation/plan generation mismatch")
    epoch, start = _uint(prepared["sourceEpoch"], 2**64 - 1), _uint(prepared["commonStartUs"])
    sources = prepared["sourceIds"]
    if sources != plan["observation"]["sourceIds"]:
        raise ValueError("timing preparation/plan source mismatch")
    sampling_path = run_root / "source-frame-inputs.json"
    sampling_raw = _read_bytes(sampling_path, 1024 * 1024)
    if ("sha256:" + hashlib.sha256(sampling_raw).hexdigest() !=
            prepared["sourceFrameInputsDigest"]):
        raise ValueError("frozen timing sampling digest mismatch")
    sampling = _decode(sampling_raw)
    for camera in CAMERAS:
        for field in ("captureTimeUs", "targetTimeUs", "inputDigests"):
            if len(sampling["sampling"][camera][field]) != count:
                raise ValueError("incomplete frozen timing sampling schedule")
        for index, actual in enumerate(sampling["sampling"][camera]["captureTimeUs"]):
            target = sampling["sampling"][camera]["targetTimeUs"][index]
            if (_uint(target) != start + index * 500000 or
                    not target <= _uint(actual) <= target + 100000):
                raise ValueError("invalid frozen actual/target sampling PTS")
    windows = [{"windowId": f"window-{i}", "sequence": i + 1, "sourceEpoch": epoch,
                **dict.fromkeys(WINDOW_EVENTS.values()), **dict.fromkeys(MODEL_POINTS), "workerPid": None} for i in range(count)]
    frames = {(i + 1, camera): {"windowId": f"window-{i}", "sequence": i + 1,
              "cameraId": camera, "sourceId": sources[camera], "sourceEpoch": epoch,
              "commonPtsUs": sampling["sampling"][camera]["captureTimeUs"][i],
              "targetTimeUs": sampling["sampling"][camera]["targetTimeUs"][i],
              "inputDigest": sampling["sampling"][camera]["inputDigests"][i],
              **dict.fromkeys(FRAME_EVENTS.values()),
              "displayNs": None} for i in range(count) for camera in CAMERAS}
    errors, origin, records = [], None, 0
    logs = [(f"uav{i + 1}-camera.log", camera) for i, camera in enumerate(CAMERAS)]
    logs += [("compute.log", "compute"), ("ground-station.log", "gs"),
             ("ground-station-display.log", "display")]
    def put(target, field, value):
        value = _uint(value)
        if target[field] is not None:
            raise ValueError("duplicate timing point: " + field)
        target[field] = value
    for filename, actor in logs:
        path = run_root / filename
        if not path.exists():
            errors.append("missing log: " + filename)
            continue
        with path.open("rb") as stream:
            raw = stream.read(32 * 1024 * 1024 + 1)
        if len(raw) > 32 * 1024 * 1024:
            errors.append("oversize log: " + filename)
            continue
        for line in raw.splitlines():
            if line.startswith(b"SPEC219_TRACE_DROPPED"):
                if len(errors) < 128:
                    errors.append(filename + ": native timing trace dropped")
                continue
            native = line.startswith(b"SPEC219_TRACE ")
            payload = line[len(b"SPEC219_TRACE "):] if native else line
            if not native and (not payload.startswith(b"{") or not any(
                    marker in payload for marker in (b'"PERSON_BACKEND_COMPLETED"', b'"DISPLAY_FRAME_SHOWN"'))):
                continue
            try:
                if len(payload) > 65536:
                    raise ValueError("oversize telemetry line")
                value = _decode(payload)
                if not isinstance(value, dict):
                    continue
                event = value.get("event")
                if not native and event not in ("PERSON_BACKEND_COMPLETED", "DISPLAY_FRAME_SHOWN"):
                    continue
                if event == "DISPLAY_FRAME_SHOWN" and value.get("personDisplay") is None:
                    continue
                records += 1
                if records > 8192:
                    raise ValueError("telemetry count exceeds bound")
                if (value.get("runId") != run_root.name or
                        value.get("missionId", value.get("sessionId")) != run_root.name or
                        value.get("clockDomain") != DOMAIN):
                    raise ValueError("runtime trace scope/clock mismatch")
                if native and value.get("schema") != "uav-person-runtime-trace-v1":
                    raise ValueError("runtime trace schema mismatch")
                if event == "replay-origin":
                    if actor != "gs" or origin is not None:
                        raise ValueError("duplicate/wrong-actor replay origin")
                    origin = _uint(value["monotonicNs"])
                    continue
                sequence = _uint(value["sequence"])
                if not 1 <= sequence <= count:
                    raise ValueError("trace outside planned schedule")
                if event == "DISPLAY_FRAME_SHOWN":
                    if (actor != "display" or type(value["window"]) is not int or
                            value["window"] != sequence - 1 or
                            _uint(value["epoch"], 2**64 - 1) != epoch):
                        raise ValueError("display scope mismatch")
                elif (value.get("windowId") != f"window-{sequence - 1}" or
                      _uint(value.get("sourceEpoch"), 2**64 - 1) != epoch):
                    raise ValueError("trace window/epoch mismatch")
                if event in FRAME_EVENTS or event == "DISPLAY_FRAME_SHOWN":
                    camera = value["cameraId"]
                    target = frames[sequence, camera]
                    expected_actor = (camera if event in ("capture", "publication-start", "published") else
                                      "compute" if event in ("fetch-start", "admitted") else
                                      "gs" if event == "received" else "display")
                    if actor != expected_actor:
                        raise ValueError("trace actor mismatch")
                    if event != "DISPLAY_FRAME_SHOWN" and value.get("sourceId") != sources[camera]:
                        raise ValueError("trace source mismatch")
                    if event != "fetch-start" and _uint(value.get(
                            "ptsUs" if event == "DISPLAY_FRAME_SHOWN" else "commonPtsUs")) != target["commonPtsUs"]:
                        raise ValueError("trace media PTS mismatch")
                    put(target, "displayNs" if event == "DISPLAY_FRAME_SHOWN" else FRAME_EVENTS[event],
                        value["monotonicNs"])
                elif event in WINDOW_EVENTS:
                    if actor != ("gs" if event == "window-request" else "compute"):
                        raise ValueError("window trace actor mismatch")
                    put(windows[sequence - 1], WINDOW_EVENTS[event], value["monotonicNs"])
                elif event == "PERSON_BACKEND_COMPLETED":
                    if actor != "compute":
                        raise ValueError("backend trace actor mismatch")
                    if value.get("actualDevice") != plan["personTracking"]["requestedDevice"]:
                        raise ValueError("backend device mismatch")
                    for field in MODEL_POINTS:
                        put(windows[sequence - 1], field, value[field])
                    if "pid" in value:
                        if _uint(value["pid"]) == 0:
                            raise ValueError("invalid backend process PID")
                        put(windows[sequence - 1], "workerPid", value["pid"])
                    if "modelResourceSample" in value:
                        windows[sequence - 1]["modelResourceSample"] = value["modelResourceSample"]
                elif event == "cached":
                    raise ValueError("cached attempt is not a fresh inference timing sample")
                else:
                    raise ValueError("unsupported timing event")
            except (ValueError, KeyError, TypeError) as error:
                if len(errors) < 128:
                    errors.append(f"{filename}: {error}")
            if records > 8192:
                break
    complete_windows = 0
    metrics = {k: [] for k in ("e2eMs", "displayMs", "captureToDisplayMs", "replayLagMs",
                               "retrievalAndValidationMs", "admittedToWorkerMs", "workerMs",
                               "associationMs", "modelInitializationMs", "modelWindowMs")}
    for window in windows:
        values = [origin] + [window[k] for k in ("requestNs", "workerStartNs", *MODEL_POINTS,
                  "workerEndNs", "associationStartNs", "associationEndNs", "committedNs")]
        window["complete"] = all(v is not None for v in values)
        if window["complete"] and values != sorted(values):
            errors.append("noncausal window timing: " + window["windowId"])
            window["complete"] = False
        complete_windows += window["complete"]
        if window["complete"]:
            for name, a, b in (("workerMs", "workerStartNs", "workerEndNs"),
                               ("associationMs", "associationStartNs", "associationEndNs"),
                               ("modelInitializationMs", "modelInitStartNs", "modelInitEndNs"),
                               ("modelWindowMs", "inferenceStartNs", "inferenceEndNs")):
                metrics[name].append((window[b] - window[a]) / 1000000)
    complete_frames = 0
    for (sequence, camera), frame in frames.items():
        window = windows[sequence - 1]
        frame["scheduledCaptureNs"] = origin + (frame["targetTimeUs"] - start) * 1000 if origin is not None else None
        chain = [frame["captureNs"], frame["publicationStartNs"], frame["admittedNs"],
                 window["workerStartNs"], window["workerEndNs"], window["associationEndNs"],
                 window["committedNs"], frame["receivedNs"], frame["displayNs"]]
        required = [*chain, frame["publishedNs"], frame["fetchStartNs"], frame["scheduledCaptureNs"]]
        frame["complete"] = window["complete"] and all(v is not None for v in required)
        if frame["complete"] and (chain != sorted(chain) or
                frame["publishedNs"] < frame["publicationStartNs"] or
                frame["fetchStartNs"] > frame["admittedNs"] or
                window["requestNs"] > frame["captureNs"] or
                window["requestNs"] > frame["fetchStartNs"]):
            errors.append(f"noncausal frame timing: {camera}/{sequence}")
            frame["complete"] = False
        complete_frames += frame["complete"]
        if frame["complete"]:
            metrics["e2eMs"].append((frame["receivedNs"] - frame["scheduledCaptureNs"]) / 1000000)
            metrics["displayMs"].append((frame["displayNs"] - frame["receivedNs"]) / 1000000)
            metrics["captureToDisplayMs"].append((frame["displayNs"] - frame["captureNs"]) / 1000000)
            metrics["replayLagMs"].append((frame["captureNs"] - frame["scheduledCaptureNs"]) / 1000000)
            metrics["retrievalAndValidationMs"].append((frame["admittedNs"] - frame["fetchStartNs"]) / 1000000)
            metrics["admittedToWorkerMs"].append((window["workerStartNs"] - frame["admittedNs"]) / 1000000)
    return {"schema": "uav-person-run-timing-v1", "clockDomain": DOMAIN,
            "runId": run_root.name, "missionId": run_root.name,
            "timeBasis": "GS-bootstrap-replay-origin-plus-common-media-offset",
            "plannedFrames": 3 * count, "plannedWindows": count,
            "completeFrameTimingRecords": complete_frames, "completeWindowTimingRecords": complete_windows,
            "timingQualified": not errors and complete_frames == 3 * count and complete_windows == count,
            "frames": list(frames.values()), "windows": windows, "errors": errors,
            "summary": {key: _percentiles(values) for key, values in metrics.items()}}


def validate_person_resources(resource: Mapping[str, Any], timing: Mapping[str, Any],
                              expected_roots: set[str]) -> dict[str, Any]:
    """Check measured scope and model-loaded sample coverage, without inventing peaks."""
    if not isinstance(resource, Mapping):
        raise ValueError("resource artifact must be an object")
    if (resource.get("schema") != "uav-person-resources-v1" or
            resource.get("clockDomain") != DOMAIN or
            any(resource.get(k) != timing[k] for k in ("runId", "missionId"))):
        raise ValueError("resource scope/clock mismatch")
    if not isinstance(resource["errors"], list) or len(resource["errors"]) > 64:
        raise ValueError("invalid resource errors")
    errors = list(resource["errors"])
    roots = resource["registeredRoots"]
    if not isinstance(roots, Mapping):
        raise ValueError("invalid resource roots")
    if len(roots) > 64:
        raise ValueError("resource root bound exceeded")
    if set(roots) != expected_roots:
        errors.append("resource roots do not match configured applications/NFDs")
    for pid in roots.values():
        if _uint(pid, 2**31 - 1) == 0:
            raise ValueError("invalid resource root PID")
    samples, processes = resource["samples"], resource["processes"]
    if (not isinstance(samples, list) or not isinstance(processes, list) or
            not 1 <= len(samples) <= 10000 or len(processes) > 512):
        raise ValueError("resource sample/process bound")
    by_id, by_pid, sample_times, process_keys = {}, {}, {}, set()
    for process in processes:
        if not isinstance(process, Mapping):
            raise ValueError("invalid resource process entry")
        identity, pid = _uint(process["identityId"]), _uint(process["pid"], 2**31 - 1)
        created = process["creationTimeUnixSeconds"]
        if (identity == 0 or identity > 512 or pid == 0 or identity in by_id or
                type(created) not in (int, float) or not math.isfinite(created) or created <= 0):
            raise ValueError("invalid/duplicate resource process identity")
        if (pid, created) in process_keys:
            raise ValueError("duplicate resource PID/create-time identity")
        process_keys.add((pid, created))
        affinity = process["cpuAffinity"]
        if (not isinstance(affinity, list) or not 1 <= len(affinity) <= 1024 or
                len(set(affinity)) != len(affinity) or any(type(x) is not int or x < 0 for x in affinity)):
            raise ValueError("invalid process CPU affinity")
        _uint(process["maxObservedRssBytes"])
        _uint(process["maxObservedThreads"])
        by_id[identity] = process
        by_pid.setdefault(pid, []).append(process)
        sample_times[identity] = []
    previous = None
    for sample in samples:
        if not isinstance(sample, Mapping):
            raise ValueError("invalid resource sample")
        ns = _uint(sample["monotonicNs"])
        if previous is not None and ns <= previous:
            raise ValueError("nonmonotonic resource samples")
        previous = ns
        _uint(sample["observedRssBytes"])
        _uint(sample["observedThreads"])
        ids = sample["processIdentityIds"]
        if not isinstance(ids, list) or len(ids) > 64 or len(set(ids)) != len(ids):
            raise ValueError("invalid active process identity list")
        for identity in ids:
            identity = _uint(identity)
            if identity not in by_id:
                raise ValueError("sample references unknown process identity")
            sample_times[identity].append(ns)
    for identity, process in by_id.items():
        seen = sample_times[identity]
        if not seen or (_uint(process["firstSampleNs"]), _uint(process["lastSampleNs"])) != (seen[0], seen[-1]):
            raise ValueError("resource process observation interval mismatch")
    for label, pid in roots.items():
        if not any(p["owner"] == label and p["maxObservedRssBytes"] > 0 for p in by_pid.get(pid, [])):
            errors.append("no resource sample for root: " + label)
    model_coverage = []
    for window in timing["windows"]:
        pid, start, end = (window.get(k) for k in ("workerPid", "modelInitEndNs", "inferenceEndNs"))
        covered = False
        observation = "missing"
        worker_sample = None
        if "modelResourceSample" in window:
            try:
                sample = window["modelResourceSample"]
                fields = {"monotonicNs", "pid", "creationTimeUnixSeconds", "rssBytes", "threads"}
                if not isinstance(sample, Mapping) or set(sample) != fields:
                    raise ValueError("invalid worker sample fields")
                observed_pid = _uint(sample["pid"], 2**31 - 1)
                ns = _uint(sample["monotonicNs"])
                created = sample["creationTimeUnixSeconds"]
                inference_start = _uint(window["inferenceStartNs"])
                if (type(pid) is not int or observed_pid == 0 or observed_pid != pid or
                        pid == roots.get("compute") or
                        type(created) not in (int, float) or not math.isfinite(created) or created <= 0 or
                        not _uint(start) <= ns <= inference_start <= _uint(end) or
                        _uint(sample["rssBytes"]) == 0 or _uint(sample["threads"]) == 0):
                    raise ValueError("invalid worker sample identity/time/measurement")
                matches = [p for p in by_pid.get(pid, [])
                           if p["owner"] == "compute/descendant" and
                           p["creationTimeUnixSeconds"] == created and
                           p["firstSampleNs"] <= ns <= p["lastSampleNs"]]
                if len(matches) != 1:
                    raise ValueError("worker sample process identity not observed")
                worker_sample = dict(sample)
                covered, observation = True, "worker-boundary"
            except (ValueError, KeyError, TypeError) as error:
                errors.append("invalid model resource sample: " + window["windowId"] + ": " + str(error))
        elif (type(pid) is int and type(start) is int and type(end) is int and start <= end and
                pid != roots.get("compute")):
            for process in by_pid.get(pid, []):
                if process["owner"] != "compute/descendant":
                    continue
                times = sample_times[process["identityId"]]
                index = bisect_left(times, start)
                if index < len(times) and times[index] <= end:
                    covered = True
                    observation = "aggregate-poll"
                    break
        model_coverage.append({"windowId": window["windowId"], "workerPid": pid,
                               "sampledWhileModelsLoaded": covered,
                               "observationKind": observation, "workerSample": worker_sample})
        if not covered:
            errors.append("no loaded-model resource sample: " + window["windowId"])
    host = resource["host"]
    if not isinstance(host, Mapping):
        raise ValueError("invalid host resource metadata")
    affinity = host.get("launcherAffinity")
    if (not isinstance(affinity, list) or not 1 <= len(affinity) <= 1024 or
            any(type(x) is not int or x < 0 for x in affinity) or len(set(affinity)) != len(affinity)):
        raise ValueError("invalid launcher CPU affinity")
    if (not host.get("cpuModel") or type(host.get("logicalCpus")) is not int or
            host["logicalCpus"] <= 0 or not host.get("launcherAffinity") or
            not host.get("pythonVersion") or not host.get("psutilVersion")):
        errors.append("incomplete host CPU/runtime metadata")
    if (resource.get("requestedDevice") != "cpu" or
            resource.get("gpuMeasurement") != "not-applicable-cpu"):
        errors.append("GPU resources unavailable for the requested device")
    peak = max(s["observedRssBytes"] for s in samples)
    if resource.get("sampledAggregatePeakRssBytes") != peak or peak <= 0:
        errors.append("invalid sampled aggregate peak RSS")
    return {"complete": not errors, "errors": errors,
            "summary": {"scope": resource["scope"], "measurement": resource["measurement"],
                        "sampleCount": len(samples), "targetSamplePeriodNs": resource["targetSamplePeriodNs"],
                        "largestObservedGapNs": resource["largestObservedGapNs"],
                        "sampledAggregatePeakRssBytes": peak,
                        "processIdentityCount": len(processes), "registeredRootCount": len(roots),
                        "maxObservedThreads": resource["maxObservedThreads"],
                        "host": host, "gpuMeasurement": resource["gpuMeasurement"],
                        "modelCoverage": model_coverage, "limitations": resource["limitations"]}}


def build_person_run_receipt(run_root: Path, plan: Mapping[str, Any], *,
                             closure_verified: bool) -> dict[str, Any]:
    """Create an execution receipt; absent resource/data qualification stays explicit."""
    timing = collect_person_timing(run_root, plan)
    prepared = _read_json(run_root / "prepared-person-sources.json")
    descriptor = _read_json(Path(plan["personTracking"]["computationManifest"]))
    lifecycle = _read_json(run_root / "lifecycle.json")
    cleanup = _read_json(run_root / "person-cleanup.json")
    start, end = cleanup.get("runStartedNs"), cleanup.get("cleanupEndedNs")
    elapsed = (end - start) / 1000000000 if type(start) is int and type(end) is int and end >= start else None
    resource_digest = None
    try:
        resource_raw = _read_bytes(run_root / "person-resources.json", 16 * 1024 * 1024)
        resource_digest = "sha256:" + hashlib.sha256(resource_raw).hexdigest()
        expected_roots = {p["name"] for p in plan["processes"]} | {
            "nfd-" + node for node in plan["topology"]["nodes"]}
        validation = validate_person_resources(_decode(resource_raw), timing, expected_roots)
    except (OSError, ValueError, KeyError, TypeError) as error:
        validation = {"complete": False, "summary": None, "errors": [str(error)[:512]]}
    committed = sum(w["committedNs"] is not None for w in timing["windows"])
    counters = {"plannedWindows": timing["plannedWindows"], "plannedFrames": timing["plannedFrames"],
                "admittedFrames": sum(f["admittedNs"] is not None for f in timing["frames"]),
                "committedWindows": committed,
                "receivedFrames": sum(f["receivedNs"] is not None for f in timing["frames"]),
                "displayedFrames": sum(f["displayNs"] is not None for f in timing["frames"]),
                "uncommittedWindows": timing["plannedWindows"] - committed,
                "unreceivedScheduledFrames": sum(f["receivedNs"] is None for f in timing["frames"])}
    throughput = (3 * committed / elapsed) if elapsed is not None and elapsed > 0 else None
    return {"schema": "uav-person-run-receipt-v1", "runId": run_root.name,
            "missionId": run_root.name, "sourceManifestDigest": prepared["sourceManifestDigest"],
            "datasetOrigin": prepared.get("datasetOrigin", "unspecified"),
            "profileDigest": plan["personTracking"]["profileDigest"],
            "computationDigest": descriptor["computationDigest"],
            "installedClosureVerified": closure_verified, "synthetic": None, "dataQualified": False,
            "dataQualification": "not-assessed; evaluation labels remain isolated from runtime",
            "terminal": lifecycle.get("terminal") is True, "outcome": lifecycle.get("outcome"),
            "elapsedSeconds": elapsed, "cleanupSeconds": cleanup.get("cleanupSeconds"),
            "remainingOwnedProcesses": cleanup.get("remainingOwnedProcesses"),
            "cleanupErrors": cleanup.get("cleanupErrors", []) + cleanup.get("auditErrors", []),
            "resourceMeasurementsComplete": validation["complete"],
            "resourceMeasurements": validation["summary"], "resourceErrors": validation["errors"],
            "resourceArtifactDigest": resource_digest, "counts": counters,
            "sourceSampleHz": 2, "effectiveProcessedFramesPerSecond": throughput,
            "throughputBasis": "committed frames / MiniNDN configuration-through-cleanup wall seconds",
            "bufferDropMeasurement": "not-applicable to request-driven frozen-frame replay",
            "backpressureMeasurement": "not-instrumented; unreceived scheduled frames reported separately",
            "failedWindows": sum(w["committedNs"] is None for w in timing["windows"]),
            "completeFrameTimingRecords": timing["completeFrameTimingRecords"],
            "completeWindowTimingRecords": timing["completeWindowTimingRecords"],
            "timingQualified": timing["timingQualified"], "timing": timing,
            "qualification": "not-assessed; requires independent data/metrics/resources"}
