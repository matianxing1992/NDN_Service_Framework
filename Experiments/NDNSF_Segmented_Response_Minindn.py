#!/usr/bin/env python3
"""Run one immutable Spec 112 segmented-response MiniNDN evidence cell."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import sys
import time
from typing import Any, Dict, List, Optional


REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_Python_Hello_Minindn as hello  # noqa: E402
import spec112_segmented_campaign as evidence  # noqa: E402
from mininet.log import setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402


def positive_int(value: str) -> int:
    try:
        number = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer") from exc
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def positive_float(value: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run one immutable Spec 112 segmented-response MiniNDN cell")
    parser.add_argument("--candidate-manifest", type=Path, required=True)
    parser.add_argument("--topology-file", type=Path, default=hello.DEFAULT_TOPOLOGY)
    parser.add_argument("--user-node", default="memphis")
    parser.add_argument("--controller-node", default="memphis")
    parser.add_argument("--provider-node", default="ucla")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--sizes", default="64x2,4000,5000,6500,8000,16000,64x3")
    parser.add_argument("--mode", choices=("normal", "targeted"), default="normal")
    parser.add_argument("--targeted-api", choices=("sync", "async"), default="sync")
    parser.add_argument("--fault-profile", choices=evidence.FAULT_PROFILES, default="none")
    parser.add_argument("--ack-timeout-ms", type=positive_int, default=1000)
    parser.add_argument("--timeout-ms", type=positive_int, default=4000)
    parser.add_argument("--startup-wait-s", type=positive_float, default=5.0)
    parser.add_argument("--controller-wait-s", type=positive_float, default=3.0)
    parser.add_argument("--wall-timeout-s", type=positive_float)
    parser.add_argument("--min-free-bytes", type=positive_int, default=2 * 1024**3)
    parser.add_argument("--ownership-lock", type=Path,
                        default=Path("/tmp/ndnsf-spec112-minindn.lock"))
    parser.add_argument("--nfd-log-level", default="WARN")
    parser.add_argument("--svs-sync-publish", action="store_true")
    return parser


def parse_result_lines(text: str) -> List[Dict[str, Any]]:
    prefix = "SEGMENTED_RESPONSE_RESULT "
    results = []
    for line in text.splitlines():
        if not line.startswith(prefix):
            continue
        try:
            value = json.loads(line[len(prefix):])
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            results.append(value)
    return results


def stop_one(proc, grace_s: float = 5.0) -> Optional[int]:
    if proc is None:
        return None
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=grace_s)
        except Exception:
            proc.kill()
            try:
                proc.wait(timeout=grace_s)
            except Exception:
                pass
    return proc.returncode


def _read_results(path: Optional[Path]) -> List[Dict[str, Any]]:
    if path is None or not path.is_file():
        return []
    return parse_result_lines(path.read_text(encoding="utf-8", errors="replace"))


def _wait_for_process(
    proc,
    output_dir: Path,
    *,
    campaign_started: float,
    wall_timeout_s: float,
    min_free_bytes: int,
    until_result_count: Optional[int] = None,
    user_log: Optional[Path] = None,
) -> Optional[str]:
    while proc.poll() is None:
        if until_result_count is not None and len(_read_results(user_log)) >= until_result_count:
            return None
        reason = evidence.resource_stop_reason(
            output_dir,
            started_monotonic=campaign_started,
            now_monotonic=time.monotonic(),
            wall_timeout_seconds=wall_timeout_s,
            min_free_bytes=min_free_bytes,
        )
        if reason is not None:
            proc.kill()
            try:
                proc.wait(timeout=5)
            except Exception:
                pass
            return reason
        time.sleep(0.2)
    return None


def _runtime_config(
    args: argparse.Namespace,
    manifest: Dict[str, Any],
    plan: Dict[str, Any],
    topology: Dict[str, Any],
    wall_timeout_s: float,
) -> Dict[str, Any]:
    return {
        "schemaVersion": "spec112-segmented-cell-config-v1",
        "candidateId": manifest["candidateId"],
        "candidateIdentitySha256": manifest["identitySha256"],
        "candidateManifest": str(args.candidate_manifest.resolve()),
        "cellId": args.output_dir.resolve().name,
        "command": list(sys.argv),
        "automaticRetry": False,
        "mode": args.mode,
        "targetedApi": args.targeted_api if args.mode == "targeted" else "not-applicable",
        "svsPublish": "sync" if args.svs_sync_publish else "async",
        "faultProfile": args.fault_profile,
        "sizePlan": plan,
        "topology": topology,
        "timeouts": {
            "ackTimeoutMs": args.ack_timeout_ms,
            "requestTimeoutMs": args.timeout_ms,
            "wallTimeoutSeconds": wall_timeout_s,
        },
        "diskFloorBytes": args.min_free_bytes,
        "forcedRoleEnvironment": {
            "NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE": "1",
            "SPEC112_FORCED_INLINE_SVS": "1",
            "NDNSF_SVS_ASYNC_PUBLISH": "0" if args.svs_sync_publish else "1",
        },
    }


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    args.candidate_manifest = args.candidate_manifest.resolve()
    args.output_dir = args.output_dir.resolve()
    args.topology_file = args.topology_file.resolve()
    if args.mode != "targeted" and args.targeted_api != "sync":
        parser.error("--targeted-api async requires --mode targeted")

    try:
        manifest = evidence.load_candidate_manifest(args.candidate_manifest, repo_root=REPO)
        plan = evidence.execution_plan(args.sizes, args.mode, args.fault_profile)
        topology = evidence.verify_zero_loss_topology(args.topology_file)
    except (ValueError, RuntimeError, OSError) as exc:
        parser.error(str(exc))

    requested_count = len(plan["expandedSizes"])
    wall_timeout_s = args.wall_timeout_s or (
        args.controller_wait_s + args.startup_wait_s
        + requested_count * (args.timeout_ms / 1000.0 + 1.0) + 30.0
    )
    try:
        evidence.reserve_cell_directory(args.output_dir, args.candidate_manifest)
    except (ValueError, RuntimeError) as exc:
        parser.error(str(exc))

    owner_lock = None
    try:
        owner_lock = evidence.acquire_minindn_ownership(
            args.ownership_lock,
            args.output_dir / "owner.json",
        )
    except RuntimeError as exc:
        evidence._atomic_json(
            args.output_dir / "launch-error.json",
            {
                "schemaVersion": "spec112-launch-error-v1",
                "candidateId": manifest["candidateId"],
                "cellId": args.output_dir.name,
                "error": str(exc),
                "stage": "MiniNDN-ownership",
            },
        )
        print(f"error: {exc}", file=sys.stderr)
        return 2

    config = _runtime_config(args, manifest, plan, topology, wall_timeout_s)
    evidence._atomic_json(args.output_dir / "cell-config.json", config)
    owner = json.loads((args.output_dir / "owner.json").read_text(encoding="utf-8"))

    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    setLogLevel("info")
    processes = []
    ndn = None
    provider_proc = None
    provider_log: Optional[Path] = None
    user_proc = None
    user_log: Optional[Path] = None
    provider_epoch: Dict[str, Any] = {}
    runtime_error = ""
    stop_reason: Optional[str] = None
    fault_injection: Dict[str, Any] = {"applied": False}
    campaign_started = time.monotonic()
    session = int(time.time()) + os.getpid()

    try:
        if evidence.resource_stop_reason(
            args.output_dir,
            started_monotonic=campaign_started,
            now_monotonic=campaign_started,
            wall_timeout_seconds=wall_timeout_s,
            min_free_bytes=args.min_free_bytes,
        ) == "disk-floor":
            raise RuntimeError("disk free-space floor is already violated")

        for lock_path in (Path("/tmp/ndnsf-python-provider-keychain.lock"),
                          Path("/tmp/ndnsf-python-user-keychain.lock")):
            try:
                lock_path.unlink()
            except FileNotFoundError:
                pass

        perf_args = hello.make_perf_args(args)
        perf_args.svs_sync_publish = args.svs_sync_publish

        # Cleanup is permitted only after the global lock and process-level
        # ownership checks above have succeeded.
        Minindn.cleanUp()
        Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(args.topology_file))
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel=args.nfd_log_level)
        hello.perf.wait_for_nfd_sockets(ndn, args.output_dir)
        hello.configure_routes(ndn, args)
        hello.perf.initialize_example_keychains(ndn, perf_args, args.output_dir)

        env = evidence.role_environment(
            hello.perf.app_env(args.output_dir, session, perf_args),
            REPO,
            svs_sync_publish=args.svs_sync_publish,
        )

        controller_cmd = hello.python_cmd(
            "hello_controller.py",
            "--policy-file", "examples/hello.policies",
            "--binary-dir", "build/examples",
            "--library-dir", "build",
        )
        hello.start(ndn.net[args.controller_node], "controller", controller_cmd,
                    env, args.output_dir, processes)
        time.sleep(args.controller_wait_s)

        provider_cmd = hello.python_cmd(
            "segmented_response_provider.py",
            "--binary-dir", "build/examples",
            "--library-dir", "build",
        )
        provider_proc, provider_log = hello.start(
            ndn.net[args.provider_node], "provider", provider_cmd,
            env, args.output_dir, processes)
        provider_epoch = {
            "node": args.provider_node,
            "pid": provider_proc.pid,
            "session": session,
            "startedAt": time.time(),
            "restartCount": 0,
        }
        time.sleep(args.startup_wait_s)
        if provider_proc.poll() is not None:
            raise RuntimeError(f"provider exited during startup; see {provider_log}")

        run_id = manifest["candidateId"] + "-" + hashlib.sha256(
            args.output_dir.name.encode("utf-8")
        ).hexdigest()[:12]
        user_arguments = [
            "--run-id", run_id,
            "--mode", args.mode,
            "--targeted-api", args.targeted_api,
            "--sizes", plan["effectiveSizes"],
            "--ack-timeout-ms", str(args.ack_timeout_ms),
            "--timeout-ms", str(args.timeout_ms),
            "--binary-dir", "build/examples",
            "--library-dir", "build",
        ]
        resume_file = args.output_dir / "provider-fault-resume"
        if plan["pauseAfterIndex"] is not None:
            user_arguments.extend([
                "--pause-after-index", str(plan["pauseAfterIndex"]),
                "--resume-file", str(resume_file),
                "--pause-timeout-s", str(max(10.0, args.timeout_ms / 1000.0 + 5.0)),
            ])
        user_cmd = hello.python_cmd("segmented_response_user.py", *user_arguments)
        user_proc, user_log = hello.start(
            ndn.net[args.user_node], "user", user_cmd, env, args.output_dir, processes)

        if args.fault_profile == "degraded-provider-after-targeted-bootstrap":
            stop_reason = _wait_for_process(
                user_proc,
                args.output_dir,
                campaign_started=campaign_started,
                wall_timeout_s=wall_timeout_s,
                min_free_bytes=args.min_free_bytes,
                until_result_count=1,
                user_log=user_log,
            )
            if stop_reason is None and len(_read_results(user_log)) >= 1:
                provider_return = stop_one(provider_proc)
                fault_injection = {
                    "applied": True,
                    "afterResultCount": 1,
                    "providerReturnCode": provider_return,
                    "atMonotonicSeconds": round(time.monotonic() - campaign_started, 3),
                }
                resume_file.write_text("provider stopped\n", encoding="utf-8")

        if stop_reason is None:
            stop_reason = _wait_for_process(
                user_proc,
                args.output_dir,
                campaign_started=campaign_started,
                wall_timeout_s=wall_timeout_s,
                min_free_bytes=args.min_free_bytes,
                user_log=user_log,
            )
    except Exception as exc:
        runtime_error = f"{type(exc).__name__}: {exc}"
    finally:
        provider_alive = bool(provider_proc is not None and provider_proc.poll() is None)
        hello.stop(processes)
        if ndn is not None:
            try:
                ndn.stop()
            except Exception as exc:
                runtime_error = runtime_error or f"ndn.stop: {type(exc).__name__}: {exc}"
        try:
            Minindn.cleanUp()
        except Exception as exc:
            runtime_error = runtime_error or f"MiniNDN cleanup: {type(exc).__name__}: {exc}"
        sys.argv = original_argv

    results = _read_results(user_log)
    if args.fault_profile == "degraded-provider-after-targeted-bootstrap" and len(results) > 1:
        results[1].setdefault("deadlineLimitMs", args.timeout_ms + 500)
        results[1].setdefault(
            "deadlineWithinLimit",
            float(results[1].get("elapsedMs", float("inf"))) <= args.timeout_ms + 500,
        )
        results[1].setdefault("timeoutTerminalCount", 1 if not results[1].get("status") else 0)
        results[1].setdefault("responseTerminalCount", 1 if results[1].get("status") else 0)
    if user_log is not None and user_log.is_file():
        user_text = user_log.read_text(encoding="utf-8", errors="replace")
        print(user_text, end="" if user_text.endswith("\n") else "\n")

    no_reference = evidence.no_reference_proof(
        args.output_dir,
        forced_value="1",
    )
    summary = evidence.make_cell_summary(
        candidate_id=manifest["candidateId"],
        cell_id=args.output_dir.name,
        mode=args.mode,
        svs_publish="sync" if args.svs_sync_publish else "async",
        fault_profile=args.fault_profile,
        requested_sizes=plan["expandedSizes"],
        results=results,
        owner=owner,
        provider_epoch=provider_epoch,
        no_reference=no_reference,
        user_return_code=user_proc.returncode if user_proc is not None else None,
        provider_return_code=provider_proc.returncode if provider_proc is not None else None,
        provider_alive=provider_alive,
        user_hung=stop_reason == "wall-timeout",
        wall_stop=stop_reason == "wall-timeout",
        disk_stop=stop_reason == "disk-floor",
        elapsed_seconds=time.monotonic() - campaign_started,
    )
    summary["runtimeError"] = runtime_error
    summary["faultInjection"] = fault_injection
    summary["configPath"] = str((args.output_dir / "cell-config.json").resolve())
    if runtime_error:
        summary["status"] = "FAILURE"
    evidence.write_cell_and_candidate_summaries(
        args.output_dir, args.candidate_manifest, summary)
    owner_lock.close()

    summary_path = args.output_dir / "cell-summary.json"
    print("SEGMENTED_MININDN_SUMMARY " + json.dumps(summary, sort_keys=True))
    print(f"SEGMENTED_MININDN_RESULT {summary_path}")
    return 0 if summary["status"] == "SUCCESS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
