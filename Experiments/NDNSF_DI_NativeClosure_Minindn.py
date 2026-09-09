#!/usr/bin/env python3
"""MiniNDN owner for the Spec182 native-closure runner.

The owner may create the small tracked topology and export identity-bound node
context, but it deliberately does not implement a second collector or produce
a business result.  Until executable closure cases are frozen, the campaign
stops at an explicit UNQUALIFIED boundary after writing that context.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import stat
import sys
import time
from typing import Any, Mapping


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tests/standalone/run-spec182-native-closure.py"
TOPOLOGY = ROOT / "Experiments/Topology/spec182-native-closure.conf"
MANIFEST_SCHEMA = "spec182-case-manifest-v1"
QUALIFICATION_SCHEMA = "spec182-native-qualification-v1"
COUNTEREXAMPLES = tuple(f"I{index:02d}" for index in range(1, 9))
PROOF_CASES = tuple(f"PO-{index:03d}" for index in range(1, 15))


def _runner_module():
    spec = importlib.util.spec_from_file_location("spec182_native_closure", RUNNER)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load Spec182 native closure runner")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_registration(manifest_path: Path) -> dict[str, Any]:
    """Validate the frozen campaign registration without starting MiniNDN.

    Registration is deliberately separate from execution: this function only
    checks that the frozen manifest names the one shared runner, all I01--I08
    detector counterexamples, and the PO-001--PO-014 acceptance owners.
    """
    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    if document.get("schema") != MANIFEST_SCHEMA:
        raise ValueError("case-manifest schema mismatch")
    registration = document.get("qualification")
    if not isinstance(registration, dict):
        raise ValueError("qualification registration is missing")
    if registration.get("schema") != QUALIFICATION_SCHEMA:
        raise ValueError("qualification schema mismatch")
    runner = registration.get("runner")
    if runner != "tests/standalone/run-spec182-native-closure.py":
        raise ValueError("qualification runner is not canonical")
    cases = registration.get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError("qualification cases are missing")
    ids: list[str] = []
    for case in cases:
        if not isinstance(case, dict) or not isinstance(case.get("id"), str):
            raise ValueError("qualification case entry is invalid")
        if case["id"] in ids:
            raise ValueError("qualification case id is duplicated")
        if case.get("executeOwner") != "T016":
            raise ValueError(f"qualification case owner is not T016: {case['id']}")
        if case.get("expectedStatus") not in {"PASS", "FAIL", "UNQUALIFIED"}:
            raise ValueError(f"qualification case status is invalid: {case['id']}")
        ids.append(case["id"])
    required = set(COUNTEREXAMPLES) | set(PROOF_CASES)
    missing = sorted(required - set(ids))
    if missing:
        raise ValueError("qualification cases are missing: " + ",".join(missing))
    limits = registration.get("limits")
    if not isinstance(limits, dict) or int(limits.get("runSeconds", 0)) <= 0 \
            or int(limits.get("cleanupSeconds", 0)) <= 0:
        raise ValueError("qualification limits are invalid")
    return registration


def _read_proc_start_ticks(pid: int) -> int:
    if pid <= 0:
        raise ValueError("node owner PID is invalid")
    try:
        text = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError(f"node owner PID is not readable: {pid}") from exc
    closing = text.rfind(")")
    if closing <= 0:
        raise ValueError("node owner stat record is malformed")
    fields = text[closing + 2:].split()
    if len(fields) <= 19:
        raise ValueError("node owner stat record has no starttime")
    try:
        value = int(fields[19])
    except ValueError as exc:
        raise ValueError("node owner starttime is not numeric") from exc
    if value <= 0:
        raise ValueError("node owner starttime is invalid")
    return value


def collect_node_context(nodes: list[Any], nfd_sockets: Mapping[str, Path],
                         peer_node_ids: Mapping[str, list[str]]) -> dict[str, dict[str, Any]]:
    """Export identity-bound context from already-created MiniNDN nodes."""
    known_names = {str(getattr(node, "name", "")) for node in nodes}
    contexts: dict[str, dict[str, Any]] = {}
    for node in nodes:
        name = str(getattr(node, "name", ""))
        if not name or name in contexts:
            raise ValueError("MiniNDN node identity is missing or duplicated")
        if not bool(getattr(node, "inNamespace", False)):
            raise ValueError(f"MiniNDN node is not namespace-isolated: {name}")
        try:
            owner_pid = int(getattr(node, "pid", 0))
        except (TypeError, ValueError) as exc:
            raise ValueError(f"MiniNDN node PID is invalid: {name}") from exc
        netns_path = Path(f"/proc/{owner_pid}/ns/net")
        try:
            netns_stat = netns_path.stat()
        except OSError as exc:
            raise ValueError(f"MiniNDN node namespace is unreadable: {name}") from exc
        if not netns_path.is_absolute():
            raise ValueError(f"MiniNDN node namespace path is not absolute: {name}")
        try:
            socket_path = Path(nfd_sockets[name])
        except KeyError as exc:
            raise ValueError(f"MiniNDN node NFD socket is missing: {name}") from exc
        if not socket_path.is_absolute():
            raise ValueError(f"MiniNDN node NFD socket path is not absolute: {name}")
        try:
            socket_mode = socket_path.stat().st_mode
        except OSError as exc:
            raise ValueError(f"MiniNDN node NFD socket is unreadable: {name}") from exc
        if not stat.S_ISSOCK(socket_mode):
            raise ValueError(f"MiniNDN node NFD path is not a socket: {name}")
        peers = peer_node_ids.get(name)
        if (not isinstance(peers, list)
                or any(not isinstance(peer, str) or not peer or peer == name
                       or peer not in known_names for peer in peers)
                or len(set(peers)) != len(peers)):
            raise ValueError(f"MiniNDN node peer metadata is invalid: {name}")
        contexts[name] = {
            "id": name,
            "netnsPath": str(netns_path),
            "netnsInode": netns_stat.st_ino,
            "ownerPid": owner_pid,
            "ownerStartTicks": _read_proc_start_ticks(owner_pid),
            "nfdSocket": str(socket_path),
            "peerNodeIds": list(peers),
        }
    if not contexts:
        raise ValueError("MiniNDN topology has no application nodes")
    return contexts


def _wait_for_nfd_sockets(app_manager: Any, timeout_s: float = 20.0) -> dict[str, Path]:
    sockets = {
        str(app.node.name): Path(str(app.sockFile))
        for app in app_manager
    }
    if not sockets:
        raise ValueError("MiniNDN started no NFD applications")
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if all(path.is_socket() for path in sockets.values()):
            return sockets
        time.sleep(0.1)
    missing = sorted(name for name, path in sockets.items() if not path.is_socket())
    raise ValueError("MiniNDN NFD sockets are not ready: " + ",".join(missing))


def _execute_runner_case(runner_manifest: Path, case_id: str, output: Path,
                         contexts: Mapping[str, dict[str, Any]]) -> dict[str, Any]:
    """Run the canonical closure runner while MiniNDN owner namespaces live."""
    runner = _runner_module()
    case = runner.load_case(runner_manifest, case_id)
    closure_output = output / "closure-run"
    staged = runner.stage_root(case, closure_output)
    run = runner.run_case(case, staged, closure_output, nodes=dict(contexts))
    observation = runner.collect_trace(case, run)
    evaluation = runner.evaluate_case(case, run, observation)
    return {"case": case_id, "manifest": str(runner_manifest),
            "staged": staged, "run": run, "observation": observation,
            "evaluation": evaluation}


def _run_owned_campaign(manifest_path: Path, output: Path,
                        runner_manifest: Path | None = None,
                        runner_case: str | None = None) -> int:
    """Create the owner topology and export context, without inventing a case."""
    output = output.resolve()
    if output.exists():
        raise ValueError("output run directory must be new")
    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    case_id = document.get("campaignCase", "")
    if not isinstance(case_id, str) or not case_id:
        raise ValueError("campaignCase is required")
    registration = load_registration(manifest_path)
    registered_ids = {case["id"] for case in registration["cases"]}
    if case_id not in registered_ids:
        raise ValueError("campaignCase is not registered")
    topology_value = registration.get("topology", str(TOPOLOGY.relative_to(ROOT)))
    topology = (ROOT / str(topology_value)).resolve()
    if topology != TOPOLOGY.resolve() or not topology.is_file():
        raise ValueError("MiniNDN owner topology is not the canonical Spec182 topology")
    if os.geteuid() != 0:
        output.mkdir(parents=True, exist_ok=False)
        (output / "result.json").write_text(
            json.dumps({"status": "UNQUALIFIED", "reason": "MININDN_REQUIRES_ROOT",
                        "campaignCase": case_id}, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        return 2
    output.mkdir(parents=True, exist_ok=False)
    ndn = None
    try:
        from minindn.apps.app_manager import AppManager
        from minindn.apps.nfd import Nfd
        from minindn.minindn import Minindn

        saved_argv = sys.argv[:]
        try:
            sys.argv = [saved_argv[0]]
            ndn = Minindn(topoFile=str(topology), workDir=str(output / "minindn-work"))
        finally:
            sys.argv = saved_argv
        ndn.start()
        app_manager = AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        nfd_sockets = _wait_for_nfd_sockets(app_manager)
        peer_ids = {"requester": ["provider"], "provider": ["requester"]}
        contexts = collect_node_context(list(ndn.net.hosts), nfd_sockets, peer_ids)
        (output / "node-context.json").write_text(
            json.dumps({"topology": str(topology), "nodes": contexts},
                       indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if runner_manifest is not None:
            selected_runner_case = runner_case or case_id
            runner_result = _execute_runner_case(
                runner_manifest.resolve(), selected_runner_case, output, contexts)
            (output / "runner-result.json").write_text(
                json.dumps(runner_result, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
            evaluation_status = str(runner_result["evaluation"].get("status", "UNQUALIFIED"))
            exit_code = {"PASS": 0, "FAIL": 1, "UNQUALIFIED": 2}.get(evaluation_status, 2)
            (output / "result.json").write_text(
                json.dumps({"status": evaluation_status,
                            "reason": "CANONICAL_RUNNER_RESULT_RECORDED",
                            "campaignCase": case_id, "runnerCase": selected_runner_case,
                            "runnerResult": "runner-result.json",
                            "nodeContext": "node-context.json"},
                           indent=2, sort_keys=True) + "\n", encoding="utf-8")
            return exit_code
        (output / "result.json").write_text(
            json.dumps({"status": "UNQUALIFIED",
                        "reason": "NATIVE_CLOSURE_CASE_DEFINITION_MISSING",
                        "campaignCase": case_id, "nodeContext": "node-context.json",
                        "registeredCases": sorted(registered_ids)},
                       indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 2
    except Exception as exc:
        (output / "result.json").write_text(
            json.dumps({"status": "UNQUALIFIED",
                        "reason": "MININDN_OWNER_FAILED:" + type(exc).__name__ + ":" + str(exc),
                        "campaignCase": case_id}, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        return 2
    finally:
        if ndn is not None:
            try:
                ndn.stop()
            except Exception:
                pass


def run_campaign(manifest_path: Path, output: Path, *, execute_owner: bool = False,
                 runner_manifest: Path | None = None,
                 runner_case: str | None = None) -> int:
    """Run one manifest-selected case after an external MiniNDN owner setup.

    The default mode preserves the registration-only preflight.  Explicit
    ``execute_owner`` creates the tracked MiniNDN topology and exports real
    node context, then stops at UNQUALIFIED until a closure case is frozen.
    """
    if execute_owner:
        try:
            return _run_owned_campaign(manifest_path, output, runner_manifest, runner_case)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            output = output.resolve()
            if output.exists():
                return 2
            try:
                output.mkdir(parents=True, exist_ok=False)
                (output / "result.json").write_text(
                    json.dumps({"status": "UNQUALIFIED", "reason": str(exc)},
                               sort_keys=True) + "\n", encoding="utf-8")
            except OSError:
                pass
            return 2
    output = output.resolve()
    output_preexisting = output.exists()
    try:
        if output_preexisting:
            raise ValueError("output run directory must be new")
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
        case_id = document.get("campaignCase", "")
        if not isinstance(case_id, str) or not case_id:
            raise ValueError("campaignCase is required")
        registration = load_registration(manifest_path)
        registered_ids = {case["id"] for case in registration["cases"]}
        if case_id not in registered_ids:
            raise ValueError("campaignCase is not registered")
        output.mkdir(parents=True, exist_ok=False)
        (output / "result.json").write_text(
            json.dumps({
                "status": "UNQUALIFIED",
                "reason": "MININDN_NODE_CONTEXT_NOT_PROVIDED",
                "campaignCase": case_id,
                "runner": registration["runner"],
                "registeredCases": sorted(registered_ids),
                "limits": registration["limits"],
            }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 2
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        if output_preexisting:
            return 2
        if not output.exists():
            try:
                output.mkdir(parents=True, exist_ok=False)
            except OSError:
                return 2
        if output.is_dir():
            (output / "result.json").write_text(
                json.dumps({"status": "UNQUALIFIED", "reason": str(exc)},
                           sort_keys=True) + "\n", encoding="utf-8")
        return 2


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--execute-owner", action="store_true",
                        help="create the isolated MiniNDN owner topology and export node context")
    parser.add_argument("--runner-manifest", type=Path,
                        help="optional canonical runner case manifest to execute while owner is alive")
    parser.add_argument("--runner-case",
                        help="case ID in --runner-manifest (defaults to campaignCase)")
    args = parser.parse_args(argv)
    if args.runner_manifest is not None and not args.execute_owner:
        parser.error("--runner-manifest requires --execute-owner")
    return run_campaign(args.manifest, args.output, execute_owner=args.execute_owner,
                        runner_manifest=args.runner_manifest, runner_case=args.runner_case)


if __name__ == "__main__":
    raise SystemExit(main())
