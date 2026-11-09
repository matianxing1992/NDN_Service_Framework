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


def audit_person_processes(run_root: Path, observed_processes=()) -> dict[str, Any]:
    """Audit pinned run identities/markers; a shared cgroup is never ownership alone."""
    import psutil

    if not isinstance(observed_processes, (list, tuple)) or len(observed_processes) > 512:
        raise ValueError("invalid bounded process identity registry")
    registered = {}
    for item in observed_processes:
        pid, created = item.get("pid"), item.get("creationTimeUnixSeconds")
        if (type(pid) is not int or pid <= 0 or type(created) not in (int, float) or
                not math.isfinite(created) or created < 0):
            raise ValueError("invalid process creation identity")
        registered.setdefault(pid, set()).add(created)
    excluded, pid = set(), os.getpid()
    while pid and pid not in excluded:
        excluded.add(pid)
        try:
            fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            pid = int(fields[1])
        except (OSError, ValueError, IndexError):
            break
    root = os.fsencode(str(run_root.resolve()))
    marker = b"NDNSF_SPEC191_RUN_ROOT=" + root
    cgroup = _read_bytes(Path("/proc/self/cgroup"), 65536)
    remaining, errors, zombies, seen_registered = [], [], [], set()
    for process in Path("/proc").iterdir():
        if not process.name.isdigit() or int(process.name) in excluded:
            continue
        process_pid = int(process.name)
        try:
            before = _read_bytes(process / "stat", 65536).rsplit(b")", 1)[1].split()
            created = psutil.Process(process_pid).create_time()
            pinned = created in registered.get(process_pid, ())
            terminal = before[0] in (b"Z", b"X")
            if terminal:
                # Linux releases controller membership and environ at exit. Only a
                # previously observed PID/creation identity can own an unreaped task.
                if not pinned:
                    continue
                command = b""
            else:
                if _read_bytes(process / "cgroup", 65536) != cgroup:
                    continue  # Outer supervisors are outside this live launch scope.
                if not pinned:
                    environment = _read_bytes(process / "environ", 65536).split(b"\0")
                    if marker not in environment:
                        continue  # An argv path/shared cgroup cannot confer ownership.
                command = _read_bytes(process / "cmdline", 65536)
            after = _read_bytes(process / "stat", 65536).rsplit(b")", 1)[1].split()
            if before[19] != after[19]:
                continue
            if pinned:
                seen_registered.add((process_pid, created))
            state = after[0].decode("ascii")
            if state in ("Z", "X"):
                zombies.append({"pid": process_pid, "creationTimeUnixSeconds": created,
                                "state": state})
            remaining.append({"pid": process_pid, "creationTimeUnixSeconds": created,
                              "startTimeTicks": int(after[19]),
                              "ownership": "pinned-identity" if pinned else "run-environment-marker",
                              "state": state,
                              "command": command[:4096].replace(b"\0", b" ").decode(errors="replace")})
            if len(remaining) >= 512:
                errors.append("remaining owned process bound reached")
                break
        except (FileNotFoundError, ProcessLookupError, psutil.NoSuchProcess):
            pass
        except (OSError, ValueError, IndexError, psutil.Error) as error:
            if len(errors) < 64:
                errors.append(f"/proc/{process.name}: {type(error).__name__}")
    return {"remainingOwnedProcesses": len(remaining), "remaining": remaining,
            "liveOwnedProcesses": len(remaining) - len(zombies),
            "unreapedOwnedProcesses": len(zombies), "zombies": zombies,
            "exitedRegisteredProcessCount": sum(
                (pid, created) not in seen_registered for pid, times in registered.items()
                for created in times),
            "auditErrors": errors,
            "scope": "live: pinned PID/creation identity or exact run marker with matching cgroup; "
                     "unreaped: pinned PID/creation identity, kernel may release cgroup/environ"}


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


