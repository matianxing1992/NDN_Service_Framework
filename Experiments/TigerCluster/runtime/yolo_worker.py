"""Per-node YOLO role execution over the shared Tiger lifecycle primitives.

This module does not authorize a run or perform placement. The coordinator
must validate the frozen candidate and resolve application commands first.
The ACK-driven application remains responsible for the execution plan.
"""
from __future__ import annotations

import math
import json
import os
from pathlib import Path
import re
import signal
import stat
import threading
import time

from runtime.baseline import BIN, Processes, container_command, container_env, nfd_config
from runtime.identities import RoleHomeLease, validate_role_homes
from runtime.worker import run_finite_application


MODEL_ROLES = frozenset(("BackboneNeck", "DetectShard0", "DetectShard1"))
PROVIDER_ROLES = MODEL_ROLES | {"Merge"}


class StartupBarrier:
    """Bounded run-bound control records, never a data/activation transport.

    The operator creates an exclusive shared directory before launching ranks.
    Records are atomically published without overwrite; payloads are evidence
    references/results already validated by their stage owner, not authority
    to bypass SIF, model or credential verification.
    """
    STAGES = frozenset(('nfd-ready', 'routes-ready', 'network-ready', 'control-ready', 'providers-ready', 'workload-complete', 'failed'))

    def __init__(self, directory, *, run_id, probe_id, candidate_digest, ranks, rank, seconds, check):
        self.directory = _directory(Path(directory))
        if (not isinstance(run_id, str) or not re.fullmatch(r'[a-z][a-z0-9-]{1,47}', run_id)
                or not isinstance(probe_id, str) or not re.fullmatch(r'[a-f0-9]{32}', probe_id)
                or not isinstance(candidate_digest, str) or not re.fullmatch(r'sha256:[a-f0-9]{64}', candidate_digest)
                or not isinstance(ranks, tuple) or ranks not in ((0,), (0, 1))
                or any(type(r) is not int for r in ranks) or type(rank) is not int or rank not in ranks
                or isinstance(seconds, bool) or not isinstance(seconds, (int, float))
                or not math.isfinite(seconds) or seconds <= 0 or not callable(check)):
            raise ValueError('STARTUP_BARRIER_ARGUMENTS')
        self.binding = dict(schema='tiger-yolo-startup-v1', runId=run_id,
                            probeId=probe_id, candidateDigest=candidate_digest)
        self.ranks, self.rank, self.check = ranks, rank, check
        self.deadline = time.monotonic() + seconds

    def remaining(self):
        self.check()
        for rank in self.ranks:
            if self._read('failed', rank) is not None:
                raise RuntimeError('STARTUP_PEER_FAILED:' + str(rank))
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('STARTUP_DEADLINE')
        return remaining

    def _read(self, stage, rank):
        from runtime.yolo_profile import _read_plane
        _directory(self.directory)
        if stage not in self.STAGES or rank not in self.ranks:
            raise ValueError('STARTUP_STAGE_OR_RANK')
        path = self.directory / (stage + '-' + str(rank) + '.json')
        if path.is_symlink():
            raise ValueError('STARTUP_RECORD_SYMLINK')
        if not path.exists():
            return None
        record = _read_plane(path)
        expected = dict(self.binding, rank=rank, stage=stage)
        if (set(record) != set(expected) | {'payload'} or type(record.get('rank')) is not int
                or not isinstance(record['payload'], dict)
                or any(record.get(k) != v for k, v in expected.items())):
            raise ValueError('STARTUP_RECORD_BINDING')
        return record['payload']

    def publish(self, stage, payload):
        import tempfile
        _directory(self.directory)
        if stage not in self.STAGES or not isinstance(payload, dict):
            raise ValueError('STARTUP_STAGE_OR_PAYLOAD')
        if stage != 'failed':
            self.remaining()
        encoded = json.dumps(dict(self.binding, rank=self.rank, stage=stage, payload=payload),
                             allow_nan=False, sort_keys=True).encode()
        if len(encoded) > 64 * 1024:
            raise ValueError('STARTUP_RECORD_TOO_LARGE')
        # Readers see the complete fsynced record or no record at all.
        with tempfile.NamedTemporaryFile(dir=self.directory, prefix='.stage-', delete=False) as tmp:
            temporary = Path(tmp.name)
            try:
                tmp.write(encoded)
                tmp.flush()
                os.fsync(tmp.fileno())
                os.link(temporary, self.directory / (stage + '-' + str(self.rank) + '.json'))
            finally:
                temporary.unlink()

    def wait(self, stage, ranks=None):
        ranks = self.ranks if ranks is None else ranks
        if not ranks or any(rank not in self.ranks for rank in ranks):
            raise ValueError('STARTUP_WAIT_RANKS')
        while True:
            remaining = self.remaining()
            values = {rank: self._read(stage, rank) for rank in ranks}
            if all(value is not None for value in values.values()):
                return values
            time.sleep(min(0.1, remaining))


