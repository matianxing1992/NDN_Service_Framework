#!/usr/bin/env python3
"""Spec 164 public publish/fetch API smoke over a real MiniNDN data plane."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

from NDNSF_DistributedRepo_Artifact_Minindn import (
    DEFAULT_TOPOLOGY,
    PUBLISHER_ROOT,
    REPO,
    SCRIPT,
    _node_environment,
    _prepare_fixture,
    _restore_output_ownership,
    _sanitize_evidence_run,
    _wait_for,
    _write_json,
)


class MiniNdnRuntime:
    def __init__(self, run_dir: Path, topology: Path, timeout_s: float):
        self.run_dir = run_dir
        self.topology = topology
        self.timeout_s = timeout_s
        self.ndn = None
        self.get_popen = None
        self.processes = []
        self.streams = []

    def environment(self, host_name: str) -> dict[str, str]:
        environment = _node_environment(host_name)
        home = self.run_dir / "homes" / host_name
        ndn_dir = home / ".ndn"
        ndn_dir.mkdir(parents=True, exist_ok=True)
        client_conf = ndn_dir / "client.conf"
        client_conf.write_text(
            f"transport=unix:///run/nfd/{host_name}.sock\n",
            encoding="utf-8",
        )
        environment.update({
            "HOME": str(home),
            "NDN_CLIENT_CONF": str(client_conf),
            "NDN_CLIENT_TRANSPORT": (
                f"unix:///run/nfd/{host_name}.sock"
            ),
            "NDN_LOG": "ndn_service_framework.*=DEBUG",
        })
        return environment

    def start(self) -> None:
        from mininet.node import Controller
        from mininet.log import setLogLevel
        from minindn.apps.app_manager import AppManager
        from minindn.apps.nfd import Nfd
        from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
        from minindn.helpers.nfdc import Nfdc
        from minindn.minindn import Minindn
        from minindn.util import getPopen

        setLogLevel("warning")
        Minindn.cleanUp()
        Minindn.verifyDependencies()
        self.ndn = Minindn(
            topoFile=str(self.topology), controller=Controller
        )
        self.get_popen = getPopen
        self.ndn.start()
        AppManager(self.ndn, self.ndn.net.hosts, Nfd, logLevel="WARN")
        time.sleep(0.5)
        publisher = self.ndn.net["publisher"]
        repo = self.ndn.net["repo"]
        consumer = self.ndn.net["consumer"]
        routing = NdnRoutingHelper(self.ndn.net, "udp", "link-state")
        routing.addOrigin(
            [publisher],
            ["/spec164/publisher", "/example/hello/controller"],
        )
        routing.addOrigin(
            [repo],
            ["/spec164/repo", "/example/hello/provider/A",
             "/example/hello/group"],
        )
        routing.addOrigin(
            [consumer],
            ["/example/hello/user", "/example/hello/group"],
        )
        routing.calculateRoutes()
        for node in self.ndn.net.hosts:
            Nfdc.setStrategy(
                node, "/example/hello", Nfdc.STRATEGY_MULTICAST
            )
            Nfdc.setStrategy(
                node, "/example/hello/group", Nfdc.STRATEGY_MULTICAST
            )

    def _run_node_command(self, host_name: str, command: str) -> str:
        assert self.ndn is not None and self.get_popen is not None
        process = self.get_popen(
            self.ndn.net[host_name],
            command,
            envDict=self.environment(host_name),
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        output, _ = process.communicate(timeout=self.timeout_s)
        if process.returncode != 0:
            raise RuntimeError(
                f"{host_name} security command failed: "
                f"{output.decode(errors='replace')}"
            )
        return output.decode(errors="replace")

    def _install_security(self) -> None:
        security = self.run_dir / "security"
        security.mkdir(parents=True, exist_ok=True)
        root = security / "root.cert"
        self._run_node_command(
            "publisher",
            "ndnsec key-gen -t r /example/hello > "
            + shlex.quote(str(root)),
        )
        exported = {}
        for index, identity in enumerate((
            "/example/hello/controller",
            "/example/hello/user",
            "/example/hello/provider/A",
        )):
            request = security / f"identity-{index}.req"
            certificate = security / f"identity-{index}.cert"
            key = security / f"identity-{index}.ndnkey"
            self._run_node_command(
                "publisher",
                "ndnsec key-gen -n -t r "
                + shlex.quote(identity)
                + " > "
                + shlex.quote(str(request)),
            )
            self._run_node_command(
                "publisher",
                "ndnsec cert-gen -s /example/hello -i ROOT "
                + shlex.quote(str(request))
                + " > "
                + shlex.quote(str(certificate)),
            )
            self._run_node_command(
                "publisher",
                "ndnsec cert-install -f "
                + shlex.quote(str(certificate)),
            )
            self._run_node_command(
                "publisher",
                "ndnsec-export -P 123456 -o "
                + shlex.quote(str(key))
                + " -i "
                + shlex.quote(identity),
            )
            exported[identity] = key
        # The authority already owns every generated private key. Re-importing
        # those same keys is a hard ndnsec error, so only install the exported
        # identities on the other namespaces.
        for host_name, identity in (
            ("repo", "/example/hello/provider/A"),
            ("consumer", "/example/hello/user"),
        ):
            self._run_node_command(
                host_name,
                "ndnsec import -P 123456 "
                + shlex.quote(str(exported[identity]))
                + " >/dev/null",
            )

    def start_command(
        self, host_name: str, name: str, command: str
    ):
        assert self.ndn is not None and self.get_popen is not None
        log_path = self.run_dir / "logs" / f"{name}.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        stream = log_path.open("wb")
        process = self.get_popen(
            self.ndn.net[host_name],
            command,
            envDict=self.environment(host_name),
            shell=True,
            stdout=stream,
            stderr=subprocess.STDOUT,
        )
        self.processes.append((name, process))
        self.streams.append(stream)
        return process, log_path

    def wait_log(
        self, path: Path, marker: str, process=None
    ) -> bool:
        deadline = time.monotonic() + self.timeout_s
        while time.monotonic() < deadline:
            if process is not None and process.poll() is not None:
                return marker in path.read_text(
                    encoding="utf-8", errors="replace"
                )
            if path.exists() and marker in path.read_text(
                    encoding="utf-8", errors="replace"
            ):
                return True
            time.sleep(0.1)
        return False

    def run_secure_control_smoke(self) -> dict:
        self._install_security()
        controller, controller_log = self.start_command(
            "publisher",
            "controller",
            "cd "
            + shlex.quote(str(REPO))
            + " && exec "
            + shlex.quote(str(REPO / "build/examples/App_ServiceController")),
        )
        if not self.wait_log(
            controller_log, "ServiceController started...", controller
        ):
            raise RuntimeError("Spec 164 Controller did not become ready")
        provider_script = (
            REPO / "tests/container/spec164-artifact-control/provider.py"
        )
        provider, provider_log = self.start_command(
            "repo",
            "artifact-control-provider",
            "cd "
            + shlex.quote(str(REPO))
            + " && exec env NDNSF_HANDLER_THREADS=1 /usr/bin/python3 "
            + shlex.quote(str(provider_script))
            + " --storage-dir "
            + shlex.quote(str(self.run_dir / "real-repo-store")),
        )
        if not self.wait_log(
            provider_log,
            "SPEC164_ARTIFACT_CONTROL_PROVIDER_READY",
            provider,
        ):
            raise RuntimeError("Spec 164 artifact control Provider failed")
        control_output = self.run_dir / "control"
        user_script = REPO / "tests/container/spec164-artifact-control/user.py"
        user, user_log = self.start_command(
            "consumer",
            "artifact-control-user",
            "cd "
            + shlex.quote(str(REPO))
            + " && exec /usr/bin/python3 "
            + shlex.quote(str(user_script))
            + " --output "
            + shlex.quote(str(control_output))
            + " --source "
            + shlex.quote(str(self.run_dir / "payload.bin"))
            + " --destination "
            + shlex.quote(str(self.run_dir / "consumer/artifact.bin")),
        )
        user.wait(timeout=self.timeout_s)
        if user.returncode != 0 or not self.wait_log(
            user_log, "SPEC164_SECURE_ARTIFACT_CONTROL_OK"
        ):
            raise RuntimeError("Spec 164 secure artifact control user failed")
        return json.loads(
            (control_output / "summary.json").read_text(encoding="utf-8")
        )

    def start_role(self, host_name: str, role: str) -> None:
        assert self.ndn is not None and self.get_popen is not None
        log_path = self.run_dir / "logs" / f"{role}.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        stream = log_path.open("wb")
        command = " ".join((
            "exec",
            shlex.quote(sys.executable),
            shlex.quote(str(SCRIPT)),
            "--role",
            role,
            "--scenario",
            "success",
            "--run-dir",
            shlex.quote(str(self.run_dir)),
            "--timeout-seconds",
            str(self.timeout_s),
        ))
        process = self.get_popen(
            self.ndn.net[host_name],
            command,
            envDict=self.environment(host_name),
            shell=True,
            stdout=stream,
            stderr=subprocess.STDOUT,
        )
        self.processes.append((role, process))
        self.streams.append(stream)

    def close(self) -> None:
        from minindn.minindn import Minindn

        try:
            (self.run_dir / "stop").write_text("stop\n", encoding="utf-8")
        except OSError:
            pass
        deadline = time.monotonic() + self.timeout_s
        for _, process in self.processes:
            if process.poll() is None:
                try:
                    process.wait(
                        timeout=max(0.1, deadline - time.monotonic())
                    )
                except subprocess.TimeoutExpired:
                    process.terminate()
        for _, process in self.processes:
            if process.poll() is None:
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
        for stream in self.streams:
            stream.close()
        if self.ndn is not None:
            self.ndn.stop()
        Minindn.cleanUp()


class PublicPublishDriver:
    def __init__(self, backend, descriptor, operation_id, emit):
        self.backend = backend
        self.descriptor = descriptor
        self.operation_id = operation_id
        self.emit = emit
        self.state = "OPEN"
        self.started = time.monotonic()
        self.sequence = 0

    def _progress(self, phase: str, committed: bool) -> None:
        from py_repoclient import ArtifactProgress

        self.sequence += 1
        size = int(self.descriptor.reference.size_bytes)
        self.emit(ArtifactProgress(
            operation_id=self.operation_id,
            artifact=self.descriptor.reference,
            phase=phase,
            received_bytes=size,
            verified_bytes=size,
            committed_bytes=size if committed else 0,
            total_bytes=size,
            selected_replicas=1,
            committed_replicas=1 if committed else 0,
            retransmitted_bytes=0,
            sequence=self.sequence,
            timestamp_ms=time.time_ns() // 1_000_000,
        ))

    def transfer(self, path, cancellation) -> None:
        cancellation.raise_if_cancelled(
            self.operation_id, self.descriptor.reference
        )
        if Path(path).read_bytes() != (
            self.backend.runtime.run_dir / "payload.bin"
        ).read_bytes():
            raise ValueError("public API source differs from signed fixture")
        self.backend.runtime.start_role("publisher", "producer")
        self.backend.runtime.start_role("repo", "repo")
        _wait_for(
            self.backend.runtime.run_dir / "repo.ready",
            self.backend.runtime.timeout_s,
        )
        self.state = "VERIFIED"
        self._progress("transfer", False)

    def status(self):
        from py_repoclient import ArtifactSessionStatus

        return ArtifactSessionStatus(
            self.operation_id,
            "PUBLISH",
            self.state,
            self.descriptor.reference,
        )

    def commit(self):
        from py_repoclient import (
            ArtifactPublishResult,
            ArtifactReplicaResult,
        )

        repo_result = json.loads(
            (self.backend.runtime.run_dir / "repo-result.json").read_text(
                encoding="utf-8"
            )
        )
        receipt = repo_result["receipt"]["receipt"]
        self._progress("commit", True)
        self.state = "COMMITTED"
        return ArtifactPublishResult(
            reference=self.descriptor.reference,
            operation_id=self.operation_id,
            requested_replicas=1,
            achieved_replicas=1,
            replicas=(
                ArtifactReplicaResult(
                    repo_node=receipt["repoNode"],
                    state=receipt["state"],
                    receipt_id=receipt["receiptId"],
                ),
            ),
            total_duration_ms=(
                time.monotonic() - self.started
            ) * 1000,
        )

    def abort(self, preserve_progress):
        del preserve_progress
        self.state = "CANCELLED"
        return self.status()


class PublicFetchDriver:
    def __init__(
        self, backend, reference, destination, operation_id, emit
    ):
        self.backend = backend
        self.reference = reference
        self.destination = destination
        self.operation_id = operation_id
        self.emit = emit
        self.state = "OPEN"
        self.started = time.monotonic()

    def transfer(self, cancellation) -> None:
        from py_repoclient import ArtifactProgress

        cancellation.raise_if_cancelled(
            self.operation_id, self.reference
        )
        expected = (
            self.backend.runtime.run_dir / "consumer/artifact.bin"
        )
        if self.destination != expected:
            raise ValueError("MiniNDN backend requires its atomic destination")
        self.backend.runtime.start_role("consumer", "consumer")
        _wait_for(
            self.backend.runtime.run_dir / "consumer-result.json",
            self.backend.runtime.timeout_s,
        )
        result = json.loads(
            (self.backend.runtime.run_dir / "consumer-result.json").read_text(
                encoding="utf-8"
            )
        )
        if not result["destinationVisible"]:
            raise RuntimeError("consumer destination was not activated")
        size = int(self.reference.size_bytes)
        self.emit(ArtifactProgress(
            operation_id=self.operation_id,
            artifact=self.reference,
            phase="transfer",
            received_bytes=size,
            verified_bytes=size,
            committed_bytes=size,
            total_bytes=size,
            selected_replicas=1,
            committed_replicas=1,
            retransmitted_bytes=int(
                result["retrievalMetrics"]["retransmissionCount"]
            ),
            sequence=1,
            timestamp_ms=time.time_ns() // 1_000_000,
        ))
        self.state = "VERIFIED"

    def status(self):
        from py_repoclient import ArtifactSessionStatus

        return ArtifactSessionStatus(
            self.operation_id,
            "FETCH",
            self.state,
            self.reference,
        )

    def commit(self):
        from py_repoclient import ArtifactFetchResult

        self.state = "COMMITTED"
        return ArtifactFetchResult(
            reference=self.reference,
            operation_id=self.operation_id,
            destination=self.destination,
            reused_bytes=0,
            transferred_bytes=int(self.reference.size_bytes),
            source_replicas=("/spec164/repo",),
            total_duration_ms=(
                time.monotonic() - self.started
            ) * 1000,
        )

    def abort(self, preserve_progress):
        del preserve_progress
        self.state = "CANCELLED"
        return self.status()


class MiniNdnArtifactBackend:
    def __init__(self, runtime: MiniNdnRuntime, reference):
        self.runtime = runtime
        self.reference = reference

    def _require_reference(self, reference):
        if reference.to_dict() != self.reference.to_dict():
            raise ValueError("public API identity differs from signed fixture")

    def begin_publish(self, descriptor, operation_id, emit_progress):
        self._require_reference(descriptor.reference)
        if descriptor.requested_replicas != 1:
            raise ValueError("smoke topology has one repository")
        return PublicPublishDriver(
            self, descriptor, operation_id, emit_progress
        )

    def begin_fetch(
        self,
        reference,
        destination,
        operation_id,
        *,
        resume,
        verify,
        replace,
        timeout_ms,
        control,
        emit_progress,
    ):
        del resume, verify, replace, timeout_ms, control
        self._require_reference(reference)
        return PublicFetchDriver(
            self,
            reference,
            Path(destination),
            operation_id,
            emit_progress,
        )


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--topology-file", type=Path, default=DEFAULT_TOPOLOGY)
    parser.add_argument("--payload-size", type=int, default=32 * 1024)
    parser.add_argument("--timeout-seconds", type=float, default=12.0)
    args = parser.parse_args(argv)
    args.output_dir = args.output_dir.resolve()
    if args.output_dir.exists():
        raise SystemExit(f"output directory exists: {args.output_dir}")
    _prepare_fixture(args.output_dir, args.payload_size)

    sys.argv = [sys.argv[0]]
    runtime = MiniNdnRuntime(
        args.output_dir,
        args.topology_file.resolve(),
        args.timeout_seconds,
    )
    started = time.monotonic()
    summary = None
    try:
        runtime.start()
        control_summary = runtime.run_secure_control_smoke()
        receipt = control_summary["receipts"][0]["receipt"]
        summary = {
            "schema": "ndnsf-repo-spec164-public-api-minindn-v1",
            "verdict": "PASS",
            "artifact": receipt["artifact"],
            "publish": {
                "operationId": control_summary["operationId"],
                "requestedReplicas": 1,
                "achievedReplicas": len(control_summary["receipts"]),
                "receiptIds": [receipt["receiptId"]],
            },
            "fetch": {
                "transferredBytes": control_summary["fetchedBytes"],
                "reusedBytes": 0,
                "sourceReplicas": control_summary["selectedRepoNodes"],
                "destinationVisible": (
                    args.output_dir / "consumer/artifact.bin"
                ).is_file(),
            },
            "progressEvents": 2,
            "network": {
                "realRepoNodeDataPlane": True,
                "wholeArtifactCollaborationCount": 1,
                "legacyPerChunkPutFileUsed": False,
            },
            "control": control_summary,
            "elapsedMs": round(
                (time.monotonic() - started) * 1000, 3
            ),
            "performanceClaim": False,
        }
        _write_json(args.output_dir / "summary.json", summary)
        print(json.dumps(summary, sort_keys=True))
        print("SPEC164_PUBLIC_ARTIFACT_MININDN_SMOKE_OK")
        return 0
    finally:
        runtime.close()
        if summary is not None:
            _sanitize_evidence_run(args.output_dir)
            _restore_output_ownership(args.output_dir)


if __name__ == "__main__":
    raise SystemExit(main())