def validate_main_telemetry(document: Mapping[str, Any], job: Mapping[str, Any]) -> dict[str, Any]:
    """Audit native phase/participant measurements without issuing a service request."""
    if not isinstance(document, Mapping):
        return {'complete': False, 'errors': ['ordinary telemetry journal must be an object'],
                'reports': [], 'maxElapsedMs': None}
    errors = []
    expected = {(source['producerIdentity'], source['droneId']) for source in job['sources']}
    expected.add((job['compute']['producerIdentity'], job['compute']['droneId']))
    phases = (0, 1, 2) if job['windowCount'] > 1 else (0, 2)
    if (document.get('schema') != 'uav-main-tracking-telemetry-v1' or
            document.get('runId') != job['runId'] or document.get('clockDomain') != 'CLOCK_MONOTONIC_MS' or
            document.get('complete') is not True or document.get('failed') is not False or
            document.get('phaseStates') != ([2, 2, 2] if len(phases) == 3 else [2, 0, 2])):
        errors.append('ordinary telemetry scope/phase completion mismatch')
    reports = document.get('reports', [])
    seen, times = {phase: set() for phase in phases}, {phase: [] for phase in phases}
    if not isinstance(reports, list) or len(reports) != 4 * len(phases):
        errors.append('ordinary telemetry report count mismatch')
        reports = reports[:12] if isinstance(reports, list) else []
    for report in reports:
        try:
            phase = _uint(report['phase'], 2)
            if phase not in phases or report['runId'] != job['runId']:
                raise ValueError('wrong phase/run')
            participant = (report['producerIdentity'], report['droneId'])
            if participant not in expected or participant in seen[phase] or report['verified'] is not True:
                raise ValueError('unverified/repeated/wrong participant')
            start, end = _uint(report['startedMonotonicMs']), _uint(report['completedMonotonicMs'])
            if end < start or end - start > 2000 or report['elapsedMs'] != end - start:
                raise ValueError('ordinary response exceeds frozen responsiveness deadline')
            windows = _uint(report['verifiedWindowsAtRequest'], job['windowCount'])
            state = report['nativeStateAtRequest']
            if ((phase == 0 and (state not in ('PREPARING', 'RUNNING') or windows != 0)) or
                    (phase == 1 and (state != 'RUNNING' or not 0 < windows < job['windowCount'])) or
                    (phase == 2 and (state != 'COMPLETED' or windows != job['windowCount']))):
                raise ValueError('ordinary probe was not taken during its declared native phase')
            seen[phase].add(participant)
            times[phase].append((start, end))
        except (KeyError, ValueError, TypeError) as error:
            errors.append('ordinary telemetry: ' + str(error))
    previous_end = None
    for phase in phases:
        if seen[phase] != expected:
            errors.append('ordinary telemetry phase has missing participants: ' + str(phase))
        if times[phase]:
            first = min(start for start, _ in times[phase])
            last = max(end for _, end in times[phase])
            if previous_end is not None and first < previous_end:
                errors.append('ordinary phases overlapped or were reordered')
            previous_end = last
    return {'complete': not errors, 'errors': errors, 'reports': reports,
            'maxElapsedMs': max((end - start for phase in times.values() for start, end in phase), default=None)}


