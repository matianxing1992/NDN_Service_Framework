#!/usr/bin/env python3
"""Run the Spec 177 multi-view collaboration fixture in MiniNDN.

Without ``--execute`` this command emits a deterministic contract only.  With
``--execute`` it starts one real NFD per MiniNDN node and independent
python-ndn processes for the controller, four UAV image producers, two
competing compute Providers, and the Ground Station coordinator.  The wire
fixture uses digest-signed named Data and records every lifecycle event; image
bytes never occur in the request, ACK, selection, or response payloads.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[2]
TOOL = Path(__file__).resolve().with_name("minindn_multiview_node.py")
FIXTURE = ROOT / "NDNSF-UAV-APP/testdata/multiview-car"
MANIFEST = FIXTURE / "manifest.json"
TOPOLOGY = FIXTURE / "minindn-topology.conf"
MODEL_CONFIG = ROOT / "NDNSF-UAV-APP/configs/uav_multiview_models.json"
SCENARIOS = ("nominal", "provider-selection", "unavailable-view", "late-view",
             "publication-failure", "model-missing", "model-digest-failure")
PROVIDERS = ("/provider/gpu", "/provider/cpu")
PRODUCERS = ("/uav/A", "/uav/B", "/uav/C", "/uav/D")
JOB_DATA_NAME = "/example/uav/gs/UAV/MULTIVIEW/REQUEST/recognition-001/1"


def load_json(path: Path) -> Dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("expected a JSON object: %s" % path)
    return value


def model_profile() -> Dict[str, Any]:
    registry = load_json(MODEL_CONFIG)
    profiles = registry.get("profiles", [])
    if not profiles:
        raise ValueError("model registry has no profiles")
    return dict(profiles[0])


def topology_nodes(path: Path) -> List[str]:
    nodes: List[str] = []
    active = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line == "[nodes]":
            active = True
            continue
        if line.startswith("["):
            active = False
        if active and line and not line.startswith("#"):
            nodes.append(line.split(":", 1)[0])
    return nodes


def preflight() -> Dict[str, Any]:
    required = [TOOL, MANIFEST, TOPOLOGY, MODEL_CONFIG]
    missing = [str(path) for path in required if not path.is_file()]
    commands = {}
    for command in ("nfd", "nfdc", "infoconv"):
        commands[command] = subprocess.call(
            ["bash", "-lc", "command -v %s >/dev/null 2>&1" % command],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
    try:
        import minindn  # noqa: F401
        minindn_error = ""
    except Exception as exc:  # pragma: no cover - host dependent
        minindn_error = "%s: %s" % (type(exc).__name__, exc)
    manifest = load_json(MANIFEST) if MANIFEST.is_file() else {}
    profile: Dict[str, Any] = {}
    try:
        profile = model_profile()
    except Exception as exc:
        missing.append("model-profile:%s" % exc)
    artifact = ROOT / str(profile.get("model_artifact", "")) if profile else Path("")
    if profile and (not artifact.is_file() or str(profile.get("model_digest", "")) == ""):
        missing.append("registered-model-artifact:%s" % artifact)
    return {
        "schemaVersion": "spec178-uav-minindn-preflight-v1",
        "requiredNodes": ["gs", "controller", "uav-a", "uav-b", "uav-c",
                           "uav-d", "pgpu", "pcpu"],
        "nodes": topology_nodes(TOPOLOGY) if TOPOLOGY.is_file() else [],
        "missing": missing,
        "commands": commands,
        "minindnAvailable": not minindn_error,
        "minindnError": minindn_error,
        "runningAsRoot": os.geteuid() == 0,
        "scenarios": list(SCENARIOS),
        "uavProducerCount": len({str(v.get("producer_identity", ""))
                                  for v in manifest.get("views", [])}),
        "computeProviders": list(PROVIDERS),
        "applicationPayloadContract": "named-data-references-only",
        "scientificAccuracyClaimAllowed": False,
        "modelProfileId": profile.get("profile_id"),
        "modelArtifact": str(artifact) if profile else None,
        "modelArtifactPresent": bool(profile and artifact.is_file()),
        "modelExecutionProvider": profile.get("execution_provider"),
    }


def dry_trace(manifest: Mapping[str, Any], provider: str, scenario: str) -> List[Dict[str, Any]]:
    mission = "mission-uav-mv"
    job = "recognition-001"
    trace: List[Dict[str, Any]] = [{
        "stage": "REQUEST_PUBLISHED", "missionSessionId": mission, "jobId": job,
        "requestDataName": JOB_DATA_NAME, "imageBytesInServicePayload": False,
    }]
    for view in manifest.get("views", []):
        producer = str(view.get("producer_identity", ""))
        view_id = str(view.get("view_id", ""))
        trace.append({
            "stage": "VIEW_DATA_PUBLISHED", "producer": producer,
            "exactDataName": "%s/UAV/IMAGE/%s/v=1" % (producer, view_id),
            "contentDigest": "sha256:%s" % view.get("sha256", ""),
            "imageBytesInServicePayload": False,
        })
    trace.extend([
        {"stage": "ACK_MATCHED", "providers": list(PROVIDERS)},
        {"stage": "PROVIDER_SELECTED", "provider": provider},
    ])
    if scenario in ("nominal", "provider-selection"):
        trace.extend([
            {"stage": "FUSION_COMPLETED", "provider": provider,
             "consumedViewCount": len(manifest.get("views", []))},
            {"stage": "TERMINAL_ACCEPTED", "terminalOwner": provider,
             "annotationCount": len(manifest.get("views", [])),
             "imageBytesInServicePayload": False},
        ])
    else:
        stage = ("VIEW_DATA_FETCH_FAILED" if scenario in ("unavailable-view", "late-view")
                 else "ANNOTATION_PUBLICATION_FAILED" if scenario == "publication-failure"
                 else "INFERENCE_FAILED")
        trace.append({"stage": stage, "provider": provider})
        trace.append({"stage": "TERMINAL_REJECTED", "terminalOwner": provider,
                      "scenario": scenario})
    return trace


def _face_lookup(created_faces: Mapping[Any, Iterable[Tuple[str, str, int]]]) -> Dict[Tuple[str, str], str]:
    lookup: Dict[Tuple[str, str], str] = {}
    for node, links in created_faces.items():
        for peer, ip, _cost in links:
            lookup[(node.name, peer)] = ip
    return lookup


def _socket_for(node: Any) -> str:
    home = Path(node.params["params"]["homeDir"])
    return str(home / (node.name + ".sock"))


def _wait_socket(path: Path, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not path.exists():
        time.sleep(0.05)
    if not path.exists():
        raise RuntimeError("NFD socket not ready: %s" % path)


def _wait_marker(path: Path, marker: str, processes: Sequence[Tuple[str, Any, Any]],
                 timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""
        if marker in text:
            return
        for name, proc, _log in processes:
            if path.name == "%s.log" % name and proc.poll() not in (None, 0, -signal.SIGINT):
                raise RuntimeError("%s exited before marker %s" % (name, marker))
        time.sleep(0.1)
    raise RuntimeError("%s did not emit marker %r" % (path, marker))


def _events(path: Path) -> List[Dict[str, Any]]:
    events: List[Dict[str, Any]] = []
    if not path.exists():
        return events
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and value.get("event"):
            events.append(value)
    return events


def _write_job(output: Path, scenario: str, late_delay_ms: int) -> Tuple[Path, Dict[str, Any]]:
    manifest = load_json(MANIFEST)
    views: List[Dict[str, Any]] = []
    producer_entries: Dict[str, List[Dict[str, Any]]] = {producer: [] for producer in PRODUCERS}
    wire_root = output / "wire-assets"
    wire_root.mkdir(parents=True, exist_ok=True)
    for index, source in enumerate(manifest["views"]):
        producer = str(source["producer_identity"])
        view_id = str(source["view_id"])
        exact_name = "%s/UAV/IMAGE/%s/v=1" % (producer, view_id)
        # The checked-in generated images are intentionally large (several MB)
        # and exceed the default MiniNDN UDP packet path.  Publish a
        # deterministic small PNG derivative as the wire fixture, while
        # retaining the checked-in source hash in the job for provenance.
        from PIL import Image
        source_path = FIXTURE / str(source["file"])
        wire_path = wire_root / (view_id + ".png")
        image = Image.open(source_path).convert("RGB")
        # Keep each Data comfortably below the UDP MTU path used by NFD.  The
        # original source remains in the checked-in fixture; this derivative
        # is only a transport/retrieval gate and carries its own digest.
        image.thumbnail((64, 64))
        image.save(wire_path, format="PNG", optimize=True)
        import hashlib
        wire_digest = "sha256:" + hashlib.sha256(wire_path.read_bytes()).hexdigest()
        entry: Dict[str, Any] = {
            "viewId": view_id,
            "producerIdentity": producer,
            "exactDataName": exact_name,
            "contentDigest": wire_digest,
            "sourceFixtureDigest": "sha256:%s" % source["sha256"],
            "captureTimeMs": 1000 + index,
            "targetId": str(manifest["target_id"]),
            "mediaType": "image/png",
        }
        views.append(entry)
        producer_entry: Dict[str, Any] = {
            "name": exact_name,
            "file": str(wire_path),
            "contentDigest": entry["contentDigest"],
            "available": True,
        }
        if scenario == "unavailable-view" and index == len(manifest["views"]) - 1:
            producer_entry["available"] = False
        if scenario == "late-view" and index == len(manifest["views"]) - 1:
            producer_entry["delayMs"] = late_delay_ms
        producer_entries.setdefault(producer, []).append(producer_entry)
    job: Dict[str, Any] = {
        "schema": "ndnsf-uav-multiview-job/v1",
        "missionSessionId": "mission-uav-mv",
        "jobId": "recognition-001",
        "attempt": 1,
        "targetId": str(manifest["target_id"]),
        "captureWindow": {"startMs": 1000, "endMs": 2000},
        "minimumViews": 2,
        "minimumDistinctProducers": 2,
        "modelProfileId": "vehicle-mvcnn-v1",
        "deadlineMs": 2500 if scenario != "late-view" else 800,
        "views": views,
        "scenario": scenario,
    }
    job_path = output / "job.json"
    job_path.write_text(json.dumps(job, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    for producer, entries in producer_entries.items():
        suffix = producer.rsplit("/", 1)[-1]
        (output / ("names-%s.json" % suffix)).write_text(
            json.dumps(entries, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return job_path, job


def _configure_nfd(ndn: Any, nodes: Sequence[Any]) -> None:
    from minindn.apps.nfd import Nfd

    for node in nodes:
        nfd = Nfd(node, logLevel="WARN")
        home = Path(node.params["params"]["homeDir"])
        desired = str(home / (node.name + ".sock"))
        conf = Path(nfd.confFile)
        conf_text = conf.read_text(encoding="utf-8")
        conf_text = conf_text.replace(str(nfd.sockFile), desired)
        conf.write_text(conf_text, encoding="utf-8")
        nfd.sockFile = desired
        Path(nfd.clientConf).write_text("transport=unix://%s\n" % desired,
                                       encoding="utf-8")
        nfd.start()
        node.params["spec177_nfd"] = nfd
    for node in nodes:
        _wait_socket(Path(_socket_for(node)))


def _run_nfdc(node: Any, socket: str, command: str) -> str:
    return node.cmd("NDN_CLIENT_TRANSPORT=unix://%s nfdc %s" % (socket, command))


def _configure_routes(ndn: Any, nodes: Sequence[Any]) -> List[Dict[str, Any]]:
    created = ndn.setupFaces()
    addresses = _face_lookup(created)
    route_records: List[Dict[str, Any]] = []

    def face_id(node: Any, peer: str) -> str:
        socket = _socket_for(node)
        ip = addresses[(node.name, peer)]
        listing = _run_nfdc(node, socket, "face list")
        pattern = r"faceid=(\d+).*remote=(?:udp|udp4)://%s:6363" % re.escape(ip)
        match = re.search(pattern, listing)
        if not match:
            raise RuntimeError("face missing node=%s peer=%s ip=%s: %s" %
                               (node.name, peer, ip, listing))
        return match.group(1)

    by_name = {node.name: node for node in nodes}
    # gs is the star router.  Every non-gs node routes service traffic through
    # gs; gs routes each producer/provider/controller prefix to its local peer.
    gs = by_name["gs"]
    for node_name, prefix in (
        ("controller", "/example/uav/controller"),
        ("uav-a", "/uav/A"), ("uav-b", "/uav/B"),
        ("uav-c", "/uav/C"), ("uav-d", "/uav/D"),
        ("pgpu", "/provider/gpu"),
        ("pcpu", "/provider/cpu"),
    ):
        peer = by_name[node_name]
        fid = face_id(gs, node_name)
        command = "route add %s nexthop %s" % (prefix, fid)
        result = _run_nfdc(gs, _socket_for(gs), command)
        route_records.append({"node": "gs", "peer": node_name, "prefix": prefix,
                              "faceId": int(fid), "result": result.strip()})
    for node in nodes:
        if node.name == "gs":
            continue
        fid = face_id(node, "gs")
        prefixes = ["/example/uav"]
        if node.name in {"pgpu", "pcpu"}:
            prefixes.append("/uav")
        for prefix in prefixes:
            command = "route add %s nexthop %s" % (prefix, fid)
            result = _run_nfdc(node, _socket_for(node), command)
            route_records.append({"node": node.name, "peer": "gs", "prefix": prefix,
                                  "faceId": int(fid), "result": result.strip()})
    return route_records


def run_real(output: Path, scenario: str, selected_provider: str,
             lifetime_ms: int, late_delay_ms: int,
             model_mode: str = "real") -> Dict[str, Any]:
    report = preflight()
    output.mkdir(parents=True, exist_ok=True)
    (output / "preflight.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                            encoding="utf-8")
    if report["missing"] or not report["minindnAvailable"]:
        report["status"] = "preflight-failed"
        return report
    if not all(report["commands"].values()):
        report["status"] = "preflight-failed"
        report["reason"] = "MiniNDN command dependency missing"
        return report
    if not report["runningAsRoot"]:
        report["status"] = "requires-root"
        report["reason"] = "MiniNDN/Mininet needs root; rerun with sudo"
        return report
    if selected_provider not in PROVIDERS:
        raise ValueError("selected provider must be one of %s" % (PROVIDERS,))

    job_path, job = _write_job(output, scenario, late_delay_ms)
    processes: List[Tuple[str, Any, Any]] = []
    started = time.monotonic()
    ndn = None
    try:
        from minindn.minindn import Minindn
        from minindn.util import getPopen

        saved_argv = sys.argv
        sys.argv = [saved_argv[0]]
        try:
            ndn = Minindn(topoFile=str(TOPOLOGY), workDir=str(output / "minindn"))
        finally:
            sys.argv = saved_argv
        ndn.start()
        nodes = list(ndn.net.hosts)
        _configure_nfd(ndn, nodes)
        routes = _configure_routes(ndn, nodes)
        (output / "routes.json").write_text(json.dumps(routes, indent=2, sort_keys=True) + "\n",
                                             encoding="utf-8")

        def launch(node_name: str, process_name: str, command: str) -> None:
            node = ndn.net[node_name]
            log_path = output / (process_name + ".log")
            log = log_path.open("wb")
            python_site = "/home/tianxing/.local/lib/python3.8/site-packages"
            env = {
                # The privileged launcher uses /usr/bin/python3.  python-ndn
                # is installed in the project user's site directory, which
                # is not visible to root; pass that exact site path into each
                # MiniNDN namespace instead of depending on host PYTHONPATH.
                "PYTHONPATH": str(TOOL.parent) + os.pathsep + python_site,
                "NDNSF_TIMELINE_TRACE_SAMPLE_RATE": "1",
                "PYTHONUNBUFFERED": "1",
            }
            proc = getPopen(node, command, envDict=env, shell=True,
                            stdout=log, stderr=subprocess.STDOUT)
            processes.append((process_name, proc, log))

        python = shlex_quote(sys.executable)
        common = "--lifetime-ms %d --scenario %s" % (lifetime_ms, shell_quote(scenario))
        launch("controller", "controller", "%s %s --mode controller --identity /example/uav/controller --prefix /example/uav/controller" % (python, str(TOOL)))
        _wait_marker(output / "controller.log", "CONTROLLER_READY", processes, 20)
        manifest = load_json(MANIFEST)
        for producer in PRODUCERS:
            suffix = producer.rsplit("/", 1)[-1]
            node_name = "uav-" + suffix.lower()
            command = ("%s %s --mode producer --identity %s --prefix %s "
                       "--names %s --fixture-root %s %s" %
                       (python, str(TOOL), shell_quote(producer), shell_quote(producer),
                        shell_quote(str(output / ("names-%s.json" % suffix))),
                        shell_quote(str(FIXTURE)), common))
            launch(node_name, "producer-%s" % suffix, command)
        for suffix in ("A", "B", "C", "D"):
            _wait_marker(output / ("producer-%s.log" % suffix), "PRODUCER_READY", processes, 20)
        profile = model_profile()
        for provider, node_name, process_name in (
            ("/provider/gpu", "pgpu", "provider-gpu"),
            ("/provider/cpu", "pcpu", "provider-cpu"),
        ):
            model_path = ROOT / profile["model_artifact"]
            model_digest = str(profile["model_digest"])
            if scenario == "model-missing":
                model_path = output / "missing-model.onnx"
            elif scenario == "model-digest-failure":
                model_digest = "sha256:" + "0" * 64
            command = ("%s %s --mode provider --identity %s --prefix %s "
                       "--model-mode %s --model %s --model-registry %s "
                       "--profile-id %s --model-digest %s %s" %
                       (python, str(TOOL), shell_quote(provider), shell_quote(provider),
                        shell_quote(model_mode),
                       shell_quote(str(model_path)),
                       shell_quote(str(MODEL_CONFIG)), shell_quote(str(profile["profile_id"])),
                        shell_quote(model_digest),
                        common))
            launch(node_name, process_name, command)
        _wait_marker(output / "provider-gpu.log", "PROVIDER_READY", processes, 20)
        _wait_marker(output / "provider-cpu.log", "PROVIDER_READY", processes, 20)
        summary_path = output / "coordinator-summary.json"
        coordinator = ("%s %s --mode coordinator --identity /example/uav/gs "
                       "--job %s --job-data-name %s --provider %s --providers %s "
                       "--summary %s --lifetime-ms %d --selection-lifetime-ms %d "
                       "--scenario %s" %
                       (python, str(TOOL), shell_quote(str(job_path)), shell_quote(JOB_DATA_NAME),
                        shell_quote(selected_provider), shell_quote(",".join(PROVIDERS)),
                        shell_quote(str(summary_path)), lifetime_ms, max(5000, lifetime_ms * 4),
                        shell_quote(scenario)))
        launch("gs", "coordinator", coordinator)
        _wait_marker(output / "coordinator.log", "COORDINATOR_COMPLETE", processes, 25)
        time.sleep(0.5)
    except Exception as exc:
        (output / "launcher-error.json").write_text(json.dumps({
            "error": "%s: %s" % (type(exc).__name__, exc),
        }, indent=2) + "\n", encoding="utf-8")
    finally:
        for _name, proc, log in reversed(processes):
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=4)
                except Exception:
                    proc.kill()
            log.close()
        if ndn is not None:
            try:
                ndn.stop()
            finally:
                type(ndn).cleanUp()

    event_files = sorted(output.glob("*.log"))
    events: List[Dict[str, Any]] = []
    for path in event_files:
        for event in _events(path):
            event["log"] = path.name
            events.append(event)
    summary = load_json(output / "coordinator-summary.json") if (output / "coordinator-summary.json").is_file() else {}
    result = {
        "schemaVersion": "spec178-uav-minindn-result-v1",
        "status": summary.get("status", "launcher-failed"),
        "scenario": scenario,
        "selectedProvider": selected_provider,
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "processReturnCodes": {name: proc.poll() for name, proc, _log in processes},
        "events": events,
        "coordinator": summary,
        "routes": "routes.json" if (output / "routes.json").is_file() else None,
        "imageBytesInServicePayload": False,
        "scientificAccuracyClaimAllowed": False,
    }
    expected = scenario in ("nominal", "provider-selection")
    if expected:
        result_events = [event for event in events if event.get("event") == "RESULT_PUBLISHED"]
        annotation_events = [event for event in events if event.get("event") == "ANNOTATION_PUBLISHED"]
        fetched_events = [event for event in events if event.get("event") == "VIEW_DATA_FETCHED_VERIFIED"]
        nonselected_outputs = [event for event in result_events + annotation_events
                               if event.get("provider") != selected_provider]
        result["gatePassed"] = (summary.get("status") == "completed" and
                                 summary.get("terminalOwner") == selected_provider and
                                 summary.get("acceptedViewCount", 0) >= 2 and
                                 summary.get("annotationCount") == summary.get("acceptedViewCount") and
                                 len(fetched_events) == summary.get("acceptedViewCount") and
                                 len(result_events) == 1 and not nonselected_outputs and
                                 any(event.get("event") == "FUSION_COMPLETED" and
                                     event.get("provider") == selected_provider for event in events) and
                                 any(event.get("event") == "INFERENCE_COMPLETED" and
                                     event.get("provider") == selected_provider for event in events))
    else:
        result["gatePassed"] = (summary.get("status") == "rejected" and
                                 any(e.get("event") == "TERMINAL_REJECTED" for e in events))
    result_path = output / "result.json"
    result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def shell_quote(value: str) -> str:
    import shlex
    return shlex.quote(value)


def shlex_quote(value: str) -> str:
    return shell_quote(value)


def main() -> int:
    global MANIFEST, FIXTURE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", default=str(MANIFEST))
    parser.add_argument("--output", default="")
    parser.add_argument("--provider", default="/provider/cpu", choices=PROVIDERS)
    parser.add_argument("--scenario", choices=SCENARIOS, default="nominal")
    parser.add_argument("--execute", action="store_true",
                        help="start real MiniNDN/NFD and multi-process wire fixture")
    parser.add_argument("--all-scenarios", action="store_true",
                        help="execute all five scenarios into child output directories")
    parser.add_argument("--lifetime-ms", type=int, default=1000)
    parser.add_argument("--late-delay-ms", type=int, default=1500)
    parser.add_argument("--model-mode", choices=("functional", "real"), default="real")
    args = parser.parse_args()
    fixture_path = Path(args.fixture).resolve()
    if fixture_path != MANIFEST.resolve():
        MANIFEST = fixture_path
        FIXTURE = fixture_path.parent
    from multiview_contract import validate_fixture_manifest
    manifest = load_json(MANIFEST)
    errors = validate_fixture_manifest(manifest, MANIFEST.parent)
    if errors:
        print(json.dumps({"status": "invalid-fixture", "errors": errors}, indent=2))
        return 2
    if args.lifetime_ms <= 0 or args.late_delay_ms <= args.lifetime_ms:
        parser.error("lifetime must be positive and late delay must exceed lifetime")
    if not args.execute:
        result = {
            "schema": "ndnsf-uav-multiview-minindn-trace/v2",
            "mode": "dry-run",
            "topology": {
                "nodes": topology_nodes(TOPOLOGY),
                "uavProducers": sorted({str(v.get("producer_identity")) for v in manifest["views"]}),
                "computeProviders": list(PROVIDERS),
                "selectedProvider": args.provider,
                "controller": "/example/uav/controller",
                "coordinator": "/example/uav/gs",
            },
            "scenarios": list(SCENARIOS),
            "selectedScenario": args.scenario,
            "trace": dry_trace(manifest, args.provider, args.scenario),
            "imageBytesInServicePayload": False,
            "scientificAccuracyClaimAllowed": False,
        }
        if args.output:
            path = Path(args.output)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    if args.all_scenarios:
        root = Path(args.output or (ROOT / "results/uav-multiview-minindn"))
        matrix: Dict[str, Any] = {"schemaVersion": "spec178-uav-minindn-matrix-v1", "runs": {}}
        exit_code = 0
        for scenario in SCENARIOS:
            result = run_real(root / scenario, scenario, args.provider,
                              args.lifetime_ms, args.late_delay_ms, args.model_mode)
            matrix["runs"][scenario] = {
                "status": result.get("status"),
                "gatePassed": result.get("gatePassed", False),
                "result": str(root / scenario / "result.json"),
            }
            if not result.get("gatePassed", False):
                exit_code = 1
        (root / "matrix.json").parent.mkdir(parents=True, exist_ok=True)
        (root / "matrix.json").write_text(json.dumps(matrix, indent=2, sort_keys=True) + "\n",
                                            encoding="utf-8")
        print(json.dumps(matrix, indent=2, sort_keys=True))
        return exit_code
    result = run_real(Path(args.output or (ROOT / "results/uav-multiview-minindn")),
                      args.scenario, args.provider, args.lifetime_ms, args.late_delay_ms,
                      args.model_mode)
    print(json.dumps(result, indent=2, sort_keys=True))
    if result.get("status") in ("preflight-failed", "requires-root"):
        return 3
    return 0 if result.get("gatePassed") else 1


if __name__ == "__main__":
    raise SystemExit(main())
