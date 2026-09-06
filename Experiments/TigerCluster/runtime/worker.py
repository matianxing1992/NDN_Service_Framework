"""One Slurm task per node, owning all forwarders and workload processes."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time

from runtime.baseline import (BIN, PYTHON, Processes, bundle_files, configure_routes,
                              container_command, container_env, digest, native_trust_rejection,
                              nfd_config, wait_json, write_json)


def run_finite_application(name, argv, log_path, cleanup, seconds=60,
                           allowed_exits=(0,), env=None):
    """Wait for an owned application and record cleanup of its entire group.

    A finite application's expected exit is not a premature service death.
    Descendants surviving that exit must be killed and prevent qualification.
    Timeout, cancellation, and unexpected exits retain their original failure.
    """
    with Path(log_path).open("wb") as log:
        proc = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True, env=env)
        forced = False
        try:
            rc = proc.wait(timeout=seconds)
        finally:
            try:
                if proc.poll() is None:
                    # Let Apptainer forward TERM before removing its mounts.
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        forced = True
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait(timeout=5)
                # Waiting for the leader does not wait for its descendants.
                # This check also covers successful and permitted abort exits.
                try:
                    os.killpg(proc.pid, 0)
                    os.killpg(proc.pid, signal.SIGKILL)
                    forced = True
                except ProcessLookupError:
                    pass
            finally:
                cleanup.append({"name": name, "kind": "finite", "pid": proc.pid,
                                "exitCode": proc.poll(), "forced": forced,
                                "reaped": proc.poll() is not None,
                                "exitedBeforeCleanup": False})
        if rc not in allowed_exits:
            raise RuntimeError("APP_EXIT:" + name + ":" + str(rc))
        return rc


def execute(run_path: Path, rank: int) -> int:
    run = json.loads(run_path.read_text())
    root, bundle = run_path.parent, Path(run["bundle"])
    profile, namespace = run["profile"], run["namespace"]
    public, barriers = root / "public", root / "barriers"
    out = root / ("node" + str(rank))
    node = Path(tempfile.mkdtemp(prefix="tiger-" + run["runId"] + "-"))
    children = Processes(out / "logs")
    finite_cleanup = []
    result = {key: run[key] for key in ("runId", "sifSha256", "profileSha256", "bundleFiles")}
    result.update(rank=rank, hostname=socket.gethostname(), cases={}, status="FAIL", launches=[])
    def command(role, argv):
        role_output = out / role
        role_output.mkdir(parents=True, exist_ok=True)
        cmd = container_command(profile, bundle, root / "private" / role,
                                public, role_output, argv, node=node)
        result["launches"].append({"role": role, "argv": cmd})
        return cmd
    def wait(path):
        return wait_json(path, profile["startupSeconds"], children, barriers)
    def run_app(name, role, argv, seconds=60, allowed_exits=(0,)):
        return run_finite_application(name, command(role, argv), out / "logs" / (name + ".log"),
                                      finite_cleanup, seconds, allowed_exits, container_env())
    def on_signal(*_):
        raise InterruptedError("WORKER_CANCELLED")
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    try:
        if bundle_files(bundle) != run["bundleFiles"]:
            raise RuntimeError("BUNDLE_CHANGED")
        if digest(Path(profile["sif"])) != run["sifSha256"]:
            raise RuntimeError("SIF_CHANGED")
        version = subprocess.check_output([profile["apptainer"], "--version"], text=True).strip()
        if version != "apptainer version " + profile["apptainerVersion"]:
            raise RuntimeError("APPTAINER_CHANGED")
        result["apptainer"] = version
        nfd_role = "nfd" + str(rank)
        run_app("runtime-identity", nfd_role, [PYTHON, "/bundle/runtime/inspect_runtime.py"])
        result["runtime"] = json.loads((out / nfd_role / "runtime.json").read_text())
        nfd_out = out / nfd_role
        nfd_out.mkdir(parents=True, exist_ok=True)
        (nfd_out / "nfd.conf").write_text(nfd_config(run["nodes"][rank]["port"]))
        children.start("nfd", command(nfd_role, [BIN + "/nfd", "--config", "/output/nfd.conf"]), container_env())
        deadline = time.monotonic() + profile["startupSeconds"]
        while True:
            children.check()
            status = subprocess.run(command(nfd_role, [BIN + "/nfdc", "status", "report"]),
                                    capture_output=True, text=True, timeout=10, env=container_env())
            if status.returncode == 0:
                (nfd_out / "status.txt").write_text(status.stdout)
                break
            if time.monotonic() >= deadline:
                raise TimeoutError("NFD_NOT_READY:" + status.stderr[-400:])
            time.sleep(0.2)
        write_json(barriers / f"nfd-{rank}.json", {"hostname": socket.gethostname()})
        wait(barriers / f"nfd-{1-rank}.json")
        remote = run["nodes"][1-rank]
        configure_routes(lambda args: command(nfd_role, args), namespace,
                         remote["address"], remote["port"], out / "routes.json")
        raw_role = "raw" + str(rank)
        raw_args = [PYTHON, "/bundle/apps/ndn_probe.py", "producer",
                    "--namespace", namespace, "--role", raw_role]
        children.start("raw-producer", command(raw_role, raw_args), container_env())
        wait(out / raw_role / "ready.json")
        write_json(barriers / f"raw-{rank}.json", {"status": "READY"})
        wait(barriers / f"raw-{1-rank}.json")
        # Use a separate identity for consumer access to avoid concurrent SQLite
        # ownership of the raw producer's PIB. nfd role has no app listener.
        run_app("raw-consumer", nfd_role,
                [PYTHON, "/bundle/apps/ndn_probe.py", "consumer", "--namespace", namespace,
                 "--role", "raw" + str(1-rank), "--timeout-ms", "5000"])
        result["cases"].update(json.loads((out / nfd_role / "result.json").read_text())["cases"])
        write_json(barriers / f"raw-done-{rank}.json", {"status": "PASS"})
        wait(barriers / f"raw-done-{1-rank}.json")
        common = ["--namespace", namespace, "--ack-ms", str(profile["ackTimeoutMs"]),
                  "--timeout-ms", str(profile["requestTimeoutMs"]), "--payload", run["payload"]]
        if rank == 0:
            children.start("controller", command("controller",
                [BIN + "/App_ServiceController", "--controller-prefix", namespace + "/controller",
                 "--policy-file", "/config/policy.conf", "--trust-schema", "/config/trust.conf",
                 "--bootstrap-token-file", "/config/bootstrap-tokens.txt"]), container_env())
            # r119's start is asynchronous: require a real, signed authority
            # response before releasing the cross-node Provider barrier.
            run_app("controller-readiness", nfd_role,
                    [PYTHON, "/bundle/apps/ndn_probe.py", "controller-ready",
                     "--namespace", namespace, "--role", nfd_role, "--timeout-ms", "10000"])
            write_json(barriers / "controller.json", {"status": "READY"})
        else:
            wait(barriers / "controller.json")
            children.start("provider", command("provider",
                [PYTHON, "/bundle/apps/service_probe.py", "provider", *common]), container_env())
            wait(out / "provider/started.json")
            write_json(barriers / "provider.json", {"status": "START_RETURNED"})
        if rank == 0:
            wait(barriers / "provider.json")
            run_app("user", "user", [PYTHON, "/bundle/apps/service_probe.py", "user", *common])
            result["cases"]["service-echo"] = json.loads((out / "user/result.json").read_text())
            run_app("denied", "denied", [PYTHON, "/bundle/apps/service_probe.py", "denied", *common])
            deny = json.loads((out / "denied/result.json").read_text())
            native_log = (out / "logs/denied.log").read_text(errors="replace")
            if "Reject request without user permission serviceName=/TIGER_ECHO" not in native_log:
                raise RuntimeError("NO_NATIVE_PERMISSION_REJECTION")
            provider_calls = json.loads((root / "node1/provider/calls.json").read_text())["calls"]
            if provider_calls != [run["payload"]]:
                raise RuntimeError("PROVIDER_CALL_ORACLE:" + repr(provider_calls))
            deny.update(nativeRejection=True, providerCalls=provider_calls)
            result["cases"]["permission-rejection"] = deny
            untrusted_exit = run_app("untrusted", "user",
                [PYTHON, "/bundle/apps/service_probe.py", "untrusted", *common], allowed_exits=(0, 134, -6))
            untrusted_path = out / "user/untrusted.json"
            untrusted = json.loads(untrusted_path.read_text()) if untrusted_path.exists() else {}
            log = (out / "logs/untrusted.log").read_text(errors="replace")
            result["cases"]["native-trust-rejection"] = native_trust_rejection(untrusted_exit, log, untrusted)
            if json.loads((root / "node1/provider/calls.json").read_text())["calls"] != [run["payload"]]:
                raise RuntimeError("UNTRUSTED_PROVIDER_EXECUTION")
            write_json(barriers / "service-done.json", {"status": "PASS"})
        else:
            wait(barriers / "service-done.json")
        result["status"] = "PASS"
    except BaseException as exc:
        result["error"] = type(exc).__name__ + ":" + str(exc)
        write_json(barriers / f"failed-{rank}.json", {"error": result["error"]})
    finally:
        result["cleanup"] = finite_cleanup + children.close()
        result["socketRemoved"] = not (node / "nfd.sock").exists()
        shutil.rmtree(node)
        write_json(out / "result.json", result)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("run", type=Path)
    parser.add_argument("--rank", type=int)
    args = parser.parse_args()
    rank = args.rank if args.rank is not None else int(os.environ["SLURM_PROCID"])
    raise SystemExit(execute(args.run.resolve(), rank))