def cross_camera_matches(result: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Report original algorithm events, without treating them as identity ground truth."""
    matches = []
    for event in result.get('associationEvents', []):
        if not isinstance(event, Mapping):
            raise ValueError('association event must be an object')
        origin = event.get('origin') or {}
        if not isinstance(origin, Mapping) or not isinstance(origin.get('localKey', {}), Mapping):
            raise ValueError('association origin/localKey must be objects')
        camera = origin.get('localKey', {}).get('cameraId')
        if event.get('status') == 'MATCHED' and camera in ('UAV1', 'UAV2', 'UAV3') and camera != event.get('cameraId'):
            matches.append(event)
    return matches


def validate_main_motion_csv(result: Mapping[str, Any], raw: bytes) -> None:
    """Compare original native measurements and their CSV serialization field by field."""
    import csv
    import io
    fields = ('schema sourceId cameraId sourceEpoch sequence localTrackId trackGeneration '
        'globalId observationName observationDigest captureTimeUs estimateTimeUs rangeTimeUs '
        'velocityEastMps velocityNorthMps speedMps courseDeg horizontalRangeM slantRangeM '
        'sigmaSpeedMps sigmaCourseDeg sigmaRangeM speedStatus bearingStatus rangeStatus quality '
        'reasonCodes acquisitionKind deliveryMode geometryValidity accuracyQualification '
        'uncertaintyKind anchorKind calibrationRevision anchorApproximation').split()
    try:
        rows = list(csv.reader(io.StringIO(raw.decode()), strict=True))
    except csv.Error as error:
        raise ValueError('malformed native CSV: ' + str(error)) from error
    measurements = result['motion']['measurements']
    if not rows or rows[0] != fields or len(rows) != len(measurements) + 1:
        raise ValueError('native CSV header/measurement count mismatch')
    for cells, measurement in zip(rows[1:], measurements):
        row = dict(measurement, **measurement['trackKey'])
        row.update(sequence=measurement['frameKey']['sequence'],
                   observationName=measurement['observationRef']['name'],
                   observationDigest=measurement['observationRef']['digest'])
        if len(cells) != len(fields):
            raise ValueError('native CSV column count mismatch')
        for key, cell in zip(fields, cells):
            expected = row.get(key)
            if expected is None:
                valid = cell == ''
            elif isinstance(expected, str):
                valid = cell == expected
            else:
                try:
                    value = json.loads(cell)
                    valid = value == expected and (type(value) is bool) == (type(expected) is bool)
                except (ValueError, TypeError):
                    valid = False
            if not valid:
                raise ValueError('native CSV/JSON measurement mismatch: ' + key)


def build_main_run_receipt(run_root: Path, plan: Mapping[str, Any], *,
                           closure_verified: bool) -> dict[str, Any]:
    """Account original native artifacts; this collector grants no execution authority."""
    main = plan["trackingMain"]
    errors, jobs, artifacts = [], [], []

    def artifact(path, bound):
        if path.is_symlink():
            raise ValueError("receipt refuses symlink artifact: " + str(path))
        raw = _read_bytes(path, bound)
        reference = {"path": str(path), "digest": "sha256:" + hashlib.sha256(raw).hexdigest(),
                     "bytes": len(raw)}
        artifacts.append(reference)
        return raw, reference

    def observed_json(path, bound=65536):
        try:
            return _read_json(path, bound)
        except (OSError, ValueError) as error:
            errors.append("missing/invalid runtime evidence: " + str(path) + ": " + str(error))
            return {}

    lifecycle = observed_json(run_root / "lifecycle.json")
    cleanup = observed_json(run_root / "main-cleanup.json")
    observations = observed_json(run_root / "main-observations.json", 256 * 1024)
    try:
        log = _read_bytes(run_root / "ground-station.log", 64 * 1024 * 1024).decode(errors="replace")
    except OSError:
        log = ""
        errors.append("main GS log absent")
    compute = _read_json(Path(main["providerConfigs"]["compute"]["path"]), 256 * 1024)
    resource_raw, resource_ref = artifact(run_root / "main-resources.json", 16 * 1024 * 1024)
    resource = _decode(resource_raw)
    try:
        roots = {item["name"] for item in plan["processes"]} | {
            "nfd-" + node for node in plan["topology"]["nodes"]}
        resource_validation = validate_person_resources(resource,
            {"runId": run_root.name, "missionId": run_root.name, "windows": []}, roots)
        if resource.get("artifactTruncated"):
            resource_validation["complete"] = False
            resource_validation["errors"].append("resource artifact truncated")
    except (OSError, ValueError, KeyError, TypeError) as error:
        resource_validation = {"complete": False, "errors": [str(error)], "summary": None}
    if not resource_validation["complete"]:
        errors.append("resource measurements incomplete")
    if not closure_verified:
        errors.append("frozen installed/source closure changed")
    if lifecycle.get("outcome") != "main-jobs-completed" or not lifecycle.get("terminal"):
        errors.append("native jobs did not complete")
    if (cleanup.get("remainingOwnedProcesses") != 0 or cleanup.get("unreapedOwnedProcesses") != 0 or
            cleanup.get("cleanupErrors") or cleanup.get("auditErrors")):
        errors.append("run cleanup incomplete")
    if cleanup.get("applicationExitCodes", {}).get("ground-station") != 0:
        errors.append("main GS did not exit cleanly after drain")
    for definition in main["jobs"]:
        job = _read_json(Path(definition["path"]), 256 * 1024)
        root = Path(job["outputDirectory"])
        status = observed_json(root / "tracking-status.json")
        count = _uint(job["windowCount"], 600)
        telemetry = validate_main_telemetry(observed_json(root / "tracking-telemetry.json"), job)
        if not telemetry["complete"]:
            errors.extend(job["runId"] + ": " + error for error in telemetry["errors"])
        if (status.get("runId") != job["runId"] or status.get("jobId") != job["jobId"] or
                status.get("wireMissionId") != "tracking-" + job["runId"] or
                status.get("trackingMode") != job["mode"] or status.get("state") != "COMPLETED" or
                status.get("cleanupState") != "CLEAN" or status.get("verifiedResultWindows") != count):
            errors.append("native terminal/binding mismatch: " + job["runId"])
        # A single host shutdown is recorded only for the last active job.
        if definition == main["jobs"][-1] and status.get("hostShutdown") != "CLEAN":
            errors.append("main host did not confirm work drain")
        windows, displayed, csv_count, cross_matches = [], 0, 0, []
        run = next(item for item in compute["runs"] if item["runId"] == job["runId"])
        profile_key = run["profile"]["digest"][7:] if run["profile"] else "native-default"
        worker_root = Path(compute["ownedWorkspace"]) / job["mode"] / profile_key / job["runId"]
        for index in range(count):
            try:
                raw, ref = artifact(root / "results" / f"window-{index}.json", 8 * 1024 * 1024)
                result = _decode(raw)
                presentation_path = root / "presentation" / f"window-{index}" / "presentation.json"
                presentation = _read_json(presentation_path, 9 * 1024 * 1024)
                if (presentation["runId"] != job["runId"] or
                        presentation["missionId"] != "tracking-" + job["runId"] or
                        presentation["resultJson"].encode() != raw or
                        presentation["resultDigest"] != ref["digest"] or
                        presentation["responseProvider"] != job["compute"]["producerIdentity"] or
                        result["runId"] != job["runId"] or result["sequence"] != index + 1):
                    raise ValueError("result/presentation context mismatch")
                cross_matches.extend({"windowIndex": index, "event": event}
                                     for event in cross_camera_matches(result))
                expected = {source["cameraId"]: source for source in job["sources"]}
                images = {image["cameraId"]: image for image in presentation["images"]}
                if set(images) != set(expected) or len(presentation["images"]) != 3:
                    raise ValueError("three-camera presentation mismatch")
                for camera, image in images.items():
                    if image["path"] != f"window-{index}/{camera}.jpg":
                        raise ValueError("presentation image path binding")
                    _, image_ref = artifact(root / "presentation" / image["path"], 4 * 1024 * 1024)
                    if image_ref["digest"] != image["inputDigest"]:
                        raise ValueError("original JPEG digest mismatch")
                    marker = (f"SPEC228_TRACKING_FRAME_SHOWN run={job['runId']} window={index} "
                              f"camera={camera} result={ref['digest']}")
                    displayed += marker in log
                if job["mode"] == "vehicle":
                    filename = f"motion-measurements-{index + 1:06d}.csv"
                    if result.get("motionCsv") != filename:
                        raise ValueError("native CSV identity mismatch")
                    csv_raw, _ = artifact(worker_root / "annotated" / filename, 8 * 1024 * 1024)
                    validate_main_motion_csv(result, csv_raw)
                    csv_count += 1
                windows.append({"index": index, "result": ref,
                                "actualDevice": result.get("actualDevice", result.get("actualDetectorDevice"))})
            except (OSError, ValueError, KeyError, TypeError) as error:
                errors.append(f"{job['runId']}/window-{index}: {error}")
        shots = []
        for shot in observations.get("screenshots", []):
            if shot.get("runId") != job["runId"]:
                continue
            try:
                png, ref = artifact(Path(shot["path"]), 32 * 1024 * 1024)
                index = _uint(shot["windowIndex"], count - 1)
                result_ref = next(w["result"] for w in windows if w["index"] == index)
                if (png[:8] != b"\x89PNG\r\n\x1a\n" or ref["digest"] != shot["digest"] or
                        shot["resultDigest"] != result_ref["digest"] or
                        shot["generation"] != status["generation"] or
                        shot["nativeState"] not in {"running": ("RUNNING",), "terminal": ("COMPLETED",),
                            "cross-camera-witness": ("RUNNING", "COMPLETED")}[shot["label"]]):
                    raise ValueError("actual screenshot/result binding mismatch")
                if shot['label'] == 'cross-camera-witness' and not any(
                        match['windowIndex'] == index for match in cross_matches):
                    raise ValueError('screenshot has no original cross-camera MATCHED event')
                shots.append(shot)
            except (OSError, ValueError, KeyError, StopIteration) as error:
                errors.append("screenshot: " + str(error))
        full_count = 120 if job["mode"] == "person" else 100
        full_gui = not main["headless"] and len(main["jobs"]) == 1 and count == full_count
        if full_gui and (not {"running", "terminal"}.issubset({shot["label"] for shot in shots}) or displayed != 3 * count):
            errors.append("full GUI draw/screenshot coverage incomplete")
        if full_gui and cross_matches and not any(shot['label'] == 'cross-camera-witness' for shot in shots):
            errors.append('observed cross-camera event has no actual GUI witness capture')
        jobs.append({"runId": job["runId"], "mode": job["mode"], "nativeStatus": status,
                     "plannedWindows": count, "verifiedWindows": len(windows),
                     "drawnFrames": displayed, "nativeCsvFiles": csv_count,
                     "windows": windows, "screenshots": shots, "ordinaryTelemetry": telemetry,
                     "fullWorkload": count == full_count, "headless": main["headless"],
                     "crossCameraMatches": cross_matches,
                     "crossCameraWitnessLimitation": None if cross_matches else
                         "No native cross-camera MATCHED event observed; cross-camera identity accuracy not established"})
    return {"schema": "uav-main-tracking-receipt-v1", "runId": run_root.name,
            "applicationMode": "main", "complete": not errors, "errors": errors,
            "installedClosureVerified": closure_verified, "jobs": jobs,
            "lifecycle": lifecycle, "cleanup": cleanup, "artifacts": artifacts,
            "stateObservations": observations.get("states", []),
            "resourceMeasurementsComplete": resource_validation["complete"],
            "resourceErrors": resource_validation["errors"],
            "resources": {"artifact": resource_ref, "sampleCount": len(resource.get("samples", [])),
                          "sampledAggregatePeakRssBytes": resource.get("sampledAggregatePeakRssBytes"),
                          "host": resource.get("host"), "measurement": "sampled; not continuous peak"},
            "scientificAccuracyClaimAllowed": False,
            "qualification": "execution evidence only; independent acceptance matrix still required"}