def _directory(value, *, may_create=False):
    if not isinstance(value, (str, Path)):
        raise ValueError("WORKER_DIRECTORY")
    path = Path(value)
    if (not path.is_absolute() or ".." in path.parts
            or any(c in str(path) for c in ":,\x00\n\r")
            or any(p.is_symlink() for p in (path, *path.parents))
            or (not path.is_dir() and (not may_create or path.exists()))):
        raise ValueError("WORKER_DIRECTORY")
    return path


def assigned_roles(mode: str, rank: int) -> tuple[str, ...]:
    """Physical startup layout, never a replacement for runtime Selection."""
    if type(rank) is not int:
        raise ValueError("WORKER_RANK")
    primary = ("nfd0", "controller", "repo", "user", "BackboneNeck", "Merge")
    heads = ("DetectShard0", "DetectShard1")
    if mode in ("local-cpu", "single-node-gpu") and rank == 0:
        return primary + heads
    if mode in ("two-node-gpu", "negative-dependency") and rank in (0, 1):
        return primary if rank == 0 else ("nfd1", *heads)
    raise ValueError("WORKER_MODE_OR_RANK")


class NodeRuntime:
    """Own this node's role processes, role-scoped mounts and cleanup evidence."""

    @classmethod
    def from_preparation(cls, plan: dict, *, expected_receipt_digest: str,
                         candidate_digest: str, **kwargs):
        """Production construction boundary; SIF/gate qualification is external.

        The direct constructor remains a low-level lifecycle component used by
        isolated process tests, not an authorized experiment entrypoint.
        """
        from runtime.yolo_bundle import verify_preparation
        # Own an immutable snapshot, so caller mutation cannot change the pin.
        plan = json.loads(json.dumps(plan, allow_nan=False))
        rank, mode = kwargs['rank'], kwargs['mode']
        if (plan.get('case') != mode or type(rank) is not int
                or Path(kwargs['output']) != Path(plan['output']) / ('node' + str(rank))):
            raise ValueError('WORKER_PREPARATION_RUN')
        matching = [node for node in plan.get('nodes', []) if node.get('rank') == rank]
        if len(matching) != 1 or set(matching[0].get('roles', [])) != set(assigned_roles(mode, rank)):
            raise ValueError('WORKER_PREPARATION_ROLES')
        verify_preparation(kwargs['public'], plan, expected_receipt_digest=expected_receipt_digest,
                           candidate_digest=candidate_digest)
        worker = cls(**kwargs)
        worker._preparation_binding = (plan, expected_receipt_digest, candidate_digest)
        return worker

    def _verify_prepared_boundary(self):
        if self._preparation_binding is not None:
            from runtime.yolo_bundle import verify_preparation
            plan, receipt, candidate = self._preparation_binding
            verify_preparation(self.public, plan, expected_receipt_digest=receipt,
                               candidate_digest=candidate)

    def __init__(self, *, profile: dict, mode: str, rank: int, bundle: Path,
                 homes: dict[str, Path], public: Path, output: Path, node: Path,
                 gpu_device: str | None,
                 cleanup_seconds: float):
        self.roles = assigned_roles(mode, rank)
        self._preparation_binding = None
        if set(homes) != set(self.roles):
            raise ValueError("WORKER_ROLE_HOMES")
        if (mode == "local-cpu") != (gpu_device is None):
            raise ValueError("WORKER_GPU_MODE")
        self.homes = validate_role_homes(homes)
        self.profile, self.mode, self.rank = dict(profile), mode, rank
        self.bundle, self.public = _directory(bundle), _directory(public)
        self.output, self.node = _directory(output, may_create=True), _directory(node)
        # Native YOLO obtains encrypted canonical artifacts over NDN and
        # assembles them in its own /output cache. Do not mount a source
        # package (especially an oracle) into the Provider as a shortcut.
        for source in (self.bundle, self.public, *self.homes.values()):
            if self.output == source or self.output in source.parents or source in self.output.parents:
                raise ValueError("WORKER_OUTPUT_OVERLAP")
        self.gpu_device, self.cleanup_seconds = gpu_device, cleanup_seconds
        self.children = Processes(self.output / "logs")
        self.children.close(seconds=cleanup_seconds)  # Validate before any spawn.
        self.launches = []
        self.started = set()
        self.leases = {}
        self.closed = False
        self.cleanup_records = {}
        self.finite_children = Processes(self.output / "logs")
        self.invocations = set()

    def run_user(self, invocation: str, argv: list[str], *, package: Path | None,
                 seconds: float, peer_failure: Path | None = None):
        """One finite User, retaining persistent Providers and User state.

        Invocation output directories are exclusive; a failed or completed
        invocation cannot be overwritten/retried under the same name.
        This is process completion only, never an inference verdict.
        """
        return self._run_finite_role('user', invocation, argv, package=package,
                                     seconds=seconds, peer_failure=peer_failure)

    def run_network_probe(self, argv: list[str], *, seconds: float,
                          peer_failure: Path | None = None):
        """Borrow one not-yet-started Provider HOME for a finite CPU-only probe."""
        if self.mode not in ('two-node-gpu', 'negative-dependency') or self._preparation_binding is None:
            raise ValueError('WORKER_NETWORK_PROBE_SCOPE')
        role = 'BackboneNeck' if self.rank == 0 else 'DetectShard0'
        return self._run_finite_role(role, 'network-readiness', argv, package=None,
                                     seconds=seconds, peer_failure=peer_failure)

    def run_management(self, invocation: str, arguments: list[str], *, seconds: float,
                       peer_failure: Path | None = None):
        """Execute exact-SIF nfdc with a borrowed, otherwise idle Provider HOME.

        NFD keeps its own HOME; no second process opens that live role's PIB.
        Management commands neither mount models nor enable CUDA.
        """
        if self._preparation_binding is None or 'nfd' + str(self.rank) not in self.started:
            raise ValueError('WORKER_MANAGEMENT_SCOPE')
        if not isinstance(invocation, str) or not invocation.startswith('nfd-'):
            raise ValueError('WORKER_MANAGEMENT_INVOCATION')
        role = 'BackboneNeck' if self.rank == 0 else 'DetectShard0'
        return self._run_finite_role(role, invocation, [BIN + '/nfdc', *arguments], package=None,
                                     seconds=seconds, peer_failure=peer_failure)

    def _run_finite_role(self, role, invocation, argv, *, package, seconds, peer_failure):
        if self.closed or role not in self.roles or role in self.started:
            raise ValueError('WORKER_USER_ROLE')
        self._verify_prepared_boundary()
        if not isinstance(invocation, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', invocation):
            raise ValueError('WORKER_INVOCATION')
        if invocation in self.invocations:
            raise ValueError('WORKER_INVOCATION_REUSED')
        if (isinstance(seconds, bool) or not isinstance(seconds, (int, float))
                or not math.isfinite(seconds) or seconds <= 0):
            raise ValueError('WORKER_USER_BUDGET')
        if not argv or not all(isinstance(arg, str) and '\x00' not in arg for arg in argv):
            raise ValueError('WORKER_ARGV')
        package = _directory(package) if package is not None else None
        role_output = _directory(self.output / role, may_create=True)
        if package is not None and (package == role_output or package in role_output.parents
                                    or role_output in package.parents):
            raise ValueError('WORKER_OUTPUT_OVERLAP')

        def check():
            self.check()
            if peer_failure is not None and peer_failure.exists():
                raise RuntimeError('PEER_FAILED')

        check()
        lease = RoleHomeLease(self.homes[role])
        tag, rows = role + '-' + invocation, []
        record = {'role': role, 'invocation': invocation, 'pid': None}
        try:
            for name in ('state', 'requests'):
                _directory(role_output / name, may_create=True).mkdir(parents=True, exist_ok=True, mode=0o700)
            (role_output / 'requests' / invocation).mkdir(mode=0o700)
            self.invocations.add(invocation)
            command = container_command(
                self.profile, self.bundle, self.homes[role], self.public,
                role_output, ['/usr/bin/env', 'NDNSF_DI_STATE_ROOT=/output/state', *argv],
                node=self.node, artifacts=package)
            record['argv'] = command
            self.launches.append(record)
            rc = run_finite_application(tag, command, self.output / 'logs' / (tag + '.log'),
                rows, seconds=seconds, env=container_env(), cwd=self.bundle,
                cleanup_seconds=self.cleanup_seconds, check=check, owner=self.finite_children)
            if any(row['forced'] or not row['reaped'] or row.get('cleanupError') for row in rows):
                raise RuntimeError('WORKER_USER_CLEANUP')
            return rc
        finally:
            if rows:
                record['pid'] = rows[-1]['pid']
                self.cleanup_records[tag] = dict(rows[-1])
                self.leases[tag] = (lease, rows[-1]['pid'])
                try:
                    os.killpg(rows[-1]['pid'], 0)
                except ProcessLookupError:
                    lease.close()
                    del self.leases[tag]
                except OSError as exc:
                    # Unknown group state must retain the HOME owner and
                    # cleanup error, not mask an earlier application failure.
                    self.cleanup_records[tag]['leaseError'] = type(exc).__name__
            elif self.finite_children.children:
                # Preserve ownership if interrupted while recording cleanup.
                self.leases[tag] = (lease, self.finite_children.children[-1][1].pid)
            else:
                lease.close()

    def start_service(self, role: str, argv: list[str]):
        return self._start_service(role, argv)

    def start_provider(self, role: str, *, identity: str, service: str,
                       group: str, controller: str, permission_wait_ms: int):
        """Launch the installed native YOLO handler, not a synthetic handler.

        The coordinator supplies validated policy identities and generates the
        three public configuration files. Cryptographic/hash validation and
        permission readiness remain mandatory coordinator gates; existence
        here is only a final mount/path check. Offer keys stay in the role HOME.
        """
        if role not in PROVIDER_ROLES or role not in self.roles:
            raise ValueError("WORKER_PROVIDER_ROLE")
        for name in (identity, service, group, controller):
            if (not isinstance(name, str) or not name.startswith('/') or name == '/'
                    or any(ord(c) < 33 or ord(c) == 127 for c in name)):
                raise ValueError("WORKER_PROVIDER_NAME")
        if type(permission_wait_ms) is not int or not 1 <= permission_wait_ms <= 120000:
            raise ValueError("WORKER_PERMISSION_BUDGET")
        files = [self.public / name for name in
                 ('native-execution-plan.json', 'service-manifest.json', 'trust-schema.conf',
                  'contracts/trust-root-registry-v1.json', 'contracts/authority.pub')]
        files.extend(self.homes[role] / name for name in
                     ('offer.pem', 'recipient.pem', 'recipient-map.json'))
        for path in files:
            if any(p.is_symlink() for p in (path, *path.parents)) or not path.is_file():
                raise ValueError("WORKER_PROVIDER_INPUT:" + path.name)
        # NativeProtectedGrantCredentials reads an identity -> private PEM map.
        # Each Provider sees only its own map/key; User receives public keys.
        home_path = '/identities/' + self.homes[role].name
        mapping = self.homes[role] / 'recipient-map.json'
        if not 0 < mapping.stat().st_size <= 4096:
            raise ValueError('WORKER_RECIPIENT_MAP_SIZE')
        with mapping.open('rb') as stream:
            wire = stream.read(4097)
        if len(wire) > 4096:
            raise ValueError('WORKER_RECIPIENT_MAP_SIZE')
        try:
            pairs = json.loads(wire, object_pairs_hook=list)
        except (ValueError, UnicodeError) as exc:
            raise ValueError('WORKER_RECIPIENT_MAP') from exc
        if pairs != [(identity, home_path + '/recipient.pem')]:
            raise ValueError('WORKER_RECIPIENT_MAP')
        key = self.homes[role] / 'recipient.pem'
        if stat.S_IMODE(key.stat().st_mode) != 0o600 or not 0 < key.stat().st_size <= 65536:
            raise ValueError('WORKER_RECIPIENT_KEY')
        gpu = self.mode != 'local-cpu' and role in MODEL_ROLES
        argv = ['/usr/bin/env',
                'SPEC181_GRANT_AUTHORITY_PUBLIC_KEY=/config/contracts/authority.pub',
                'SPEC181_PROVIDER_RECIPIENT_KEY_MAP=' + home_path + '/recipient-map.json',
                BIN + '/di-native-provider', '--serve',
                '--plan', '/config/native-execution-plan.json',
                '--manifest', '/config/service-manifest.json',
                '--trust-schema', '/config/trust-schema.conf',
                '--provider', identity, '--service', service,
                '--group', group, '--controller', controller, '--roles', role,
                '--workers', '1', '--handler-threads', '1', '--ack-threads', '1',
                '--artifact-cache-dir', '/output/artifact-cache',
                '--selection-offer-key-file', '/identities/' + self.homes[role].name + '/offer.pem',
                '--offer-backend', 'onnxruntime-cuda' if gpu else 'onnxruntime-cpu',
                '--offer-device', 'cuda:0' if gpu else 'cpu',
                '--offer-can-provision', '--permission-wait-ms', str(permission_wait_ms)]
        return self._start_service(role, argv)

    def start_forwarder(self, port: int):
        config = nfd_config(port)
        role = "nfd" + str(self.rank)
        return self._start_service(role, [BIN + "/nfd", "--config", "/output/nfd.conf"],
                                   initial_files=(("nfd.conf", config),))

    def _start_service(self, role, argv, initial_files=()):
        if self.closed:
            raise ValueError("WORKER_CLOSED")
        self._verify_prepared_boundary()
        if role not in self.roles or role == "user":
            raise ValueError("WORKER_SERVICE_ROLE")
        if role in self.started:
            raise ValueError("WORKER_ROLE_ALREADY_STARTED")
        if not argv or not all(isinstance(arg, str) and "\x00" not in arg for arg in argv):
            raise ValueError("WORKER_ARGV")
        role_output = _directory(self.output / role, may_create=True)
        gpu = self.mode != "local-cpu" and role in MODEL_ROLES
        application = ["/usr/bin/env", "NDNSF_DI_STATE_ROOT=/output/state",
                       "NDNSF_DI_DEPENDENCY_OBJECT_TRACE=1",
                       "NDNSF_DI_ORT_PROFILE_PREFIX=/output/ort/session", *argv]
        command = container_command(
            self.profile, self.bundle, self.homes[role], self.public, role_output,
            application, node=self.node,
            gpu=gpu, gpu_device=self.gpu_device if gpu else None)
        lease = RoleHomeLease(self.homes[role])
        try:
            for name in ("state", "ort"):
                _directory(role_output / name, may_create=True).mkdir(parents=True, exist_ok=True, mode=0o700)
            for name, content in initial_files:
                fd = os.open(str(role_output / name), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                with os.fdopen(fd, "w") as stream:
                    stream.write(content)
            record = {"role": role, "rank": self.rank, "argv": command, "pid": None}
            self.launches.append(record)
            child = self.children.start(role, command, container_env(), cwd=self.bundle)
        except BaseException as exc:
            if self.launches and self.launches[-1]["role"] == role:
                self.launches[-1]["startError"] = type(exc).__name__
            lease.close()
            raise
        record["pid"] = child.pid
        self.started.add(role)
        self.leases[role] = (lease, child.pid)
        return child

    def check(self):
        self.children.check()

    def wait_marker(self, role: str, marker: str, *, seconds: float,
                    peer_failure: Path | None = None):
        """Bounded stdout observation; a marker alone is not runtime readiness.

        The coordinator also verifies Controller/Provider permissions and
        signed publication receipts before requests. Read incrementally so a
        chatty process does not turn readiness into repeated full-log scans.
        """
        if (role not in self.started or not isinstance(marker, str) or not marker
                or len(marker.encode()) > 4096):
            raise ValueError("WORKER_MARKER")
        if (isinstance(seconds, bool) or not isinstance(seconds, (int, float))
                or not math.isfinite(seconds) or seconds <= 0):
            raise ValueError("WORKER_WAIT_BUDGET")
        deadline = time.monotonic() + seconds
        needle, tail = marker.encode(), b""
        fd = os.open(str(self.children.log_dir / (role + ".log")),
                     os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        try:
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                raise ValueError("WORKER_LOG_TYPE")
            while time.monotonic() < deadline:
                self.check()
                if peer_failure is not None and peer_failure.exists():
                    raise RuntimeError("PEER_FAILED")
                chunk = os.read(fd, 65536)
                if needle in tail + chunk:
                    self.check()
                    return
                tail = (tail + chunk)[-len(needle):]
                if not chunk:
                    time.sleep(min(0.05, max(0, deadline - time.monotonic())))
            raise TimeoutError("WORKER_MARKER_TIMEOUT:" + role)
        finally:
            os.close(fd)

    def close(self):
        # The Slurm worker runs on the main thread. Preserve a first failure
        # and complete its bounded teardown despite another TERM/INT arriving.
        handlers = ({sig: signal.signal(sig, signal.SIG_IGN)
                     for sig in (signal.SIGINT, signal.SIGTERM)}
                    if threading.current_thread() is threading.main_thread() else {})
        try:
            return self._close()
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)

    def _close(self):
        self.closed = True
        deadline = time.monotonic() + self.cleanup_seconds
        rows = self.children.close(seconds=self.cleanup_seconds)
        if self.finite_children.children:
            finite_rows = self.finite_children.close(seconds=max(0.001, deadline - time.monotonic()))
            for row in finite_rows:
                row['kind'] = 'finite'
            rows += finite_rows
        for row in rows:
            self.cleanup_records[row["name"]] = dict(row)
        current = {row["name"] for row in rows}
        for role, record in self.cleanup_records.items():
            if role not in current:
                rows.append(dict(record))
        # Reaping a leader does not prove that its group no longer exists.
        # Keep the HOME lease while a descendant or unreaped group survives.
        lease_errors = {}
        for role, (lease, pid) in list(self.leases.items()):
            try:
                os.killpg(pid, 0)
            except ProcessLookupError:
                lease.close()
                del self.leases[role]
            except OSError as exc:
                lease_errors[role] = type(exc).__name__
        for row in rows:
            row["leaseReleased"] = row["name"] not in self.leases
            if row["name"] in lease_errors:
                row["leaseError"] = lease_errors[row["name"]]
        return rows
