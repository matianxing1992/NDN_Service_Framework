#!/usr/bin/env python3
"""Prepare and run the Spec191 five-node UAV tracking demonstration.

The command is intentionally offline by default. ``--prepare`` creates only a
small manifest and process plan; it never downloads videos/models or writes
private keys. ``--run`` requires a fully built native application and root
MiniNDN privileges, then runs the exact manifest through MiniNDN/NFD. It does
not fall back to a host-process launch because that cannot prove network
faces, routes, or node isolation.
"""

from __future__ import annotations

import argparse
import base64
from dataclasses import dataclass
import copy
from datetime import datetime, timezone
import fcntl
from contextlib import ExitStack, closing, contextmanager, nullcontext
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import signal
import shlex
import shutil
import stat
import sqlite3
import subprocess
import sys
import time
from typing import TYPE_CHECKING, Any, Iterable, Sequence
from types import MappingProxyType

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SOURCE = Path(os.environ.get(
    "NDNSF_SPEC191_ASSET_ROOT", str(Path.home() / ".cache/ndnsf/spec191-assets")))
DEFAULT_MODEL = DEFAULT_SOURCE / "3UAVs.pt"
DEFAULT_OUTPUT = ROOT / "results/uav-tracking/spec191-local"
DEFAULT_OBSERVATION_PROFILE = ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json"
DEFAULT_TRUST_SCHEMA = ROOT / "examples/trust-schema.conf"
TOPOLOGY = Path(__file__).with_name("topology.conf")
MAX_REPLAY_WINDOWS = 600
NFD_CS_MAX_PACKETS = 4096
DEFAULT_SAMPLE_RATE_HZ = 5.0
DEFAULT_FRAME_COUNT = 100
MAX_UNIX_SOCKET_PATH_BYTES = 107
REQUIRED_NATIVE_PROGRAMS = (
    "UavTrackingNode",
    "App_ServiceController",
    "UavTrackingGroundStation",
)
DEFAULT_TRACKING_LIBEXEC = Path(
    os.environ.get("NDNSF_UAV_TRACKING_LIBEXEC",
                   "/usr/local/libexec/ndnsf-uav/tracking"))

sys.path.insert(0, str(Path(__file__).parent))
from tracking_artifacts import (ArtifactError, digest_file, load_person_source_manifest,
                                validate_live_source_model,
                                validate_real_assets,
                                validate_video_reference_assets)  # noqa: E402
from tracking_topology import TopologyError, load_topology  # noqa: E402
from motion_fixture import MOTION_PROFILES, write_fixture  # noqa: E402
from observation_source import (load_demo_profile, load_observation_profile, parse_observation_profile,
                                load_source_generation_manifest,
                                source_generation_for_window,
                                write_profile_frame)  # noqa: E402


if TYPE_CHECKING:
    from person_profile import PersonProfile


class RunnerError(RuntimeError):
    """Raised when setup or lifecycle preconditions fail."""


@dataclass(frozen=True)
class ProcessSpec:
    name: str
    node: str
    identity: str
    cwd: Path
    command: tuple[str, ...]
    private_dir: Path

    def public_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "node": self.node,
            "identity": self.identity,
            "cwd": str(self.cwd),
            "command": list(self.command),
            # This is a directory contract, not a key or credential.
            "privateDir": str(self.private_dir),
        }


# The maintained demo's actor configuration is shared by both application modes.
TRACKING_ACTORS = MappingProxyType({
    'controller': ('gs', '/example/uav/controller', 'controller'),
    'ground-station': ('gs', '/example/uav/gs', 'ground-station'),
    'uav1-camera': ('uav1', '/example/uav/drone/UAV1', 'uav1'),
    'uav2-camera': ('uav2', '/example/uav/drone/UAV2', 'uav2'),
    'uav3-camera': ('uav3', '/example/uav/drone/UAV3', 'uav3'),
    'compute': ('compute', '/example/uav/compute', 'compute'),
})


def _actor_spec(name: str, root: Path, output: Path, command: tuple[str, ...]) -> ProcessSpec:
    node, identity, private = TRACKING_ACTORS[name]
    return ProcessSpec(name, node, identity, root, command, output / 'private' / private)


def _required_native_programs(application_mode: str) -> tuple[str, ...]:
    if application_mode == 'main':
        return ('UavDroneApp', 'App_ServiceController', 'UavGroundStationApp')
    if application_mode != 'compat':
        raise RunnerError('application mode must be main or compat')
    return REQUIRED_NATIVE_PROGRAMS


def _main_asset(path: Path, *, interpreter: bool = False) -> dict[str, str]:
    path = path.expanduser().absolute() if interpreter else path.expanduser().resolve(strict=True)
    if not path.is_file() or path.is_symlink():
        raise RunnerError(f'main assets require a regular file: {path}')
    return {'path': str(path), 'digest': digest_file(path)}


def _write_main_config(path: Path, value: dict[str, Any]) -> None:
    payload = (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()
    if len(payload) > 256 * 1024:
        raise RunnerError('native main configuration exceeds 256 KiB')
    if path.is_symlink() or any(parent.is_symlink() for parent in path.parents):
        raise RunnerError(f'main configuration cannot use a symlink: {path}')
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open('xb') as stream:
            stream.write(payload)
    except FileExistsError:
        if not path.is_file() or path.read_bytes() != payload:
            raise RunnerError(f'prepared main configuration is immutable; use a fresh run: {path}')



def _main_process_specs(root: Path, output: Path, *, model: Path, headless: bool,
                        window_count: int, request_timeout_ms: int,
                        source_ids: dict[str, str], source_epoch: int,
                        observation_profile: dict[str, Any],
                        source_generations_path: Path, trust_schema: Path,
                        person_tracking: dict[str, Any] | None,
                        worker_python: Path) -> list[ProcessSpec]:
    output = output.resolve()
    if (output / 'lifecycle.json').exists() or (output / 'main-network-attempt.json').exists():
        raise RunnerError('main run already attempted network execution; use a fresh run')
    if (not re.fullmatch(r'[A-Za-z0-9_.-]{1,119}', output.name) or output.name in {'.', '..'}):
        raise RunnerError('main run id must fit its native tracking- wire mission binding')
    if observation_profile.get('deliveryMode') != 'replay':
        raise RunnerError('main application requires prepared replay streams')
    _write_main_config(source_generations_path,
        _source_generations(window_count, source_ids, source_epoch, ()))
    generations = _main_asset(source_generations_path)
    source_names = [f'uav{i}-camera' for i in range(1, 4)]
    producers = [TRACKING_ACTORS[name][1] for name in source_names]
    compute = TRACKING_ACTORS['compute'][1]
    controller = TRACKING_ACTORS['controller'][1]
    user = TRACKING_ACTORS['ground-station'][1]
    group = controller.rsplit('/', 1)[0]
    mode = 'person' if person_tracking is not None else 'vehicle'
    config = output / 'private/config'
    job_output = output / 'tracking'
    profile = _main_asset(Path(person_tracking['profile'])) if person_tracking else None
    computation = _main_asset(Path(person_tracking['computationManifest'])) if person_tracking else None
    job = {'schema': 'uav-tracking-job-v1', 'jobId': output.name, 'runId': output.name,
           'missionId': output.name, 'mode': mode, 'sources': [],
           'compute': {'producerIdentity': compute, 'droneId': compute.rsplit('/', 1)[1]},
           'calibrationIdentity': None, 'profile': profile, 'computationManifest': computation,
           'outputDirectory': str(job_output), 'windowCount': window_count,
           'deadlineMs': request_timeout_ms, 'requestTimeoutMs': request_timeout_ms}
    for index, identity in enumerate(producers, 1):
        camera = f'UAV{index}'
        job['sources'].append({'cameraId': camera, 'sourceId': source_ids[camera],
            'sourceEpoch': source_epoch, 'producerIdentity': identity,
            'droneId': identity.rsplit('/', 1)[1], 'sourceManifest': generations})
    job_path = config / 'main-tracking-job.json'
    _write_main_config(job_path, job)
    main_node = _installed_program('UavDroneApp')
    main_gs = _installed_program('UavGroundStationApp')
    controller_binary = _installed_program('App_ServiceController')
    policy = root / 'Experiments/UAV/spec228.policies'
    if not policy.is_file():
        raise RunnerError('maintained main tracking policy is unavailable')
    specs = [_actor_spec('controller', root, output,
        (controller_binary, '--node', 'gs', '--controller-prefix', controller,
         '--group-prefix', group, '--policy-file', str(policy), '--trust-schema', str(trust_schema),
         '--no-demo-identities', '--ensure-identities', ','.join([*producers, compute, user])))]
    gs_args = (main_gs, '--group-prefix', group, '--controller-prefix', controller,
        '--ground-station-identity', user, '--trust-schema', str(trust_schema),
        '--no-cert-dialog', '--target-drone', job['sources'][0]['droneId'],
        '--patrol-drones', ','.join(source['droneId'] for source in job['sources']),
        '--operator-id', user, '--operator-lease-drone', 'all', '--operator-lease-scope', 'control',
        '--tracking-job-config', str(job_path), '--tracking-auto-start')
    specs.append(_actor_spec('ground-station', root, output,
                            (*gs_args, '--headless') if headless else gs_args))
    for name in [*source_names, 'compute']:
        identity = TRACKING_ACTORS[name][1]
        is_compute = name == 'compute'
        camera = '' if is_compute else f'UAV{source_names.index(name) + 1}'
        run = {'runId': output.name, 'mode': mode, 'role': 'compute' if is_compute else 'source',
            'cameraId': camera, 'sourceProducers': producers, 'computeProducer': compute,
            'windowCount': window_count, 'requestTimeoutMs': request_timeout_ms,
            'sourceGenerations': generations,
            'observationStreamDirectory': '' if is_compute else str(output / 'private/observations' / camera),
            'worldFrameId': observation_profile['worldFrameId'],
            'acquisitionKind': observation_profile['acquisitionKind'],
            'deliveryMode': 'replay',
            'model': _main_asset(model) if is_compute else None,
            'profile': profile if is_compute else None,
            'reidModel': _main_asset(Path(person_tracking['reidModel'])) if is_compute and person_tracking else None,
            'computationManifest': computation if is_compute else None,
            'workerPython': _main_asset(Path(person_tracking['workerPython']) if person_tracking else worker_python,
                                       interpreter=True) if is_compute else None,
            'workerScript': _main_asset(_installed_tracking_script('tracking_worker.py')) if is_compute else None}
        provider_path = config / f'main-{name}.json'
        _, _, private = TRACKING_ACTORS[name]
        catalog = {'schema': 'uav-tracking-provider-v1', 'producerIdentity': identity,
            'droneId': identity.rsplit('/', 1)[1],
            'ownedWorkspace': str(output / 'private' / private / 'tracking'), 'runs': [run]}
        _write_main_config(provider_path, catalog)
        specs.append(_actor_spec(name, root, output,
            (main_node, '--headless', '--group-prefix', group, '--controller-prefix', controller,
             '--drone-prefix', identity.rsplit('/', 1)[0], '--drone-id', catalog['droneId'],
             '--trust-schema', str(trust_schema), '--flight-controller-backend', 'mock',
             '--tracking-provider-config', str(provider_path))))
    return specs


def _main_plan_metadata(specs: Sequence[ProcessSpec], output: Path) -> dict[str, Any]:
    by_name = {spec.name: spec for spec in specs}
    gs = by_name['ground-station']
    job_path = Path(gs.command[gs.command.index('--tracking-job-config') + 1])
    job = json.loads(job_path.read_bytes())
    providers = {}
    files = {job_path, TOPOLOGY, Path(__file__).resolve(), ROOT / 'Experiments/UAV/spec228.policies',
             Path(__file__).with_name('person_run_receipt.py'),
             Path(__file__).with_name('person_run_resources.py')}
    for name in ('uav1-camera', 'uav2-camera', 'uav3-camera', 'compute'):
        spec = by_name[name]
        path = Path(spec.command[spec.command.index('--tracking-provider-config') + 1])
        providers[name] = _main_asset(path)
        files.add(path)
        value = json.loads(path.read_bytes())
        for run in value['runs']:
            for key in ('sourceGenerations', 'model', 'profile', 'reidModel', 'computationManifest',
                        'workerPython', 'workerScript'):
                if run[key] is not None:
                    files.add(Path(run[key]['path']))
    files.update(path for path in DEFAULT_TRACKING_LIBEXEC.glob('*.py') if path.is_file())
    files.update(path for path in DEFAULT_TRACKING_LIBEXEC.glob('*.lock') if path.is_file())
    for run in json.loads(Path(providers['compute']['path']).read_bytes())['runs']:
        environment = Path(run['workerPython']['path']).parent.parent
        files.update(path for path in (environment / 'pyvenv.cfg', environment / 'spec228-environment.json') if path.is_file())
        files.update(path for path in environment.glob('lib/python*/site-packages/*.pth') if path.is_file())
    for spec in specs:
        files.add(Path(spec.command[0]).resolve(strict=True))
    return {'applicationMode': 'main', 'trackingMain': {
        'schema': 'uav-main-tracking-plan-v1', 'jobs': [_main_asset(job_path)],
        'providerConfigs': providers, 'statusFile': str(Path(job['outputDirectory']) / 'tracking-status.json'),
        'computeWorkspace': json.loads(Path(providers['compute']['path']).read_bytes())['ownedWorkspace'],
        'frozenFiles': [_main_asset(path) for path in sorted(files)],
        'sourceFiles': [], 'headless': '--headless' in gs.command}}


def _sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _source_generation_ready_marker(observation: dict[str, Any]) -> str | None:
    generations = observation.get("sourceGenerations", {}).get("generations", [])
    if len(generations) <= 1:
        return None
    digest = observation.get("sourceGenerationManifestSha256")
    if not isinstance(digest, str) or not digest.startswith("sha256:"):
        raise RunnerError("source generation cutover has no canonical manifest digest")
    return f"generation-manifest-sha256={digest}"


def _has_all_markers_on_one_line(text: str, markers: tuple[str, ...]) -> bool:
    return any(all(marker in line for marker in markers) for line in text.splitlines())


def _uav_socket_root(output: Path) -> Path:
    run_digest = hashlib.sha256(os.fsencode(output.resolve())).hexdigest()
    return Path("/tmp") / f"ndnsf-uav-{run_digest}"


def _uav_unix_socket_path(socket_root: Path, socket_name: str) -> Path:
    socket = socket_root / f"{socket_name}.sock"
    path_bytes = os.fsencode(socket)
    if len(path_bytes) > MAX_UNIX_SOCKET_PATH_BYTES:
        raise RunnerError(
            f"NFD Unix socket path exceeds {MAX_UNIX_SOCKET_PATH_BYTES}-byte limit: "
            f"{len(path_bytes)} bytes")
    return socket


def _effective_paths(source: Path, model: Path, output: Path) -> tuple[Path, Path, Path]:
    source = source.expanduser().resolve()
    model = model.expanduser().resolve()
    output = output.expanduser().resolve()
    if not source.is_dir():
        raise RunnerError(f"source directory is not available: {source}")
    if not model.is_file():
        raise RunnerError(f"model checkpoint is not available: {model}")
    output.mkdir(parents=True, exist_ok=True)
    return source, model, output


def _installed_program(name: str) -> str:
    """Resolve a program from the installed PATH, never from the checkout."""
    return shutil.which(name) or name


def _installed_tracking_script(name: str) -> Path:
    return DEFAULT_TRACKING_LIBEXEC / name


def _observation_source_ids(output: Path, profile: dict[str, Any]) -> dict[str, str]:
    run_key = re.sub(r"[^A-Za-z0-9._-]+", "-", output.name).strip("-.") or "run"
    prefix = str(profile.get("sourceIdPrefix", "uav-demo"))
    return {
        camera: f"{prefix}-{run_key}-{camera}"[:128]
        for camera in ("UAV1", "UAV2", "UAV3")
    }


def _source_generations(window_count: int, source_ids: dict[str, str], source_epoch: int,
                        switch_after_windows: Sequence[int]) -> dict[str, Any]:
    if not 1 <= window_count <= MAX_REPLAY_WINDOWS or source_epoch < 1:
        raise RunnerError("source generation window count or epoch is invalid")
    if any(type(value) is not int or not 1 <= value < window_count
           for value in switch_after_windows):
        raise RunnerError("source switches must occur after a completed window before the final window")
    if any(right <= left for left, right in zip(switch_after_windows,
                                               switch_after_windows[1:])):
        raise RunnerError("source switch windows must be supplied in strictly increasing order")
    if set(source_ids) != {"UAV1", "UAV2", "UAV3"} or len(set(source_ids.values())) != 3:
        raise RunnerError("initial source IDs must uniquely cover UAV1, UAV2, and UAV3")
    generations: list[dict[str, Any]] = [{
        "firstWindowIndex": 0,
        "sourceEpoch": source_epoch,
        "sourceIds": dict(source_ids),
    }]
    for generation_index, first_window_index in enumerate(switch_after_windows, start=2):
        suffix = f"-g{generation_index}"
        next_ids = {
            camera: source_ids[camera][:128 - len(suffix)] + suffix
            for camera in ("UAV1", "UAV2", "UAV3")
        }
        if len(set(next_ids.values())) != 3 or any(
                next_ids[camera] == generations[-1]["sourceIds"][camera]
                for camera in next_ids):
            raise RunnerError("source ID prefix cannot produce unique generation identities")
        generations.append({
            "firstWindowIndex": first_window_index,
            "sourceEpoch": source_epoch + generation_index - 1,
            "sourceIds": next_ids,
        })
    return {
        "schema": "uav-source-generations-v1",
        "windowCount": window_count,
        "generations": generations,
    }


REFERENCE_IDENTITY_CONTENT_FAULTS = frozenset(
    f"bad-reference-{kind}-{category}"
    for kind in ("content", "producer")
    for category in ("clock", "calibration", "ground", "pose")
) | frozenset({"bad-reference-epoch-pose"}) | frozenset(
    f"bad-reference-revision-{category}" for category in ("clock", "calibration", "ground")
)

PERSON_BINDING_FAULTS = frozenset({
    "wrong-person-profile-binding", "wrong-person-computation-binding",
})
PERSON_SOURCE_EPOCH_RESTART_FAULT = "person-source-epoch-restart-after-window-0"

FAULTS = frozenset({
    *PERSON_BINDING_FAULTS,
    *REFERENCE_IDENTITY_CONTENT_FAULTS,
    "none", "missing-view", "bad-content", "bad-identity", "bad-reference-digest",
    "bad-reference-digest-clock", "bad-reference-digest-calibration",
    "bad-reference-digest-ground", "bad-reference-digest-pose",
    "missing-reference-clock",
    "duplicate-reference-pose", "overlimit-reference-pose",
    "stale-epoch", "worker-failure", "response-loss", "late-response-loss", "cancel",
    "cancel-during-active-fetch", "cancel-during-active-reference-fetch",
    "user-cancel-during-person-worker",
    "user-cancel-during-active-reference-fetch", "recover-active-reference-fetch",
    "user-cancel-after-source-switch-during-active-reference-fetch",
    "user-cancel-terminal-response-race",
    "user-cancel-terminal-response-cancel-wins-race",
    "user-cancel-before-person-commit",
    PERSON_SOURCE_EPOCH_RESTART_FAULT,
    "replay-committed-window",
    "alter-committed-window",
    "unauthorized-frame-cache",
    "display-failure", "display-loss-after-ready", "window-close", "eos",
    "stale-generation-result",
})


def _process_specs(root: Path, output: Path, *, source: Path, model: Path,
                   headless: bool, window_count: int, fault: str,
                   request_timeout_ms: int, motion_dir: Path | None,
                   source_ids: dict[str, str], source_epoch: int,
                   observation_profile: dict[str, Any],
                   source_generations_path: Path,
                   trust_schema: Path,
                   external_source_adapter: Path | None = None,
                   observation_enabled: bool = True,
                   person_tracking: dict[str, Any] | None = None,
                   application_mode: str = "compat",
                   worker_python: Path | None = None) -> list[ProcessSpec]:
    if type(request_timeout_ms) is not int or not 1000 < request_timeout_ms <= 1200000:
        raise RunnerError("tracking request budget must be within 1001..1200000 ms")
    if fault == "user-cancel-before-person-commit" and person_tracking is None:
        raise RunnerError("person precommit cancellation fault requires person tracking")
    if fault in PERSON_BINDING_FAULTS and person_tracking is None:
        raise RunnerError("person binding fault requires person tracking")
    if fault == PERSON_SOURCE_EPOCH_RESTART_FAULT and person_tracking is None:
        raise RunnerError("person source epoch restart fault requires person tracking")
    if fault in {"replay-committed-window", "alter-committed-window"} and person_tracking is None:
        raise RunnerError("committed window replay requires person tracking")
    if fault == "unauthorized-frame-cache" and person_tracking is None:
        raise RunnerError("unauthorized cache fault requires person tracking")
    if person_tracking is not None:
        if (not observation_enabled or motion_dir is not None or
                external_source_adapter is not None or
                observation_profile.get("deliveryMode") != "replay"):
            raise RunnerError("person processes require observation replay without legacy adapters")
        for key in ("profile", "computationManifest", "reidModel", "workerPython"):
            value = person_tracking.get(key)
            if not isinstance(value, str) or not Path(value).is_absolute():
                raise RunnerError(f"person process path must be absolute: {key}")
    _required_native_programs(application_mode)
    if application_mode == "main":
        if (fault != "none" or motion_dir is not None or external_source_adapter is not None or
                not observation_enabled):
            raise RunnerError("main mode requires replay without compatibility fault/legacy adapters")
        return _main_process_specs(root, output, model=model, headless=headless,
            window_count=window_count, request_timeout_ms=request_timeout_ms,
            source_ids=source_ids, source_epoch=source_epoch,
            observation_profile=observation_profile, source_generations_path=source_generations_path,
            trust_schema=trust_schema, person_tracking=person_tracking,
            worker_python=worker_python or Path(sys.executable))
    node_budget_args = ("--request-timeout-ms", str(request_timeout_ms))
    common = ("--run-id", output.name, "--topology", str(TOPOLOGY),
              "--window-count", str(window_count))
    display = "headless" if headless else "windows"
    fault_args = (() if fault in {"none", "user-cancel-during-person-worker",
                                  PERSON_SOURCE_EPOCH_RESTART_FAULT} else
                  ("--fault", fault))
    node_fault_args = (() if fault in PERSON_BINDING_FAULTS or
                       fault == "replay-committed-window" else fault_args)
    display_fault_args = () if fault in PERSON_BINDING_FAULTS or fault in {
        "replay-committed-window",
        "alter-committed-window",
        "unauthorized-frame-cache",
        "cancel-during-active-fetch", "cancel-during-active-reference-fetch",
        "user-cancel-during-active-reference-fetch", "recover-active-reference-fetch",
    "user-cancel-after-source-switch-during-active-reference-fetch",
    "user-cancel-terminal-response-race",
    "user-cancel-terminal-response-cancel-wins-race",
    "user-cancel-before-person-commit"} else fault_args
    trust_args = ("--trust-schema", str(trust_schema))
    motion_args = () if motion_dir is None else ("--motion-dir", str(motion_dir))
    observation_stream_root = ("observation-pipes" if observation_profile.get("deliveryMode") == "live-push"
                               else "observations")
    observation_args = lambda camera: (
        ("--observation-stream-dir", str(output / f"private/{observation_stream_root}/{camera}"),
         "--observation-source-id", source_ids[camera],
         "--source-epoch", str(source_epoch),
         "--source-generations", str(source_generations_path),
         "--world-frame-id", str(observation_profile["worldFrameId"]),
         "--acquisition-kind", str(observation_profile["acquisitionKind"]),
         "--delivery-mode", "live-push" if observation_profile.get("deliveryMode") == "live-push"
         else "replay") if observation_enabled else ())
    video_args = lambda camera: (
        () if person_tracking is not None or (
            observation_enabled and observation_profile.get("deliveryMode") == "live-push")
        else ("--video-source", str(source / f"uav{camera[-1]}_1min.mp4")))
    source_model_args = () if person_tracking is not None else ("--model", str(model))
    source_frame_args = lambda camera: (() if person_tracking is not None else
                                       ("--frame-dir", str(output / f"private/frames/{camera}")))
    presentation_root = output / "private/ground-station/presentation"
    person_gs_args = (() if person_tracking is None else (
        "--tracking-profile", person_tracking["profile"],
        "--person-computation-manifest", person_tracking["computationManifest"],
        "--person-presentation-dir", str(presentation_root)))
    person_compute_args = (() if person_tracking is None else (
        "--tracking-profile", person_tracking["profile"],
        "--reid-model", person_tracking["reidModel"]))
    worker_python = sys.executable if person_tracking is None else person_tracking["workerPython"]
    observation_request_args = (
        ("--observation-schema", "uav-observation-v2", "--source-epoch", str(source_epoch),
         "--source-uav1-id", source_ids["UAV1"], "--source-uav2-id", source_ids["UAV2"],
         "--source-uav3-id", source_ids["UAV3"],
         "--source-generations", str(source_generations_path)) if observation_enabled else ())
    observation_runtime_args = (
        ("--source-epoch", str(source_epoch),
         "--world-frame-id", str(observation_profile["worldFrameId"]),
         "--acquisition-kind", str(observation_profile["acquisitionKind"]),
         "--delivery-mode", str(observation_profile["deliveryMode"]),
         "--source-generations", str(source_generations_path))
        if observation_enabled else ())
    controller = _installed_program("App_ServiceController")
    ground_station = _installed_program("UavTrackingGroundStation")
    tracking_node = _installed_program("UavTrackingNode")
    display_runner = str(_installed_tracking_script("tracking_display.py"))
    display_socket = _uav_unix_socket_path(_uav_socket_root(output), "display")
    worker_root = output / "private/compute/worker"
    display_clock_args = (
        ("--playback-clock-manifest", str(worker_root / "verified-observation-inputs.json"),
         "--playback-clock-input-dir", str(worker_root / "verified-input"))
        if observation_enabled and
        observation_profile.get("source", {}).get("kind") == "video-reference" else ())
    display_scope_args = (
        ("--observation-profile", str(output / "private/config/uav_observation_profile.json"),
         "--source-generations", str(source_generations_path)) if observation_enabled else ())
    pushers: list[ProcessSpec] = []
    external_sources: list[ProcessSpec] = []
    if observation_enabled and observation_profile.get("deliveryMode") == "live-push":
        if observation_profile.get("source", {}).get("kind") == "video-reference":
            pusher_script = str(_installed_tracking_script("observation_source.py"))
            for camera in ("UAV1", "UAV2", "UAV3"):
                node = camera.lower()
                pushers.append(ProcessSpec(
                    f"observation-push-{camera.lower()}", node,
                    f"/example/uav/source-adapter/{camera}", root,
                    (sys.executable, pusher_script, "--push-video",
                     str(source / f"uav{camera[-1]}_1min.mp4"),
                     "--profile", str(output / "private/config/uav_observation_profile.json"),
                     "--fifo-directory", str(output / f"private/observation-pipes/{camera}"),
                     "--camera", camera, "--source-id", source_ids[camera],
                     "--source-epoch", str(source_epoch),
                     "--source-generations", str(source_generations_path),
                     "--sample-rate-hz", str(observation_profile["sampleRateHz"]),
                     "--window-count", str(window_count)),
                    output / f"private/pusher-{camera}"))
        elif (observation_profile.get("source", {}).get("kind") == "external-fd" and
              external_source_adapter is not None):
            for camera in ("UAV1", "UAV2", "UAV3"):
                node = camera.lower()
                external_sources.append(ProcessSpec(
                    f"external-source-{node}", node,
                    f"/example/uav/source-adapter/{camera}", root,
                    (str(external_source_adapter), "--camera", camera,
                     "--profile", str(output / "private/config/uav_observation_profile.json"),
                     "--event-stream-dir",
                     str(output / f"private/observation-pipes/{camera}"),
                     "--source-id", source_ids[camera],
                     "--source-epoch", str(source_epoch),
                     "--source-generations", str(source_generations_path),
                     "--world-frame-id", str(observation_profile["worldFrameId"]),
                     "--sample-rate-hz", str(observation_profile["sampleRateHz"]),
                     "--window-count", str(window_count)),
                    output / f"private/external-source-{camera}"))
    specs = [
        # Controller and GS are separate identities/processes in the same
        # MiniNDN node. Their private application state stays separate, while
        # the launcher supplies one campaign-scoped PIB/TPM below so the
        # Controller can resolve and encrypt to the exact certificates used by
        # the providers and ground station.
        _actor_spec("controller", root, output,
                    (controller, "--node", "gs", "--controller-prefix",
                     "/example/uav/controller", "--policy-file",
                     str(root / "Experiments/UAV/spec191.policies"),
                     *trust_args,
                     "--no-demo-identities",
                     "--ensure-identities",
                     "/example/uav/drone/UAV1,/example/uav/drone/UAV2,"
                     "/example/uav/drone/UAV3,/example/uav/compute,/example/uav/gs",
                     *common)
                    ),
        _actor_spec("ground-station", root, output,
                    (ground_station, "--node", "gs", "--display", display,
                     "--group-prefix", "/example/uav", "--controller-prefix",
                     "/example/uav/controller", *trust_args, *motion_args,
                     *observation_request_args,
                     *person_gs_args,
                     "--request-timeout-ms",
                     str(request_timeout_ms), *common, *fault_args)
                    ),
        _actor_spec("uav1-camera", root, output,
                    (tracking_node, *node_budget_args, "--role", "source", "--camera", "UAV1",
                     *video_args("UAV1"),
                     *source_frame_args("UAV1"),
                     *observation_args("UAV1"),
                     *motion_args,
                     *trust_args,
                     *source_model_args, *common, *node_fault_args)
                    ),
        _actor_spec("uav2-camera", root, output,
                    (tracking_node, *node_budget_args, "--role", "source", "--camera", "UAV2",
                     *video_args("UAV2"),
                     *source_frame_args("UAV2"),
                     *observation_args("UAV2"),
                     *motion_args,
                     *trust_args,
                     *source_model_args, *common, *node_fault_args)
                    ),
        _actor_spec("uav3-camera", root, output,
                    (tracking_node, *node_budget_args, "--role", "source", "--camera", "UAV3",
                     *video_args("UAV3"),
                     *source_frame_args("UAV3"),
                     *observation_args("UAV3"),
                     *motion_args,
                     *trust_args,
                     *source_model_args, *common, *node_fault_args)
                    ),
        _actor_spec("compute", root, output,
                    (tracking_node, *node_budget_args, "--role", "compute", "--display", display,
                     "--model", str(model), "--worker-python", worker_python,
                     "--worker-script",
                     str(_installed_tracking_script("tracking_worker.py")),
                     *person_compute_args,
                     *observation_runtime_args,
                     *motion_args,
                     *trust_args,
                     *common, *node_fault_args)
                    ),
        ProcessSpec("compute-display", "compute", "/example/uav/compute/display", root,
                    (sys.executable, display_runner, "--socket",
                     str(display_socket),
                     "--mode", display, "--replay-json",
                     str(worker_root / "tracking-result.json"), "--replay-dir",
                     str(worker_root / "annotated"), *display_clock_args, *display_scope_args,
                     *display_fault_args),
                    output / "private/compute-display"),
    ]
    if person_tracking is not None:
        specs[-1] = ProcessSpec(
            "ground-station-display", "gs", "/example/uav/gs/display", root,
            (worker_python, display_runner, "--socket",
             str(display_socket),
             "--mode", display, "--replay-json", str(presentation_root / "latest.json"),
             "--person-display-context", str(presentation_root / "context.json"),
             *display_fault_args),
            output / "private/ground-station-display")
    # Source adapters are local producers attached to existing UAV nodes; they
    # do not add network nodes or alter the five-node forwarding topology.
    return [specs[0], *pushers, *external_sources, *specs[1:]]


def _check_python_dependencies(*, model_dependencies: bool = True) -> list[str]:
    missing: list[str] = []
    for module in (("cv2", "numpy", "ultralytics", "lap") if model_dependencies else ("cv2", "numpy")):
        try:
            __import__(module)
        except Exception as error:  # pragma: no cover - host-dependent
            missing.append(f"python:{module}:{type(error).__name__}")
    return missing


def _application_library_environment_command() -> str:
    override = os.environ.get("NDNSF_UAV_LIBRARY_PATH")
    return ("unset LD_LIBRARY_PATH" if override is None else
            f"export LD_LIBRARY_PATH={shlex.quote(override)}")


def _person_application_environment(output: Path) -> dict[str, str]:
    """Keep native inventory and eventual compute imports in the same environment."""
    private = output / "private/compute"
    values = {
        "HOME": str(private / "home"), "XDG_CONFIG_HOME": str(private / "config"),
        "XDG_DATA_HOME": str(private / "data"), "XDG_CACHE_HOME": str(private / "cache"),
        "MPLCONFIGDIR": str(private / "mpl"),
        "YOLO_CONFIG_DIR": str(private / "config/Ultralytics"),
        "PYTHONPATH": str(DEFAULT_TRACKING_LIBEXEC), "PYTHONNOUSERSITE": "1",
    }
    for key, value in values.items():
        if key not in {"PYTHONPATH", "PYTHONNOUSERSITE"}:
            Path(value).mkdir(parents=True, exist_ok=True)
    return values


def _describe_person_computation(person_tracking: dict[str, Any], model: Path,
                                 output: Path, *,
                                 application_mode: str = "compat") -> dict[str, Any]:
    """Snapshot the installed native inventory without starting an NDN application."""
    output = output.resolve()
    config = output / "private/config"
    config.mkdir(parents=True, exist_ok=True)
    descriptor = config / "person-computation.json"
    descriptor.unlink(missing_ok=True)
    _required_native_programs(application_mode)
    executable = Path(_installed_program("UavDroneApp" if application_mode == "main" else "UavTrackingNode"))
    worker = _installed_tracking_script("tracking_worker.py")
    if (not executable.is_absolute() or not executable.is_file() or
            not worker.is_absolute() or not worker.is_file()):
        raise RunnerError("person description requires installed native Node and worker")
    command = [str(executable), *([] if application_mode == "main" else ["--role", "compute"]),
               "--model", str(model.resolve()),
               "--tracking-profile", person_tracking["profile"],
               "--reid-model", person_tracking["reidModel"],
               "--worker-python", person_tracking["workerPython"],
               "--worker-script", str(worker), "--describe-person-computation", "stdout"]
    env = dict(os.environ, **_person_application_environment(output))
    override = os.environ.get("NDNSF_UAV_LIBRARY_PATH")
    if override is None:
        env.pop("LD_LIBRARY_PATH", None)
    else:
        env["LD_LIBRARY_PATH"] = override
    stdout_path = output / "person-computation-inspection.stdout"
    stderr_path = output / "person-computation-inspection.stderr"
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(command, env=env, cwd=output, stdout=stdout, stderr=stderr,
                                   start_new_session=True)
        try:
            try:
                code = process.wait(timeout=90)
            except subprocess.TimeoutExpired as error:
                raise RunnerError("installed person computation inspection exceeded 90 seconds") from error
        finally:
            # A successful/exited leader can still leave members of its owned group.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                process.poll()
                try:
                    os.killpg(process.pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(.02)
            else:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            process.wait(timeout=2)
    if code != 0:
        raise RunnerError(f"installed person computation inspection failed ({code}); see {stderr_path}")
    if not 0 < stdout_path.stat().st_size <= 64 * 1024:
        raise RunnerError("installed person computation descriptor exceeds byte bound")
    raw = stdout_path.read_bytes()
    if not isinstance(json.loads(raw), dict):
        raise RunnerError("installed person computation descriptor must be an object")
    temporary = descriptor.with_suffix(".tmp")
    temporary.write_bytes(raw)
    os.replace(temporary, descriptor)
    return {"computationManifest": str(descriptor), "computationManifestDigest": _sha256_bytes(raw),
            "installedNode": str(executable), "installedWorker": str(worker),
            "workerPython": person_tracking["workerPython"]}


def _child_pythonpath() -> str:
    """Preserve the launcher's Python runtime after HOME isolation.

    MiniNDN application processes use private HOME/XDG roots so that one
    campaign cannot reuse another campaign's caches or credentials.  Python's
    user-site directory is derived from HOME, however; changing HOME would
    otherwise hide packages (for example cv2 and ultralytics) that the
    launcher already verified.  Export the launcher's real import roots
    explicitly instead of relying on the child's user-site discovery.
    """
    paths: list[str] = []
    for item in sys.path:
        if not item:
            continue
        candidate = str(Path(item).resolve())
        if Path(candidate).is_dir() and candidate not in paths:
            paths.append(candidate)
    inherited = os.environ.get("PYTHONPATH", "").strip()
    if inherited:
        for item in inherited.split(os.pathsep):
            if item and item not in paths:
                paths.append(item)
    return os.pathsep.join(paths)


@contextmanager
def _minindn_space_delimiter_compat():
    """Keep MiniNDN 0.7.0 topology parsing compatible with Python 3.10+.

    MiniNDN uses ``ConfigParser(delimiters=' ', allow_no_value=True)``. The
    newer multi-word option regex can swallow a complete link line as a
    valueless key. Use the older first-space regex expected by MiniNDN only
    while it constructs the topology, then restore ConfigParser.
    """
    import configparser

    original_config_parser = configparser.ConfigParser

    class MiniNdnConfigParser(original_config_parser):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            if (kwargs.get("delimiters") == " " and
                    kwargs.get("allow_no_value") is True):
                self._optcre = re.compile(
                    r"(?P<option>.*?)\s*(?: (?P<vi>\ )\s* (?P<value>.*))?$",
                    re.VERBOSE)

    configparser.ConfigParser = MiniNdnConfigParser
    try:
        yield
    finally:
        configparser.ConfigParser = original_config_parser


def _write_uav_trust_schema(destination: Path) -> None:
    """Write the shared fixed-root policy; callers own the destination and root."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    template = ROOT / "examples/trust-schema.conf"
    # SVS v3 signs a StateVector at the exact group/version name, not actor Data.
    # Admit only this run's root-chained RSA actors at that protocol name.
    sync_rule = '''rule
{
  id "UAV SVS v3 StateVector"
  for data
  filter
  {
    type name
    name /example/uav/v=3
    relation equal
  }
  checker
  {
    type customized
    sig-type rsa-sha256
    key-locator
    {
      type name
      regex "^<example|8=example><uav|8=uav><drone|8=drone><UAV1|8=UAV1|UAV2|8=UAV2|UAV3|8=UAV3><KEY|8=KEY><>{1,3}$"
    }
  }
  checker
  {
    type customized
    sig-type rsa-sha256
    key-locator
    {
      type name
      regex "^<example|8=example><uav|8=uav><compute|8=compute|gs|8=gs|controller|8=controller><KEY|8=KEY><>{1,3}$"
    }
  }
}
'''
    destination.write_bytes(sync_rule.encode() + template.read_bytes())


def build_plan(*, source: Path, model: Path, output: Path, license_text: str,
               headless: bool = False, run_seconds: float = 60.0,
               window_count: int | None = None, fault: str = "none",
               global_deadline_ms: int = 60000,
               motion_fixture: Path | None = None,
               motion_profile: str | None = None,
               legacy_mode: bool = False,
               observation_profile: Path | None = None,
               observation_mode: str = "simulated",
               source_switch_after_window: Sequence[int] = (),
               external_source_adapter: Path | None = None,
               trust_schema: Path = DEFAULT_TRUST_SCHEMA,
               person_tracking: dict[str, Any] | None = None,
               application_mode: str = "compat",
               worker_python: Path | None = None) -> dict[str, Any]:
    _required_native_programs(application_mode)
    if application_mode == "main" and (fault != "none" or legacy_mode or motion_fixture is not None or
            motion_profile is not None or external_source_adapter is not None or
            source_switch_after_window or observation_mode not in {"simulated", "replay"}):
        raise RunnerError("main mode requires fixed prepared replay without compatibility faults")
    if (fault in PERSON_BINDING_FAULTS or fault == PERSON_SOURCE_EPOCH_RESTART_FAULT) and \
            person_tracking is None:
        raise RunnerError("person-specific fault requires person tracking")
    if person_tracking is not None:
        if (legacy_mode or motion_fixture is not None or motion_profile is not None or
                observation_profile is not None or external_source_adapter is not None or
                source_switch_after_window or observation_mode not in {"simulated", "replay"} or
                trust_schema != DEFAULT_TRUST_SCHEMA):
            raise RunnerError("person mode owns replay inputs and fixed-root trust configuration")
        return _build_person_plan(person_tracking, model, output, headless=headless,
                                  run_seconds=run_seconds, window_count=window_count,
                                  fault=fault, global_deadline_ms=global_deadline_ms,
                                  application_mode=application_mode)
    if window_count is not None and type(window_count) is not int:
        raise RunnerError("window_count must be an integer")
    model = model.expanduser().resolve()
    output = output.expanduser().resolve()
    trust_schema = trust_schema.expanduser().resolve()
    if not model.is_file():
        raise RunnerError(f"model checkpoint is not available: {model}")
    if not trust_schema.is_file():
        raise RunnerError(f"trust schema is not a regular file: {trust_schema}")
    output.mkdir(parents=True, exist_ok=True)
    managed_trust = trust_schema == DEFAULT_TRUST_SCHEMA.resolve()
    if managed_trust:
        keychain = output / "private/keychain"
        if keychain.exists() and any(keychain.iterdir()):
            raise RunnerError("UAV credentials require a fresh run-owned keychain")
        trust_schema = output / "private/config/uav-trust-schema.conf"
        _write_uav_trust_schema(trust_schema)
    trust_schema_digest = _sha256_bytes(trust_schema.read_bytes())
    legacy_motion = motion_fixture is not None or motion_profile is not None
    legacy_compatibility = legacy_mode or legacy_motion
    if legacy_compatibility and observation_profile is not None:
        raise RunnerError("AMBIGUOUS_INPUT: legacy mode cannot be combined with an explicit observation profile")
    if legacy_compatibility and observation_mode != "simulated":
        raise RunnerError("legacy mode cannot be combined with a Spec210 observation mode")
    effective_observation_profile = observation_profile or DEFAULT_OBSERVATION_PROFILE
    if legacy_compatibility and source_switch_after_window:
        raise RunnerError("source generation switching requires the Spec210 observation path")
    if legacy_compatibility and external_source_adapter is not None:
        raise RunnerError("--external-source-adapter cannot be combined with legacy mode")
    if legacy_compatibility:
        source, model, output = _effective_paths(source, model, output)
        profile = None
        runtime_profile: dict[str, Any] = {}
    else:
        try:
            raw_profile = effective_observation_profile.expanduser().resolve().read_bytes()
            profile = parse_observation_profile(raw_profile)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            raise RunnerError(f"observation profile is invalid: {error}") from error
        if observation_mode not in {"simulated", "replay", "live-push"}:
            raise RunnerError(f"unsupported observation mode: {observation_mode}")
        runtime_profile = copy.deepcopy(profile)
        runtime_profile["deliveryMode"] = "live-push" if observation_mode == "live-push" else "replay"
        if observation_mode == "simulated":
            if runtime_profile.get("acquisitionKind") != "simulated":
                raise RunnerError("simulated mode requires a simulated observation profile")
        source_kind = runtime_profile.get("source", {}).get("kind")
        acquisition_kind = runtime_profile.get("acquisitionKind")
        if source_kind == "external-fd" and observation_mode != "live-push":
            raise RunnerError("source.kind=external-fd requires observation-mode=live-push")
        if acquisition_kind == "measured" and (
                source_kind != "external-fd" or observation_mode != "live-push"):
            raise RunnerError("measured observations require a live external-fd source")
        if source_kind == "video-reference" and acquisition_kind != "simulated":
            raise RunnerError("video-reference observations must remain explicitly simulated")
        if external_source_adapter is not None:
            external_source_adapter = external_source_adapter.expanduser().resolve()
            if source_kind != "external-fd":
                raise RunnerError("--external-source-adapter requires source.kind=external-fd")
            if not external_source_adapter.is_file() or not os.access(external_source_adapter, os.X_OK):
                raise RunnerError(f"external source adapter is not executable: {external_source_adapter}")
        if observation_mode == "live-push":
            if source_kind not in {"video-reference", "external-fd"}:
                raise RunnerError("live-push profile must select video-reference or external-fd")
            source = source.expanduser().resolve()
            output.mkdir(parents=True, exist_ok=True)
        else:
            if source_kind != "video-reference":
                raise RunnerError("simulated/replay mode requires source.kind=video-reference")
            source, model, output = _effective_paths(source, model, output)
    try:
        topology = load_topology(TOPOLOGY)
    except (OSError, TopologyError) as error:
        raise RunnerError(f"topology preflight failed: {error}") from error
    try:
        if not legacy_compatibility and observation_mode == "live-push":
            passport = (validate_video_reference_assets(source, model, license_text)
                        if source_kind == "video-reference"
                        else validate_live_source_model(model, license_text))
        else:
            passport = validate_real_assets(source, model, license_text)
    except (ArtifactError, OSError, json.JSONDecodeError) as error:
        raise RunnerError(f"artifact preflight failed: {error}") from error
    if run_seconds <= 0:
        raise RunnerError("run_seconds must be positive")
    if global_deadline_ms <= 1000:
        raise RunnerError("global_deadline_ms must exceed the 1-second ACK timeout")
    if fault not in FAULTS:
        raise RunnerError(f"unsupported Spec191 fault: {fault}")
    if fault in {"bad-reference-digest", "bad-reference-digest-clock",
                 "bad-reference-digest-calibration", "bad-reference-digest-ground",
                 "bad-reference-digest-pose", "missing-reference-clock",
                 "duplicate-reference-pose", "overlimit-reference-pose",
                 *REFERENCE_IDENTITY_CONTENT_FAULTS} and legacy_compatibility:
        raise RunnerError("observation dependency faults require the Spec210 observation path")
    if fault in {"user-cancel-terminal-response-race",
                 "user-cancel-terminal-response-cancel-wins-race"} and legacy_compatibility:
        raise RunnerError("terminal response race faults require the Spec210 observation path")
    if legacy_motion and motion_profile is not None and motion_profile not in MOTION_PROFILES:
        raise RunnerError(f"unsupported legacy motion profile: {motion_profile}")
    if legacy_compatibility:
        if window_count is None:
            window_count = 3
        if window_count <= 0 or window_count > 60:
            raise RunnerError("legacy window_count must be in 1..60")
        if legacy_motion:
            motion_dir = (motion_fixture.expanduser().resolve() if motion_fixture is not None
                          else output / "private/motion")
            if motion_fixture is not None:
                motion_manifest_path = motion_dir / "manifest.json"
                if not motion_manifest_path.is_file():
                    raise RunnerError(f"motion fixture manifest is not available: {motion_manifest_path}")
                motion_manifest = json.loads(motion_manifest_path.read_text(encoding="utf-8"))
                if motion_manifest.get("schema") != "spec191-motion-fixture-v1":
                    raise RunnerError("motion fixture schema is not Spec191 v1")
                if int(motion_manifest.get("windowCount", 0)) < window_count:
                    raise RunnerError("motion fixture has fewer windows than requested")
            else:
                motion_manifest = write_fixture(motion_dir, window_count,
                                                profile=motion_profile or "nominal")
            motion_manifest_digest = _sha256_bytes((motion_dir / "manifest.json").read_bytes())
        else:
            motion_dir = None
            motion_manifest = None
            motion_manifest_digest = ""
        sample_rate_hz = 1.0
        source_ids = {camera: "" for camera in ("UAV1", "UAV2", "UAV3")}
        source_epoch = 1
        runtime_profile = {}
    else:
        motion_dir = None
        if window_count is None:
            window_count = int(runtime_profile.get("frameCount", DEFAULT_FRAME_COUNT))
        sample_rate_hz = float(runtime_profile.get("sampleRateHz", DEFAULT_SAMPLE_RATE_HZ))
        if not math.isfinite(sample_rate_hz) or not 1.0 <= sample_rate_hz <= 30.0:
            raise RunnerError("observation sampleRateHz must be in 1..30")
        if window_count <= 0 or window_count > MAX_REPLAY_WINDOWS:
            raise RunnerError(f"observation frameCount must be in 1..{MAX_REPLAY_WINDOWS}")
        runtime_profile["sampleRateHz"] = sample_rate_hz
        runtime_profile["frameCount"] = window_count
        source_epoch = secrets.randbits(63) or 1
        source_ids = _observation_source_ids(output, runtime_profile)
        motion_manifest = None
        motion_manifest_digest = ""

    if window_count is None or window_count <= 0:
        raise RunnerError("window_count must be positive")
    if not legacy_compatibility and window_count > MAX_REPLAY_WINDOWS:
        raise RunnerError(f"observation frameCount must be in 1..{MAX_REPLAY_WINDOWS}")
    source_generations = ({} if legacy_compatibility else
                          _source_generations(window_count, source_ids, source_epoch,
                                              source_switch_after_window))
    source_generations_path = output / "private/config/source-generations.json"
    specs = _process_specs(ROOT, output, source=source, model=model,
                           headless=headless, window_count=window_count, fault=fault,
                           request_timeout_ms=global_deadline_ms, motion_dir=motion_dir,
                           source_ids=source_ids, source_epoch=source_epoch,
                           observation_profile=runtime_profile,
                           source_generations_path=source_generations_path,
                           trust_schema=trust_schema,
                           external_source_adapter=external_source_adapter,
                           observation_enabled=not legacy_compatibility,
                           application_mode=application_mode, worker_python=worker_python)
    nodes = sorted(topology.nodes)
    if {item.node for item in specs} != set(nodes):
        raise RunnerError("process plan does not cover the exact topology nodes")
    if len(nodes) != 5 or "controller" in nodes:
        raise RunnerError("Controller must remain a gs-local process, not a MiniNDN node")
    return {
        **(_main_plan_metadata(specs, output) if application_mode == "main" else {}),
        "schema": "spec228-uav-main-tracking-run/v1" if application_mode == "main" else "spec191-uav-tracking-run/v1",
        "mode": "headless" if headless else "windows",
        "topology": {
            "path": str(TOPOLOGY),
            "nodes": nodes,
            "links": [list(link) for link in sorted(topology.links)],
            "controllerHost": topology.controller_host,
            "computeHost": topology.compute_host,
        },
        "assets": passport,
        "security": {
            "trustSchema": str(trust_schema),
            "trustSchemaDigest": trust_schema_digest,
            "trustSchemaSource": "maintained-fixed-root" if managed_trust else "explicit-input",
            **({"credentialsPrepared": False} if managed_trust else {}),
        },
        "motion": ({
            "mode": "legacy",
            "schema": motion_manifest.get("schema"),
            "provenance": motion_manifest.get("provenance", "simulated-input"),
            "profile": motion_manifest.get("profile", "nominal"),
            "windowCount": int(motion_manifest.get("windowCount", 0)),
            "manifestDigest": motion_manifest_digest,
            "directory": str(motion_dir),
        } if legacy_motion else {"mode": "disabled", "reason": "legacy-no-motion"}
            if legacy_compatibility else
            {"mode": "disabled", "reason": "v2 observations own motion inputs"}),
        "observation": ({
            "enabled": True,
            "mode": observation_mode,
            "schema": runtime_profile["schema"],
            "profile": str(effective_observation_profile.expanduser().resolve()),
            "profileDigest": _sha256_bytes(raw_profile),
            "rawProfileSnapshot": str(output / "private/config/uav_observation_profile.input.json"),
            "effectiveProfileSnapshot": str(output / "private/config/uav_observation_profile.json"),
            "effectiveProfileDigest": _sha256_bytes(
                (json.dumps(runtime_profile, indent=2, sort_keys=True) + "\n").encode()),
            "acquisitionKind": runtime_profile["acquisitionKind"],
            "deliveryMode": runtime_profile["deliveryMode"],
            "geometryValidity": runtime_profile["geometryValidity"],
            "accuracyQualification": runtime_profile["accuracyQualification"],
            "worldFrameId": runtime_profile["worldFrameId"],
            "sourceIds": source_ids,
            "sourceEpoch": source_epoch,
            "sourceGenerations": source_generations,
            "sourceGenerationManifest": str(source_generations_path),
            "sourceGenerationManifestSha256": _sha256_bytes(
                (json.dumps(source_generations, indent=2, sort_keys=True) + "\n").encode()),
            "sampleRateHz": sample_rate_hz,
            "frameCount": window_count,
            "source": {
                "kind": runtime_profile["source"]["kind"],
                "adapter": ("decoded-video-replay" if observation_mode != "live-push"
                            else "reference-push-adapter"
                            if runtime_profile["source"]["kind"] == "video-reference"
                            else "external-observation-fd"),
                "root": str(source),
                **({"writerEndpoints": {
                    "transport": "named-fifo-file-descriptor",
                    "eventSchema": "uav-source-event-v2",
                    "closeEachWindow": True,
                    "pathsByCamera": {
                        camera: [str(output / "private/observation-pipes" / camera /
                                     f"window-{window}.jsonl")
                                 for window in range(window_count)]
                        for camera in ("UAV1", "UAV2", "UAV3")
                    },
                    "writerOwner": ("runner-source-adapter" if external_source_adapter is not None
                                    else "external-caller"),
                    "generationManifest": str(source_generations_path),
                }} if runtime_profile["source"]["kind"] == "external-fd" else {}),
            },
        } if not legacy_compatibility else {"enabled": False, "mode": "legacy"}),
        "processes": [item.public_dict() for item in specs],
        "settings": {
            "nfdCsMaxPackets": NFD_CS_MAX_PACKETS,
            "sampleFps": sample_rate_hz,
            "sourceSeconds": 60.0,
            "windowSeconds": 1.0 / sample_rate_hz,
            "windowCount": window_count,
            "sourceSwitchAfterWindows": list(source_switch_after_window),
            "fault": fault,
            "ackTimeoutMs": 1000,
            "globalDeadlineMs": global_deadline_ms,
            "trackingHostRequestTimeoutMs": global_deadline_ms,
            "runSeconds": run_seconds,
            "retryReselection": False,
            "cpuOnly": True,
        },
        "scientificAccuracyClaimAllowed": False,
    }


def _build_person_plan(person: dict[str, Any], model: Path, output: Path, *, headless: bool,
                       run_seconds: float, window_count: int | None, fault: str,
                       global_deadline_ms: int, application_mode: str = "compat") -> dict[str, Any]:
    """Freeze the actual prepared source epoch and installed execution inputs."""
    from person_profile import load_person_profile, verify_person_model_files

    output = output.resolve()
    receipt_path = output / "prepared-person-sources.json"
    if not receipt_path.is_file() or receipt_path.stat().st_size > 64 * 1024:
        raise RunnerError("person plan requires its successful bounded source preparation receipt")
    receipt = json.loads(receipt_path.read_bytes())
    profile = load_person_profile(Path(person["profile"]))
    verify_person_model_files(profile, model, Path(person["reidModel"]))
    if receipt.get("status") != "PASS" or receipt.get("profileDigest") != profile.digest:
        raise RunnerError("person source preparation status/profile mismatch")
    count = receipt["windowCount"]
    if (type(count) is not int or not 1 <= count <= MAX_REPLAY_WINDOWS or
            (window_count is not None and (type(window_count) is not int or window_count != count)) or
            not math.isfinite(run_seconds) or run_seconds * 1_000_000 != receipt["durationUs"]):
        raise RunnerError("person count and source duration must match the prepared manifest")
    if (type(global_deadline_ms) is not int or global_deadline_ms <= 1000 or
            global_deadline_ms > profile.limits["runDeadlineMs"] or fault not in FAULTS):
        raise RunnerError("invalid person deadline or fault")
    observation_path = Path(receipt["observationProfile"])
    sampling_path = Path(receipt["sourceFrameInputs"])
    if (digest_file(observation_path) != receipt["observationProfileDigest"] or
            digest_file(sampling_path) != receipt["sourceFrameInputsDigest"]):
        raise RunnerError("person source preparation artifacts changed")
    observation = load_observation_profile(observation_path)
    generations_path = Path(receipt["sourceGenerations"])
    generations = load_source_generation_manifest(generations_path, count)
    first = generations["generations"][0]
    restart_expected = fault == PERSON_SOURCE_EPOCH_RESTART_FAULT
    if (type(receipt.get("sourceRestartAfterWindowZero")) is not bool or
            receipt["sourceRestartAfterWindowZero"] != restart_expected):
        raise RunnerError("prepared person source fault does not match the run fault")
    expected_generation_count = 2 if restart_expected else 1
    if (len(generations["generations"]) != expected_generation_count or
            first["firstWindowIndex"] != 0 or first["sourceEpoch"] != receipt["sourceEpoch"] or
            first["sourceIds"] != receipt["sourceIds"]):
        raise RunnerError("prepared person source identities changed")
    if restart_expected:
        restarted = generations["generations"][1]
        if (restarted["firstWindowIndex"] != 1 or
                restarted["sourceEpoch"] != first["sourceEpoch"] + 1 or
                any(restarted["sourceIds"][camera] == first["sourceIds"][camera]
                    for camera in ("UAV1", "UAV2", "UAV3"))):
            raise RunnerError("prepared person source restart identity is invalid")
    config = output / "private/config"
    trust_schema = config / "person-trust-schema.conf"
    _write_uav_trust_schema(trust_schema)
    topology = load_topology(TOPOLOGY)
    specs = _process_specs(ROOT, output, source=sampling_path.parent, model=model.resolve(),
        headless=headless, window_count=count, fault=fault, request_timeout_ms=global_deadline_ms,
        motion_dir=None, source_ids=receipt["sourceIds"], source_epoch=receipt["sourceEpoch"],
        observation_profile=observation, source_generations_path=generations_path,
        trust_schema=trust_schema, person_tracking=person, application_mode=application_mode)
    files = [receipt_path, Path(person["profile"]), Path(person["computationManifest"]),
             observation_path, generations_path, sampling_path, model.resolve(),
             Path(person["reidModel"]), TOPOLOGY,
             ROOT / ("Experiments/UAV/spec228.policies" if application_mode == "main" else "Experiments/UAV/spec191.policies")]
    files.extend(Path(__file__).with_name(name) for name in (
        "run_multicamera_tracking_demo.py", "person_run_receipt.py", "person_run_resources.py", "tracking_artifacts.py",
        "tracking_topology.py", "motion_fixture.py"))
    files.extend(Path(_installed_program(name)) for name in _required_native_programs(application_mode))
    files.extend(_installed_tracking_script(name) for name in (
        "tracking_worker.py", "tracking_display.py", "display_clock.py", "person_profile.py", "person_reid.py",
        "person_runtime.py", "requirements-person.lock", "byte_track_state.py",
        "observation_source.py", "observation_schema.py", "motion_schema.py", "motion_estimator.py")
        if application_mode != "main" or name != "tracking_display.py")
    files.extend(output / f"private/observations/{camera}/window-{index}.jsonl"
                 for camera in ("UAV1", "UAV2", "UAV3") for index in range(count))
    frozen = [{"path": str(path.resolve()), "digest": digest_file(path)} for path in files]
    return {
        **(_main_plan_metadata(specs, output) if application_mode == "main" else {}),
        "schema": "spec228-uav-main-tracking-run/v1" if application_mode == "main" else "spec219-uav-person-run/v1", "mode": "headless" if headless else "windows",
        "topology": {"path": str(TOPOLOGY), "nodes": sorted(topology.nodes),
                     "links": [list(link) for link in sorted(topology.links)],
                     "controllerHost": topology.controller_host, "computeHost": topology.compute_host},
        "personTracking": {**person, "profileDigest": profile.digest,
                           "requestedDevice": profile.requested_device, "frozenFiles": frozen,
                           "preparationReceipt": str(receipt_path)},
        "security": {"trustSchema": str(trust_schema), "trustSchemaDigest": digest_file(trust_schema),
                     "credentialsPrepared": False, "trustSchemaSource": "maintained-fixed-root"},
        "motion": {"mode": "disabled", "reason": "indoor person results carry unknown geometry"},
        "observation": {"enabled": True, "mode": "replay", "deliveryMode": "replay",
                        "profile": str(observation_path), "profileDigest": digest_file(observation_path),
                        "sourceIds": receipt["sourceIds"], "sourceEpoch": receipt["sourceEpoch"],
                        "sourceGenerations": generations, "sourceGenerationManifest": str(generations_path),
                        "sourceGenerationManifestSha256": digest_file(generations_path),
                        "source": {"kind": "video-reference", "root": str(sampling_path.parent)},
                        "synchronizationQualification": receipt["synchronizationQualification"]},
        "processes": [spec.public_dict() for spec in specs],
        "settings": {"nfdCsMaxPackets": NFD_CS_MAX_PACKETS, "sampleFps": 2, "sourceSeconds": run_seconds, "windowSeconds": .5,
                     "windowCount": count, "sourceSwitchAfterWindows": [], "fault": fault,
                     "ackTimeoutMs": 1000, "globalDeadlineMs": global_deadline_ms,
                     "trackingHostRequestTimeoutMs": global_deadline_ms,
                     "runSeconds": run_seconds, "retryReselection": False,
                     "requestedDevice": profile.requested_device},
        "scientificAccuracyClaimAllowed": False,
    }


def _verify_person_plan(plan: dict[str, Any]) -> None:
    for item in plan["personTracking"]["frozenFiles"]:
        path = Path(item["path"])
        if not path.is_file() or digest_file(path) != item["digest"]:
            raise RunnerError(f"frozen person execution input changed: {path}")


def _person_final_display_shown(text: str, *, run_id: str, epoch: int,
                                window_index: int, result_digest: str) -> bool:
    shown = set()
    for line in text.splitlines():
        try:
            item = json.loads(line)
        except ValueError:
            continue
        if (isinstance(item, dict) and item.get("event") == "DISPLAY_FRAME_SHOWN" and
                item.get("runId") == run_id and item.get("sessionId") == run_id and
                type(item.get("window")) is int and item["window"] == window_index and
                type(item.get("epoch")) is int and item["epoch"] == epoch and
                type(item.get("sequence")) is int and item["sequence"] == window_index + 1 and
                isinstance(item.get("personDisplay"), dict) and
                item["personDisplay"].get("resultDigest") == result_digest):
            shown.add(item.get("cameraId"))
    return shown == {"UAV1", "UAV2", "UAV3"}


def _person_failure_display_presented(text: str, *, run_id: str, window_index: int,
                                      projection_digest: str, mode: str) -> bool:
    if mode not in ("windows", "headless"):
        return False
    event = ("DISPLAY_PERSON_FAILURE_SHOWN" if mode == "windows"
             else "DISPLAY_PERSON_FAILURE_OBSERVED")
    presented = set()
    for line in text.splitlines():
        try:
            item = json.loads(line)
        except ValueError:
            continue
        if (isinstance(item, dict) and item.get("event") == event and
                item.get("mode") == mode and item.get("runId") == run_id and
                item.get("missionId") == run_id and
                type(item.get("window")) is int and item["window"] == window_index and
                item.get("projectionDigest") == projection_digest and
                item.get("clockDomain") == "CLOCK_MONOTONIC_NS" and
                type(item.get("monotonicNs")) is int and item["monotonicNs"] > 0):
            camera = item.get("cameraId")
            if camera in ("UAV1", "UAV2", "UAV3"):
                presented.add(camera)
    return presented == {"UAV1", "UAV2", "UAV3"}


def _provision_run_security(output: Path) -> dict[str, Any]:
    """Use ndnsec to provision one fresh, run-owned RSA trust chain."""
    keychain = output / "private/keychain"
    if keychain.exists() and any(keychain.iterdir()):
        raise RunnerError("UAV credentials require a fresh run-owned keychain")
    keychain.mkdir(parents=True, exist_ok=True, mode=0o700)
    keychain.chmod(0o700)
    for part in ("pib", "tpm", "home"):
        (keychain / part).mkdir(mode=0o700)
    env = dict(os.environ, HOME=str(keychain / "home"),
               NDN_CLIENT_PIB="pib-sqlite3:" + str(keychain / "pib"),
               NDN_CLIENT_TPM="tpm-file:" + str(keychain / "tpm"))
    override = os.environ.get("NDNSF_UAV_LIBRARY_PATH")
    if override is None:
        env.pop("LD_LIBRARY_PATH", None)
    else:
        env["LD_LIBRARY_PATH"] = override
    ndnsec = shutil.which("ndnsec")
    if ndnsec is None:
        raise RunnerError("installed ndnsec is required for UAV credentials")
    deadline = time.monotonic() + 120

    def invoke(args: list[str], payload: bytes | None = None) -> bytes:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RunnerError("UAV credential provisioning exceeded 120 seconds")
        result = subprocess.run([ndnsec, *args], input=payload, env=env, capture_output=True,
                                timeout=min(10, remaining), check=False)
        if result.returncode or len(result.stdout) > 64 * 1024:
            raise RunnerError("ndnsec credential step failed: " + " ".join(args[:2]) +
                              ": " + result.stderr.decode(errors="replace")[:1024])
        return result.stdout

    config = output / "private/config"
    root_identity = "/example/uav"
    root_certificate = invoke(["key-gen", "-t", "r", root_identity])
    (config / "root.cert").write_bytes(root_certificate)
    public = [{"identity": root_identity, "path": str(config / "root.cert"),
               "digest": digest_file(config / "root.cert")}]
    not_before = datetime.now(timezone.utc).strftime("%Y%m%d%H%M%S")
    actors = ("controller", "drone/UAV1", "drone/UAV2", "drone/UAV3", "compute", "gs")
    for actor in actors:
        identity = root_identity + "/" + actor
        request = invoke(["key-gen", "-t", "r", identity])
        signed = invoke(["cert-gen", "-s", root_identity, "-i", "spec219", "-S", not_before, "-"], request)
        certificate_path = config / ("actor-" + actor.replace("/", "-") + ".cert")
        certificate_path.write_bytes(signed)
        invoke(["cert-install", "-I", "-f", str(certificate_path)])
        installed = invoke(["cert-dump", "-i", identity])
        if base64.b64decode(installed) != base64.b64decode(signed):
            raise RunnerError("root-signed actor certificate is not its installed default: " + identity)
        public.append({"identity": identity, "path": str(certificate_path),
                       "digest": digest_file(certificate_path)})
    # Native PIB readers treat SQLITE_BUSY as a missing certificate. Keep readers
    # independent of another actor's write transaction in this run-owned store.
    try:
        with closing(sqlite3.connect(keychain / "pib" / "pib.db", timeout=5)) as database:
            if database.execute("PRAGMA journal_mode=WAL").fetchone() != ("wal",):
                raise RunnerError("run-owned shared PIB requires WAL journal mode")
    except sqlite3.Error as error:
        raise RunnerError("cannot configure run-owned shared PIB journal: " + str(error)) from error
    return {"credentialsPrepared": True, "rootIdentity": root_identity,
            "publicCertificates": public}


def _write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _snapshot_observation_profile(observation: dict[str, Any], output: Path) -> dict[str, Any]:
    """Keep original configuration provenance separate from runtime overrides."""
    raw = Path(observation["profile"]).read_bytes()
    if _sha256_bytes(raw) != observation["profileDigest"]:
        raise RunnerError("observation profile changed after the run plan was created")
    profile = parse_observation_profile(raw)
    raw_path = output / "private/config/uav_observation_profile.input.json"
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = raw_path.with_suffix(raw_path.suffix + ".tmp")
    temporary.write_bytes(raw)
    os.replace(temporary, raw_path)
    profile["sampleRateHz"] = float(observation["sampleRateHz"])
    profile["frameCount"] = int(observation["frameCount"])
    profile["deliveryMode"] = observation["deliveryMode"]
    effective_path = output / "private/config/uav_observation_profile.json"
    _write_json(effective_path, profile)
    if _sha256_bytes(effective_path.read_bytes()) != observation["effectiveProfileDigest"]:
        raise RunnerError("effective observation profile does not match the run plan")
    return profile


def _video_presentation_timestamps(video: Path, *, end_time_us: int | None = None) -> list[int]:
    """Read decoded video-frame PTS through ffprobe; never infer time from FPS."""
    interval = []
    if end_time_us is not None:
        if type(end_time_us) is not int or not 0 < end_time_us < 2**63:
            raise RunnerError("video PTS interval end must be a positive signed64-bit integer")
        # Empty start preserves original frame ordinals; do not seek into the video.
        # Packet cutoff can precede delayed presentation frames; bound lookahead,
        # then trim decoded timestamps to the caller's original exclusive end.
        probe_end_us = min(end_time_us + 1000000, 2**63 - 1)
        interval = ["-read_intervals", f"%{probe_end_us // 1000000}.{probe_end_us % 1000000:06d}"]
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
         "-show_entries", "frame=best_effort_timestamp_time", "-of", "json", *interval, str(video)],
        check=False, capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        raise RunnerError(f"ffprobe failed for {video}: {result.stderr.strip()}")
    try:
        frames = json.loads(result.stdout).get("frames", [])
        timestamps = [
            int((Decimal(str(frame["best_effort_timestamp_time"])) * Decimal(1_000_000))
                .quantize(Decimal("1"), rounding=ROUND_HALF_UP))
            for frame in frames
        ]
    except (json.JSONDecodeError, KeyError, InvalidOperation, TypeError, ValueError) as error:
        raise RunnerError(f"video has missing or malformed decoded-frame PTS: {video}") from error
    if not timestamps or any(current <= previous
                             for previous, current in zip(timestamps, timestamps[1:])):
        raise RunnerError(f"video frame PTS is empty or non-monotonic: {video}")
    if end_time_us is not None:
        timestamps = [pts for pts in timestamps if pts < end_time_us]
        if not timestamps:
            raise RunnerError(f"video has no decoded frames before the requested end: {video}")
    return timestamps


def _prepare_legacy_source_frame_files(source: Path, output: Path, *,
                                       window_count: int) -> dict[str, list[str]]:
    """Materialize legacy JPEGs for the explicit Spec191 motion-fixture path."""
    import cv2

    frame_dir = output / "private/frames"
    frame_dir.mkdir(parents=True, exist_ok=True)
    records: dict[str, list[str]] = {}
    sampling: dict[str, dict[str, Any]] = {}
    for camera in ("UAV1", "UAV2", "UAV3"):
        video = source / f"uav{camera[-1]}_1min.mp4"
        capture = cv2.VideoCapture(str(video))
        camera_dir = frame_dir / camera
        camera_dir.mkdir(parents=True, exist_ok=True)
        paths: list[str] = []
        try:
            timestamps = _video_presentation_timestamps(video)
            sampling[camera] = {"periodUs": 1_000_000, "frameIndices": [],
                                "captureTimeUs": []}
            decoded_index = -1
            for window in range(window_count):
                target_us = window * 1_000_000
                target_index = next((i for i, pts in enumerate(timestamps)
                                     if pts - timestamps[0] >= target_us), None)
                if target_index is None:
                    raise RunnerError(f"video is too short for legacy frame {window}: {video}")
                while decoded_index < target_index:
                    ok, frame = capture.read()
                    if not ok or frame is None:
                        raise RunnerError(f"cannot decode legacy frame {window} for {camera}: {video}")
                    decoded_index += 1
                target = camera_dir / f"window-{window}.jpg"
                encoded, data = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                if not encoded or data.nbytes == 0:
                    raise RunnerError(f"cannot encode legacy frame {window} for {camera}")
                target.write_bytes(data.tobytes())
                paths.append(str(target))
                sampling[camera]["frameIndices"].append(target_index)
                sampling[camera]["captureTimeUs"].append(timestamps[target_index] - timestamps[0])
        finally:
            capture.release()
        records[camera] = paths
    _write_json(output / "source-frame-inputs.json", {
        "schema": "spec191-source-frame-inputs/v3", "frames": records,
        "windowCount": window_count, "sampling": sampling,
    })
    return records


def _prepare_source_frame_files(source: Path, output: Path, *, window_count: int,
                                profile: dict[str, Any] | None = None,
                                source_ids: dict[str, str] | None = None,
                                source_epoch: int = 1,
                                source_generations: dict[str, Any] | None = None,
                                legacy: bool = False,
                                person_sources: dict[str, Any] | None = None) -> dict[str, list[str]]:
    """Create bounded JPEG JSONL bundles for source-owned observation replay."""
    if legacy:
        if person_sources is not None:
            raise RunnerError("person sources cannot use legacy frame preparation")
        return _prepare_legacy_source_frame_files(source, output, window_count=window_count)
    if profile is None or source_ids is None or source_generations is None:
        raise RunnerError("v2 observation replay requires its profile and source identities")
    import cv2

    sample_rate = float(profile.get("sampleRateHz", DEFAULT_SAMPLE_RATE_HZ))
    if person_sources is not None and (
            sample_rate != 2.0 or window_count != person_sources["windowCount"] or
            profile["clock"]["a"] != 1.0 or profile["clock"]["bUs"] != 0.0):
        raise RunnerError("person preparation requires the admitted count and identity 2 Hz clock")
    if not math.isfinite(sample_rate) or not 1.0 <= sample_rate <= 30.0:
        raise RunnerError("observation sampleRateHz must be in 1..30")
    period_us = int((Decimal(1_000_000) / Decimal(str(sample_rate)))
                    .quantize(Decimal("1"), rounding=ROUND_HALF_UP))
    observation_dir = output / ("private/push-events" if profile.get("deliveryMode") == "live-push"
                                else "private/observations")
    observation_dir.mkdir(parents=True, exist_ok=True)
    records: dict[str, list[str]] = {}
    sampling: dict[str, dict[str, Any]] = {}
    preparation_deadline = time.monotonic() + 300
    for camera_index, camera in enumerate(("UAV1", "UAV2", "UAV3")):
        person_view = person_sources["cameras"][camera_index] if person_sources is not None else None
        video = Path(person_view["videoPath"]) if person_view is not None else source / f"uav{camera[-1]}_1min.mp4"
        if not video.is_file():
            raise RunnerError(f"video is unavailable for {camera}: {video}")
        timestamps = (_video_presentation_timestamps(
            video, end_time_us=person_view["videoStartUs"] + person_sources["durationUs"])
            if person_view is not None else _video_presentation_timestamps(video))
        origin_us = person_view["videoStartUs"] if person_view is not None else timestamps[0]
        common_start_us = person_sources["commonStartUs"] if person_view is not None else 0
        if person_view is not None and (
                timestamps[-1] + person_view["framePeriodUs"] < origin_us + person_sources["durationUs"]):
            raise RunnerError(f"person source video does not cover the declared interval for {camera}")
        targets = [index * period_us for index in range(window_count)]
        selected: list[int] = []
        cursor = 0
        for target_us in targets:
            while cursor < len(timestamps) and timestamps[cursor] - origin_us < target_us:
                cursor += 1
            if cursor >= len(timestamps):
                raise RunnerError(f"video is too short for {window_count} frames at "
                                  f"{sample_rate:g} Hz for {camera}: {video}")
            if selected and cursor <= selected[-1]:
                raise RunnerError(f"video cannot provide unique {sample_rate:g} Hz frames for {camera}")
            if person_view is not None and (
                    timestamps[cursor] - origin_us - target_us > person_sources["maxClockSkewUs"]):
                raise RunnerError(f"person source sample lateness exceeds clock bound for {camera}")
            selected.append(cursor)

        camera_dir = observation_dir / camera
        camera_dir.mkdir(parents=True, exist_ok=True)
        paths: list[str] = []
        decoded_index = -1
        frame_indices: list[int] = []
        capture_times: list[int] = []
        target_offsets: list[int] = []
        input_digests: list[str] = []
        capture = (cv2.VideoCapture(str(video), cv2.CAP_FFMPEG,
                                   [cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, 5000,
                                    cv2.CAP_PROP_READ_TIMEOUT_MSEC, 5000])
                   if person_view is not None else cv2.VideoCapture(str(video)))
        try:
            if person_view is not None and not capture.isOpened():
                raise RunnerError(f"cannot open person source video for {camera}")
            for window, frame_index in enumerate(selected):
                while decoded_index < frame_index:
                    if person_view is not None and time.monotonic() >= preparation_deadline:
                        raise RunnerError("person source preparation exceeded 300 seconds")
                    ok, frame = capture.read()
                    if not ok or frame is None:
                        raise RunnerError(f"cannot decode PTS-selected frame {window} for {camera}: {video}")
                    decoded_index += 1
                if frame.ndim < 2 or frame.shape[0] <= 0 or frame.shape[1] <= 0:
                    raise RunnerError(f"decoded frame has invalid dimensions for {camera}: {video}")
                encoded, data = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                if not encoded or data.nbytes == 0:
                    raise RunnerError(f"cannot encode observation frame {window} for {camera}")
                if person_view is not None and (
                        data.nbytes > person_sources["maxFrameBytes"] or
                        frame.shape[1] > person_sources["maxFrameWidth"] or
                        frame.shape[0] > person_sources["maxFrameHeight"]):
                    raise RunnerError(f"person decoded image exceeds profile bounds for {camera}")
                target = camera_dir / f"window-{window}.jsonl"
                temporary = target.with_suffix(".jsonl.tmp")
                generation = source_generation_for_window(source_generations, window)
                jpeg_bytes = data.tobytes()
                with temporary.open("wb") as stream:
                    write_profile_frame(
                        stream, profile, camera, window + 1, common_start_us + timestamps[frame_index] - origin_us,
                        jpeg_bytes, source_id=str(generation["sourceIds"][camera]),
                        epoch=int(generation["sourceEpoch"]),
                        source_time_origin_us=0, image_width=int(frame.shape[1]),
                        image_height=int(frame.shape[0]))
                if person_view is not None and target.exists():
                    if digest_file(temporary) != digest_file(target):
                        temporary.unlink()
                        raise RunnerError("person source retry would change an immutable prepared frame")
                    temporary.unlink()
                else:
                    os.replace(temporary, target)
                paths.append(str(target))
                frame_indices.append(frame_index)
                capture_times.append(common_start_us + timestamps[frame_index] - origin_us)
                target_offsets.append(timestamps[frame_index] - origin_us - targets[window])
                input_digests.append(_sha256_bytes(jpeg_bytes))
        finally:
            capture.release()
        records[camera] = paths
        sampling[camera] = {
            "sourcePresentationTimeOriginUs": origin_us,
            "periodUs": period_us,
            "frameIndices": frame_indices,
            "targetTimeUs": [common_start_us + target for target in targets],
            "captureTimeUs": capture_times,
            "targetOffsetUs": target_offsets,
        }
        if person_view is not None:
            if digest_file(video) != person_view["videoDigest"]:
                raise RunnerError("person source video changed during preparation")
            sampling[camera].update({"videoPath": str(video), "videoDigest": person_view["videoDigest"],
                                     "viewId": person_view["viewId"],
                                     "rawPresentationTimeUs": [timestamps[index] for index in selected],
                                     "inputDigests": input_digests,
                                     "mappingAuthority": "operator-declared"})
    _write_json(output / "source-frame-inputs.json", {
        **({"personSourceManifestDigest": person_sources["manifestDigest"],
            "synchronizationQualification": "unverified"} if person_sources is not None else {}),
        "schema": "uav-source-observation-inputs/v1",
        "frames": records,
        "windowCount": window_count,
        "sampleRateHz": sample_rate,
        "sampling": sampling,
        "acquisitionKind": profile["acquisitionKind"],
        "deliveryMode": profile["deliveryMode"],
        "geometryValidity": profile["geometryValidity"],
        "accuracyQualification": profile["accuracyQualification"],
        "note": "source-owned observation JSONL generated from decoded frames and ffprobe presentation timestamps",
    })
    return records


def prepare_person_sources(source_manifest: Path, output: Path,
                           profile: PersonProfile, *,
                           source_restart_after_window_zero: bool = False) -> dict[str, Any]:
    """Prepare source-owned replay through the maintained observation writer.

    An exclusive run-owned directory and immutable retries keep one epoch tied
    to one set of prepared bytes. Synchronization/GT qualification is external.
    """
    if type(source_restart_after_window_zero) is not bool:
        raise RunnerError("person source restart fault flag must be boolean")
    source_manifest = source_manifest.expanduser().resolve(strict=True)
    with source_manifest.open("rb") as stream:
        raw_source = stream.read(65537)
    if not raw_source or len(raw_source) > 65536:
        raise RunnerError("person source manifest must be within 64 KiB")
    source_manifest_digest = _sha256_bytes(raw_source)
    output = output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    lock_path = output / ".person-source.lock"
    owner_path = output / ".person-source-owner.json"
    if lock_path.is_symlink() or owner_path.is_symlink():
        raise RunnerError("person source ownership files cannot be symlinks")
    if not owner_path.exists() and any(path.name != lock_path.name for path in output.iterdir()):
        raise RunnerError("person preparation requires an empty or matching owned directory")
    with lock_path.open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        raw_template = DEFAULT_OBSERVATION_PROFILE.read_bytes()
        owner = {"schema": "uav-person-source-owner-v1", "profileDigest": profile.digest,
                 "sourceManifestDigest": source_manifest_digest,
                 "observationTemplateDigest": _sha256_bytes(raw_template)}
        if owner_path.exists():
            if not 0 < owner_path.stat().st_size <= 4096:
                raise RunnerError("person source owner marker exceeds its bound")
            previous = json.loads(owner_path.read_text())
            epoch = previous.get("sourceEpoch")
            if (type(epoch) is not int or not 1 <= epoch < 2**63 or
                    previous != dict(owner, sourceEpoch=epoch)):
                raise RunnerError("person preparation owner/context mismatch")
        else:
            epoch = secrets.randbits(63) or 1
            _write_json(owner_path, dict(owner, sourceEpoch=epoch))
        receipt_path = output / "prepared-person-sources.json"
        receipt = {**owner, "schema": "uav-person-source-preparation-v1", "status": "PREPARING",
                   "sourceEpoch": epoch, "sampleRateHz": 2,
                   "sourceRestartAfterWindowZero": source_restart_after_window_zero,
                   "synchronizationQualification": "unverified"}
        _write_json(receipt_path, receipt)
        try:
            sources = load_person_source_manifest(source_manifest, profile.limits)
            if sources["manifestDigest"] != source_manifest_digest:
                raise RunnerError("person source manifest changed during admission")
            receipt.update(windowCount=sources["windowCount"], sourceIds=sources["sourceIds"],
                           datasetOrigin=sources["datasetOrigin"],
                           commonStartUs=sources["commonStartUs"], durationUs=sources["durationUs"])
            observation = parse_observation_profile(raw_template)
            observation.update(sampleRateHz=2, frameCount=sources["windowCount"],
                               deliveryMode="replay", acquisitionKind="simulated",
                               accuracyQualification="unvalidated", geometryValidity="illustrative",
                               worldFrameId="indoor-replay-illustrative", source={"kind": "video-reference"})
            observation["ground"]["geometryValidity"] = "illustrative"
            observation["clock"].update(a=1.0, bUs=0.0, clockId="person-replay-media-time")
            switches = (1,) if source_restart_after_window_zero else ()
            generations = _source_generations(
                sources["windowCount"], sources["sourceIds"], epoch, switches)
            config = output / "private/config"
            observation_path = config / "uav_observation_profile.json"
            generations_path = config / "source-generations.json"
            tracking_path = config / "uav_person_tracking_profile.json"
            _write_json(observation_path, observation)
            _write_json(generations_path, generations)
            tracking_path.write_bytes(profile.canonical_bytes)
            (config / "person-source-manifest.input.json").write_bytes(raw_source)
            _prepare_source_frame_files(Path(sources["manifestPath"]).parent, output,
                                        window_count=sources["windowCount"], profile=observation,
                                        source_ids=sources["sourceIds"], source_epoch=epoch,
                                        source_generations=generations, person_sources=sources)
            receipt.update(status="PASS", observationProfile=str(observation_path),
                           observationProfileDigest=digest_file(observation_path),
                           sourceGenerations=str(generations_path), trackingProfile=str(tracking_path),
                           sourceFrameInputs=str(output / "source-frame-inputs.json"),
                           sourceFrameInputsDigest=digest_file(output / "source-frame-inputs.json"))
            _write_json(receipt_path, receipt)
            return receipt
        except BaseException as error:
            receipt.update(status="FAIL", failureReason=str(error)[:1024] or type(error).__name__)
            _write_json(receipt_path, receipt)
            raise


def _verify_main_plan(plan: dict[str, Any], *, prepared: bool = False) -> None:
    if plan.get('applicationMode') != 'main':
        return
    main = plan['trackingMain']
    processes = {item['name']: item for item in plan['processes']}
    if set(processes) != set(TRACKING_ACTORS) or len(plan['processes']) != len(processes):
        raise RunnerError('main process plan must contain exactly the six maintained actors')
    for name, spec in processes.items():
        node, identity, _ = TRACKING_ACTORS[name]
        executable = ('App_ServiceController' if name == 'controller' else
                      'UavGroundStationApp' if name == 'ground-station' else 'UavDroneApp')
        if (spec['node'] != node or spec['identity'] != identity or
                Path(spec['command'][0]) != Path(_installed_program(executable))):
            raise RunnerError(f'main actor executable/identity mismatch: {name}')
    gs_command = processes['ground-station']['command']
    controller_command = processes['controller']['command']
    def argument(command, flag):
        if command.count(flag) != 1 or command.index(flag) + 1 >= len(command):
            raise RunnerError('missing/duplicate main command argument: ' + flag)
        return command[command.index(flag) + 1]
    if (argument(gs_command, '--ground-station-identity') != TRACKING_ACTORS['ground-station'][1] or
            argument(controller_command, '--controller-prefix') != TRACKING_ACTORS['controller'][1] or
            argument(gs_command, '--controller-prefix') != TRACKING_ACTORS['controller'][1]):
        raise RunnerError('main command actor identity mismatch')
    expected_jobs = [item['path'] for item in main['jobs']]
    actual_jobs = [gs_command[index + 1] for index, flag in enumerate(gs_command[:-1])
                   if flag == '--tracking-job-config']
    if actual_jobs != expected_jobs or '--tracking-auto-start' not in gs_command:
        raise RunnerError('main GS command job catalog mismatch')
    trust = plan.get('security', {}).get('trustSchema')
    if trust is not None and any(argument(item['command'], '--trust-schema') != trust
                                 for item in processes.values()):
        raise RunnerError('main command trust-schema mismatch')
    if argument(controller_command, '--policy-file') != str(ROOT / 'Experiments/UAV/spec228.policies'):
        raise RunnerError('main controller policy mismatch')
    for item in [*main['frozenFiles'], *main['sourceFiles']]:
        path = Path(item['path'])
        if not path.is_file() or path.is_symlink() or digest_file(path) != item['digest']:
            raise RunnerError(f'frozen main input changed: {path}')
    if not 1 <= len(main['jobs']) <= 16:
        raise RunnerError('main job catalog bound')
    if set(main['providerConfigs']) != {'uav1-camera', 'uav2-camera', 'uav3-camera', 'compute'}:
        raise RunnerError('main provider catalog requires all four actors')
    for name, reference in main['providerConfigs'].items():
        path = Path(reference['path'])
        if digest_file(path) != reference['digest']:
            raise RunnerError('main provider configuration changed')
        catalog = json.loads(path.read_bytes())
        command = processes[name]['command']
        if (catalog['producerIdentity'] != processes[name]['identity'] or
                command[command.index('--tracking-provider-config') + 1] != str(path) or
                command[command.index('--drone-prefix') + 1] + '/' +
                command[command.index('--drone-id') + 1] != catalog['producerIdentity']):
            raise RunnerError('main provider config/actual actor identity mismatch')
    for reference in main['jobs']:
        if digest_file(Path(reference['path'])) != reference['digest']:
            raise RunnerError('main job definition changed')
    if prepared and not main['sourceFiles']:
        raise RunnerError('main source files must be frozen after preparation')


def _freeze_main_sources(plan: dict[str, Any]) -> None:
    if plan.get('applicationMode') != 'main':
        return
    main = plan['trackingMain']
    files = set()
    for reference in main['providerConfigs'].values():
        catalog = json.loads(Path(reference['path']).read_bytes())
        for run in catalog['runs']:
            if run['role'] == 'source':
                directory = Path(run['observationStreamDirectory'])
                for index in range(run['windowCount']):
                    path = directory / f'window-{index}.jsonl'
                    if not path.is_file() or path.is_symlink():
                        raise RunnerError(f'main prepared source is missing: {path}')
                    files.add(path)
    if len(files) > 16 * 3 * MAX_REPLAY_WINDOWS:
        raise RunnerError('main source snapshot count exceeds catalog bound')
    snapshot = [_main_asset(path) for path in sorted(files)]
    if main['sourceFiles'] and main['sourceFiles'] != snapshot:
        raise RunnerError('prepared main source bytes changed')
    main['sourceFiles'] = snapshot
    _verify_main_plan(plan, prepared=True)



def prepare_main_plan_sources(plan: dict[str, Any]) -> None:
    """Prepare the existing replay adapter once, before composing native catalogs."""
    _verify_main_plan(plan)
    job = json.loads(Path(plan['trackingMain']['jobs'][0]['path']).read_bytes())
    if len(plan['trackingMain']['jobs']) != 1:
        raise RunnerError('prepare individual main plans before composing a sequence')
    output = Path(job['outputDirectory']).parent
    with _uav_run_ownership(output), (output / '.person-source.lock').open('a') as source_lock:
        fcntl.flock(source_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if (output / 'lifecycle.json').exists() or (output / 'main-network-attempt.json').exists():
            raise RunnerError('main input already attempted network execution')
        if 'personTracking' in plan:
            _verify_person_plan(plan)
        elif not plan['trackingMain']['sourceFiles']:
            observation = plan['observation']
            profile = _snapshot_observation_profile(observation, output)
            generations_path = Path(observation['sourceGenerationManifest'])
            if digest_file(generations_path) != observation['sourceGenerationManifestSha256']:
                raise RunnerError('main source generation changed before preparation')
            generations = load_source_generation_manifest(generations_path, job['windowCount'])
            _prepare_source_frame_files(Path(observation['source']['root']), output,
                window_count=job['windowCount'], profile=profile,
                source_ids=observation['sourceIds'], source_epoch=observation['sourceEpoch'],
                source_generations=generations)
        _freeze_main_sources(plan)
        _write_json(output / 'manifest.json', plan)


def _nfd_cache_packets(plan: dict[str, Any]) -> int:
    value = plan.get("settings", {}).get("nfdCsMaxPackets")
    if type(value) is not int or value != NFD_CS_MAX_PACKETS:
        raise RunnerError("NFD cache size differs from the frozen 4096-packet profile")
    return value


def combine_main_plans(plans: Sequence[dict[str, Any]], *, output: Path,
                       global_deadline_ms: int) -> dict[str, Any]:
    """Compose prepared native catalogs; the maintained runner still owns every actor."""
    if not 1 <= len(plans) <= 16 or type(global_deadline_ms) is not int or not 1000 < global_deadline_ms <= 1200000:
        raise RunnerError('invalid main sequence count or frozen overall budget')
    output = output.absolute()
    if output.is_symlink() or output != output.resolve() or (output / 'lifecycle.json').exists():
        raise RunnerError('main sequence requires a fresh canonical output directory')
    first = plans[0]
    jobs, frozen, sources, catalogs, run_ids, job_ids, outputs, epochs = [], {}, {}, {}, set(), set(), set(), set()
    for plan in plans:
        _nfd_cache_packets(plan)
        _verify_main_plan(plan, prepared=True)
        if len(plan['trackingMain']['jobs']) != 1 or plan['security'].get('credentialsPrepared'):
            raise RunnerError('compose individual main plans before provisioning or network execution')
        if (plan['topology'] != first['topology'] or plan['mode'] != first['mode'] or
                plan['security']['trustSchemaDigest'] != first['security']['trustSchemaDigest']):
            raise RunnerError('main sequence topology/display/trust root mismatch')
        for left, right in zip(first['processes'], plan['processes']):
            if any(left[key] != right[key] for key in ('name', 'node', 'identity', 'cwd')) or left['command'][0] != right['command'][0]:
                raise RunnerError('main sequence actor/installed artifact mismatch')
        reference = plan['trackingMain']['jobs'][0]
        job = json.loads(Path(reference['path']).read_bytes())
        child = Path(job['outputDirectory']).parent
        if (child == output or child in output.parents or output in child.parents or
                any((child / name).exists() for name in ('lifecycle.json', 'person-network-attempt.json', 'main-network-attempt.json'))):
            raise RunnerError('main sequence child must be fresh and outside campaign output')
        if (job['runId'] in run_ids or job['jobId'] in job_ids or job['outputDirectory'] in outputs or
                job['deadlineMs'] > global_deadline_ms or job['requestTimeoutMs'] > global_deadline_ms):
            raise RunnerError('main sequence repeats an identity or exceeds its frozen budget')
        run_ids.add(job['runId']); job_ids.add(job['jobId']); outputs.add(job['outputDirectory'])
        for source in job['sources']:
            binding = (source['producerIdentity'], source['sourceEpoch'])
            if binding in epochs:
                raise RunnerError('main sequence reuses a Producer/source epoch')
            epochs.add(binding)
        jobs.append(reference)
        extra = plan.get('personTracking', {}).get('frozenFiles', [])
        for item in [*plan['trackingMain']['frozenFiles'], *extra]:
            if item['path'] in frozen and frozen[item['path']]['digest'] != item['digest']:
                raise RunnerError('main sequence frozen source/artifact mismatch')
            frozen[item['path']] = item
        for item in plan['trackingMain']['sourceFiles']:
            sources[item['path']] = item
        for name, ref in plan['trackingMain']['providerConfigs'].items():
            catalog = json.loads(Path(ref['path']).read_bytes())
            if name not in catalogs:
                catalogs[name] = copy.deepcopy(catalog)
                catalogs[name]['ownedWorkspace'] = str(output / 'private' / TRACKING_ACTORS[name][2] / 'tracking')
            else:
                catalogs[name]['runs'].extend(catalog['runs'])
    result = copy.deepcopy(first)
    result.pop('personTracking', None)
    result['settings'].update(windowCount=sum(
        json.loads(Path(ref['path']).read_bytes())['windowCount'] for ref in jobs),
        globalDeadlineMs=global_deadline_ms,
        trackingHostRequestTimeoutMs=global_deadline_ms, fault='none')
    result['security'] = {'trustSchema': str(output / 'private/config/main-trust-schema.conf'),
        'trustSchemaDigest': first['security']['trustSchemaDigest'],
        'trustSchemaSource': 'maintained-fixed-root', 'credentialsPrepared': False}
    _write_uav_trust_schema(Path(result['security']['trustSchema']))
    if digest_file(Path(result['security']['trustSchema'])) != result['security']['trustSchemaDigest']:
        raise RunnerError('main sequence trust bytes differ')
    configs = {}
    for spec in result['processes']:
        name, command = spec['name'], spec['command']
        spec['privateDir'] = str(output / 'private' / TRACKING_ACTORS[name][2])
        command[command.index('--trust-schema') + 1] = result['security']['trustSchema']
        if name in catalogs:
            path = output / 'private/config' / f'main-{name}.json'
            _write_main_config(path, catalogs[name])
            command[command.index('--tracking-provider-config') + 1] = str(path)
            configs[name] = _main_asset(path)
            frozen[str(path)] = configs[name]
        elif name == 'ground-station':
            index = command.index('--tracking-job-config')
            del command[index:index + 2]
            for ref in jobs:
                command.extend(('--tracking-job-config', ref['path']))
    frozen[result['security']['trustSchema']] = _main_asset(Path(result['security']['trustSchema']))
    result['trackingMain'].update(jobs=jobs, providerConfigs=configs,
        computeWorkspace=catalogs['compute']['ownedWorkspace'],
        frozenFiles=[frozen[path] for path in sorted(frozen)],
        sourceFiles=[sources[path] for path in sorted(sources)], preparedSequence=True,
        childRunRoots=sorted(str(Path(json.loads(Path(ref['path']).read_bytes())['outputDirectory']).parent) for ref in jobs))
    _verify_main_plan(result, prepared=True)
    return result


def _mark_main_network_attempt(plan: dict[str, Any], output: Path) -> None:
    roots = [output, *map(Path, plan['trackingMain'].get('childRunRoots', []))]
    if any((root / 'main-network-attempt.json').exists() or (root / 'lifecycle.json').exists() for root in roots):
        raise RunnerError('main network attempt already exists; use fresh run/source identities')
    for root in roots:
        with (root / 'main-network-attempt.json').open('x') as stream:
            json.dump({'campaign': str(output), 'jobs': plan['trackingMain']['jobs'],
                       'startedAt': datetime.now(timezone.utc).isoformat()}, stream)


def _run_main_sequence(plan: dict[str, Any], output: Path, *, headless: bool,
                       wall_timeout_seconds: float) -> int:
    with ExitStack() as owners:
        for child in map(Path, plan['trackingMain']['childRunRoots']):
            owners.enter_context(_uav_run_ownership(child))
            source_lock = owners.enter_context((child / '.person-source.lock').open('a'))
            fcntl.flock(source_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if (child / 'person-network-attempt.json').exists():
                raise RunnerError('main sequence input already attempted person execution')
        _verify_main_plan(plan, prepared=True)
        check = preflight(plan, run=True, headless=headless)
        _write_json(output / 'preflight.json', check)
        if not check['ok']:
            return 2
        _mark_main_network_attempt(plan, output)
        plan['security'].update(_provision_run_security(output))
        _write_json(output / 'manifest.json', plan)
        code = _run_minindn(plan, output, wall_timeout_seconds=wall_timeout_seconds)
        return _collect_main_run(plan, output, code)


def preflight(plan: dict[str, Any], *, run: bool, headless: bool) -> dict[str, Any]:
    errors: list[str] = []
    try:
        _nfd_cache_packets(plan)
    except RunnerError as error:
        errors.append(str(error))
    if plan.get("applicationMode") == "main":
        try:
            _verify_main_plan(plan)
        except (OSError, ValueError, KeyError, RunnerError) as error:
            errors.append("main execution preflight failed: " + str(error))
    settings = plan.get("settings", {})
    budget = settings.get("trackingHostRequestTimeoutMs")
    if (type(budget) is not int or not 1000 < budget <= 1200000 or
            budget != settings.get("globalDeadlineMs")):
        errors.append("tracking host fence policy differs from the frozen request budget")
    device_probe = None
    resource_probe = None
    person = plan.get("personTracking")
    if person is None:
        errors.extend(_check_python_dependencies(model_dependencies=False)
                      if plan.get("applicationMode") == "main" else _check_python_dependencies())
    if person is not None or plan.get("applicationMode") == "main":
        try:
            import psutil
            resource_probe = {"version": psutil.__version__, "path": psutil.__file__,
                              "launcherPython": sys.executable,
                              "launcherRssBytes": psutil.Process().memory_info().rss}
        except Exception as error:
            errors.append(f"person resource preflight failed: {type(error).__name__}: {error}")
    if person is not None:
        try:
            _verify_person_plan(plan)
            env = dict(os.environ, **_person_application_environment(
                Path(person["preparationReceipt"]).parent))
            probe = subprocess.run(
                [person["workerPython"], "-c",
                 "import sys,json,torch,ultralytics,lap,torchreid; d=sys.argv[1]; "
                 "t=torch.empty(1,device=d); assert str(t.device)==d; "
                 "print('SPEC219_DEVICE_PROBE '+json.dumps(dict(schema='uav-person-device-probe-v1',device=str(t.device))))",
                 person["requestedDevice"]],
                env=env, capture_output=True, timeout=15, check=False)
            device_probe = {"returncode": probe.returncode,
                            "stdout": probe.stdout[:8192].decode(errors="replace"),
                            "stderr": probe.stderr[:8192].decode(errors="replace")}
            if probe.returncode:
                raise RunnerError("backend import/device probe exited " + str(probe.returncode))
            prefix = b"SPEC219_DEVICE_PROBE "
            records = [line[len(prefix):] for line in probe.stdout.splitlines() if line.startswith(prefix)]
            if len(probe.stdout) > 64 * 1024 or len(records) != 1 or json.loads(records[0]) != {
                    "schema": "uav-person-device-probe-v1", "device": person["requestedDevice"]}:
                raise RunnerError("backend device probe has missing, duplicate or mismatched result")
        except (OSError, ValueError, RunnerError, subprocess.TimeoutExpired) as error:
            errors.append("person execution preflight failed: " + str(error))
    if plan.get("applicationMode") == "main":
        try:
            catalog = json.loads(Path(plan["trackingMain"]["providerConfigs"]["compute"]["path"]).read_bytes())
            for candidate in catalog["runs"]:
                script = ("import torch,ultralytics,lap; "
                          "assert str(torch.empty(1,device='cpu').device)=='cpu'; ")
                if candidate["mode"] == "person":
                    script += "import torchreid; "
                if not headless:
                    if not os.environ.get("DISPLAY"):
                        raise RunnerError("main GTK screenshots require an explicit X11 DISPLAY")
                    script += ("from PIL import ImageGrab; import os; "
                               "im=ImageGrab.grab(xdisplay=os.environ['DISPLAY']); assert im.width>0; ")
                probe = subprocess.run([candidate["workerPython"]["path"], "-c", script],
                    env=dict(os.environ, **_person_application_environment(Path(plan["processes"][0]["privateDir"]).parents[1])),
                    capture_output=True, timeout=20)
                if probe.returncode:
                    raise RunnerError("main worker/device/X11 probe failed: " + probe.stderr[:1024].decode(errors="replace"))
        except (OSError, ValueError, KeyError, RunnerError, subprocess.TimeoutExpired) as error:
            errors.append("main execution preflight failed: " + str(error))
    observation = plan.get("observation", {})
    if (observation.get("enabled") and
            observation.get("source", {}).get("kind") == "external-fd" and
            observation.get("source", {}).get("writerEndpoints", {}).get("writerOwner") !=
            "runner-source-adapter"):
        errors.append("external-fd run requires --external-source-adapter to provide FIFO writers")
    if run:
        try:
            import minindn  # noqa: F401
        except Exception as error:  # pragma: no cover - host dependent
            errors.append(f"MiniNDN import unavailable: {type(error).__name__}: {error}")
        errors.extend(
            f"missing installed native binary/program: {name}"
            for name in _required_native_programs(plan.get("applicationMode", "compat"))
            if shutil.which(name) is None)
        errors.extend(
            f"missing installed tracking script: {path}"
            for path in (_installed_tracking_script("tracking_worker.py"),
                         _installed_tracking_script("tracking_display.py"),
                         _installed_tracking_script("motion_estimator.py"),
                         _installed_tracking_script("motion_schema.py"),
                         _installed_tracking_script("observation_schema.py"),
                         _installed_tracking_script("observation_source.py"))
            if not path.is_file() and not (plan.get("applicationMode") == "main" and
                                          path.name == "tracking_display.py"))
        if observation.get("enabled") and observation.get("mode") != "live-push" and \
                shutil.which("ffprobe") is None:
            errors.append("ffprobe is required to bind decoded replay frames to presentation timestamps")
        if os.geteuid() != 0:
            errors.append("MiniNDN run requires root privileges")
        if not headless and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
            errors.append("windows display requested but DISPLAY/WAYLAND_DISPLAY is unavailable")
    return {
        "schema": "spec191-uav-preflight/v1",
        "ok": not errors,
        "errors": errors,
        "run": run,
        "display": "headless" if headless else "windows",
        "nodes": plan["topology"]["nodes"],
        "processCount": len(plan["processes"]),
        **({"personDeviceProbe": device_probe} if person is not None else {}),
        **({"personResourceProbe": resource_probe} if person is not None else {}),
    }


class ProcessSupervisor:
    """Own child processes and guarantee bounded, run-scoped cleanup."""

    def __init__(self, output: Path, terminate_timeout: float = 5.0) -> None:
        self.output = output
        self.terminate_timeout = terminate_timeout
        self.processes: list[tuple[str, subprocess.Popen[str], Any]] = []

    def start(self, spec: ProcessSpec, env: dict[str, str]) -> subprocess.Popen[str]:
        log = (self.output / f"{spec.name}.log").open("w", encoding="utf-8")
        cwd = spec.cwd.resolve()
        command = list(spec.command)
        executable = shutil.which(command[0], path=env.get("PATH"))
        if executable is None:
            log.close()
            raise RunnerError(f"executable not found for {spec.name}: {command[0]}")
        command[0] = executable
        child_env = dict(env)
        if Path(command[0]).name == "UavTrackingNode":
            budget = (command[command.index("--request-timeout-ms") + 1]
                      if "--request-timeout-ms" in command else "30000")
            child_env["NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS"] = budget
        spec.private_dir.mkdir(parents=True, exist_ok=True)
        # Keep process-owned configuration/cache roots separate even when the
        # Controller and GS share the same MiniNDN node.  The native apps may
        # additionally use their own key-chain flags; these environment roots
        # are the launcher-level isolation floor and are recorded in the
        # manifest for post-run verification.
        child_env["NDNSF_SPEC191_PRIVATE_DIR"] = str(spec.private_dir)
        child_env["HOME"] = str(spec.private_dir / "home")
        child_env["XDG_CONFIG_HOME"] = str(spec.private_dir / "config")
        child_env["XDG_DATA_HOME"] = str(spec.private_dir / "data")
        child_env["XDG_CACHE_HOME"] = str(spec.private_dir / "cache")
        for name in ("home", "config", "data", "cache"):
            (spec.private_dir / name).mkdir(parents=True, exist_ok=True)
        process = subprocess.Popen(command, cwd=str(cwd), env=child_env, stdout=log,
                                   stderr=subprocess.STDOUT, text=True,
                                   start_new_session=True)
        self.processes.append((spec.name, process, log))
        return process

    def stop_all(self) -> None:
        for _name, process, _log in reversed(self.processes):
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        deadline = time.monotonic() + self.terminate_timeout
        while time.monotonic() < deadline and any(p.poll() is None for _, p, _ in self.processes):
            time.sleep(0.05)
        for _name, process, _log in reversed(self.processes):
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        for _name, _process, log in self.processes:
            log.close()


def _read_main_status(path: Path) -> dict[str, Any] | None:
    if not path.exists():
        return None
    with path.open('rb') as stream:
        raw = stream.read(65537)
    if not raw or len(raw) > 65536:
        raise RunnerError('main status artifact exceeds its native bound')
    value = json.loads(raw)
    if value.get('schema') != 'uav-main-tracking-job-v1':
        raise RunnerError('main status schema mismatch')
    return value


def _main_frame_shown(log: str, run_id: str, window: int, digest: str) -> bool:
    return all(f'SPEC228_TRACKING_FRAME_SHOWN run={run_id} window={window} camera=UAV{i} result={digest}'
               in log for i in range(1, 4))


def _capture_main_screen(plan: dict[str, Any], output: Path, job: dict[str, Any],
                         status: dict[str, Any], label: str) -> dict[str, Any] | None:
    main = plan['trackingMain']
    status_path = Path(job['outputDirectory']) / 'tracking-status.json'
    keys = ('runId', 'generation', 'state', 'verifiedResultWindows', 'lastResultDigest')
    before = _read_main_status(status_path)
    if before is None or any(before.get(key) != status.get(key) for key in keys):
        return None
    compute = json.loads(Path(main['providerConfigs']['compute']['path']).read_bytes())
    run = next(value for value in compute['runs'] if value['runId'] == job['runId'])
    python = run['workerPython']['path']
    directory = output / 'screenshots'
    directory.mkdir(exist_ok=True)
    candidate = directory / '.main-capture.png'
    script = ('from PIL import ImageGrab; import sys; '
              'im=ImageGrab.grab(xdisplay=sys.argv[1]); im.save(sys.argv[2],format="PNG")')
    env = dict(os.environ, **_person_application_environment(output))
    capture = subprocess.run([python, '-c', script, os.environ.get('DISPLAY', ''), str(candidate)],
                             env=env, capture_output=True, timeout=10)
    if capture.returncode:
        raise RunnerError('actual main X11 screenshot failed: ' + capture.stderr[:1024].decode(errors='replace'))
    after = _read_main_status(status_path)
    if after is None or any(after.get(key) != before.get(key) for key in keys):
        candidate.unlink(missing_ok=True)
        return None
    path = directory / f'{job["runId"]}-{label}.png'
    if path.exists():
        raise RunnerError('preserve existing main screenshot')
    os.replace(candidate, path)
    return {'path': str(path), 'digest': digest_file(path), 'label': label,
            'runId': job['runId'], 'generation': status['generation'],
            'windowIndex': status['verifiedResultWindows'] - 1,
            'resultDigest': status['lastResultDigest'], 'nativeState': status['state'],
            'capture': 'raw X11 screen from actual main GTK; no offline overlay or image editing'}


def _observe_main_jobs(plan: dict[str, Any], output: Path, processes: list,
                       run_deadline: float, resource_sampler: Any) -> int:
    main = plan['trackingMain']
    jobs = [json.loads(Path(item['path']).read_bytes()) for item in main['jobs']]
    screenshots, observations = [], []
    captured = set()
    capture_attempts = {}
    finished = set()
    last_state = {}
    while time.monotonic() < run_deadline:
        if resource_sampler is not None:
            resource_sampler.sample()
        log_path = output / 'ground-station.log'
        with log_path.open('rb') as stream:
            stream.seek(max(0, log_path.stat().st_size - 256 * 1024))
            log = stream.read(256 * 1024).decode(errors='replace')
        failed = False
        for index, job in enumerate(jobs):
            status = _read_main_status(Path(job['outputDirectory']) / 'tracking-status.json')
            if status is None:
                continue
            if status['runId'] != job['runId'] or status['jobId'] != job['jobId']:
                raise RunnerError('main status does not belong to the prepared job')
            state_key = (status['state'], status['cleanupState'])
            if last_state.get(job['runId']) != state_key:
                observations.append({'runId': job['runId'], 'state': status['state'],
                    'cleanupState': status['cleanupState'],
                    'nativeUpdatedMonotonicMs': status['updatedMonotonicMs'],
                    'launcherObservedNs': time.monotonic_ns()})
                last_state[job['runId']] = state_key
            completed = status['verifiedResultWindows']
            shown = completed > 0 and _main_frame_shown(
                log, job['runId'], completed - 1, status['lastResultDigest'])
            if (not main['headless'] and len(jobs) == 1 and status['state'] == 'RUNNING' and
                    shown and (job['runId'], 'running') not in captured):
                raw = json.loads((Path(job['outputDirectory']) / 'results' /
                                  f'window-{completed - 1}.json').read_bytes())
                if any(frame['tracks'] for frame in raw['frames']):
                    key = (job['runId'], 'running')
                    capture_attempts[key] = capture_attempts.get(key, 0) + 1
                    if capture_attempts[key] > 3:
                        raise RunnerError('main running screenshot changed across three bounded captures')
                    shot = _capture_main_screen(plan, output, job, status, 'running')
                    if shot:
                        screenshots.append(shot)
                        captured.add((job['runId'], 'running'))
            if (not main['headless'] and len(jobs) == 1 and job['mode'] == 'person' and shown and
                    status['state'] in ('RUNNING', 'COMPLETED') and
                    (job['runId'], 'cross-camera-witness') not in captured):
                from person_run_receipt import cross_camera_matches
                raw = json.loads((Path(job['outputDirectory']) / 'results' /
                                  f'window-{completed - 1}.json').read_bytes())
                if cross_camera_matches(raw):
                    key = (job['runId'], 'cross-camera-witness')
                    capture_attempts[key] = capture_attempts.get(key, 0) + 1
                    if capture_attempts[key] > 3:
                        raise RunnerError('main witness changed across three bounded captures')
                    shot = _capture_main_screen(plan, output, job, status, 'cross-camera-witness')
                    if shot:
                        screenshots.append(shot)
                        captured.add(key)
            if status['state'] in ('FAILED', 'CANCELLED') or status.get('ordinaryTelemetryFailed') is True:
                failed = True
            if (status['state'] == 'COMPLETED' and status['cleanupState'] == 'CLEAN' and
                    status.get('ordinaryTelemetryComplete') is True and
                    status.get('ordinaryTelemetryFailed') is False):
                if completed != job['windowCount']:
                    raise RunnerError('main completed with an incomplete verified result count')
                if main['headless'] or shown:
                    finished.add(job['runId'])
                    if (not main['headless'] and index == len(jobs) - 1 and
                            (job['runId'], 'terminal') not in captured):
                        key = (job['runId'], 'terminal')
                        capture_attempts[key] = capture_attempts.get(key, 0) + 1
                        if capture_attempts[key] > 3:
                            raise RunnerError('main terminal screenshot changed across three bounded captures')
                        shot = _capture_main_screen(plan, output, job, status, 'terminal')
                        if shot:
                            screenshots.append(shot)
                            captured.add((job['runId'], 'terminal'))
        _write_json(output / 'main-observations.json', {'states': observations, 'screenshots': screenshots})
        if failed:
            _write_json(output / 'lifecycle.json', {'schema': 'spec228-main-lifecycle-v1',
                'terminal': True, 'outcome': 'main-job-failed-or-cancelled'})
            return 1
        if len(finished) == len(jobs):
            if not main['headless'] and (jobs[-1]['runId'], 'terminal') not in captured:
                time.sleep(.05)
                continue
            _write_json(output / 'lifecycle.json', {'schema': 'spec228-main-lifecycle-v1',
                'terminal': True, 'outcome': 'main-jobs-completed', 'completedRunIds': sorted(finished)})
            return 0
        for name, process, _ in processes:
            if name == 'ground-station' and process.poll() is not None:
                raise RunnerError('main GS exited before its native jobs completed')
        time.sleep(.05)
    raise RunnerError('main native jobs exceeded the frozen overall deadline')



def _run_minindn(plan: dict[str, Any], output: Path, *, wall_timeout_seconds: float) -> int:
    """Run the exact five-node topology through MiniNDN/NFD.

    The host launcher is intentionally not a qualification path: it cannot
    prove that the NDN faces, routes, or process-local private roots are wired
    correctly.  This function fails closed when MiniNDN cannot be imported or
    a required readiness/result marker is absent.
    """
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn
    from minindn.util import getPopen

    output.mkdir(parents=True, exist_ok=True)
    saved_argv = sys.argv
    sys.argv = [saved_argv[0]]
    try:
        with _minindn_space_delimiter_compat():
            ndn = Minindn(topoFile=str(TOPOLOGY), workDir=str(output / "minindn"),
                          controller=None)
    finally:
        sys.argv = saved_argv

    processes: list[tuple[str, Any, Any]] = []
    nfd_apps: list[Any] = []
    started = time.monotonic()
    started_ns = time.monotonic_ns()
    person_mode = "personTracking" in plan
    main_mode = plan.get("applicationMode") == "main"
    resource_sampler = None
    if person_mode or main_mode:
        from person_run_resources import PersonRunResources
        resource_sampler = PersonRunResources(output.name,
            plan["personTracking"]["requestedDevice"] if person_mode else "cpu",
            sample_period_ns=200000000 if main_mode else 100000000)
    socket_root = _uav_socket_root(output)
    nfd_sockets = {
        node_name: _uav_unix_socket_path(socket_root, node_name)
        for node_name in plan["topology"]["nodes"]
    }
    _uav_unix_socket_path(socket_root, "display")
    try:
        socket_root.mkdir(mode=0o700)
    except FileExistsError as error:
        raise RunnerError("MiniNDN socket root already exists; run output cannot be reused") from error
    display_name = ("ground-station" if main_mode else
                    "ground-station-display" if person_mode else "compute-display")
    run_deadline = started + wall_timeout_seconds

    def launch(spec: ProcessSpec) -> Any:
        node = ndn.net[spec.node]
        log_path = output / f"{spec.name}.log"
        log = log_path.open("wb")
        private = spec.private_dir.resolve()
        shared_keychain = (output / "private" / "keychain").resolve()
        generation_state = (output / "private" / "controller" /
                            "controller-generation.state").resolve()
        mpl_config = private / "mpl"
        yolo_config = private / "config" / "Ultralytics"
        for name in ("home", "config", "data", "cache", "pib", "tpm"):
            (private / name).mkdir(parents=True, exist_ok=True)
        mpl_config.mkdir(parents=True, exist_ok=True)
        yolo_config.mkdir(parents=True, exist_ok=True)
        for name in ("pib", "tpm"):
            (shared_keychain / name).mkdir(parents=True, exist_ok=True)
        socket = nfd_sockets[node.name]
        child_ndn_log = os.environ.get("SPEC191_CHILD_NDN_LOG", "*=WARN").strip() or "*=WARN"
        child_pythonpath = str(DEFAULT_TRACKING_LIBEXEC) if person_mode or main_mode else _child_pythonpath()
        env = [
            f"export NDNSF_SPEC191_PRIVATE_DIR={shlex.quote(str(private))}",
            f"export HOME={shlex.quote(str(private / 'home'))}",
            f"export XDG_CONFIG_HOME={shlex.quote(str(private / 'config'))}",
            f"export XDG_DATA_HOME={shlex.quote(str(private / 'data'))}",
            f"export XDG_CACHE_HOME={shlex.quote(str(private / 'cache'))}",
            f"export MPLCONFIGDIR={shlex.quote(str(mpl_config))}",
            f"export YOLO_CONFIG_DIR={shlex.quote(str(yolo_config))}",
            # A campaign-scoped keychain is required for cross-process
            # certificate resolution. Application scratch remains process
            # private; this does not merge MiniNDN nodes or NFD transports.
            f"export NDN_CLIENT_PIB={shlex.quote('pib-sqlite3:' + str(shared_keychain / 'pib'))}",
            f"export NDN_CLIENT_TPM={shlex.quote('tpm-file:' + str(shared_keychain / 'tpm'))}",
            # Do not reuse ServiceController's process-global /tmp state lock
            # across campaigns. The generation lease is campaign-scoped and
            # therefore remains auditable and cannot be poisoned by an older
            # interrupted MiniNDN run.
            f"export NDNSF_CONTROLLER_GENERATION_STATE={shlex.quote(str(generation_state))}",
            f"export NDN_CLIENT_TRANSPORT={shlex.quote('unix://' + str(socket))}",
            # Let installed ELF paths select their SDKs. A forced global
            # path could shadow the selected NAC SDK with an older library.
            # Retain the explicit application override for installed closures.
            _application_library_environment_command(),
            # Keep application logging separate from MiniNDN/NFD startup. A
            # global NDN_LOG would be inherited by NFD and can make NFD reject
            # an application logger selector as malformed configuration.
            f"export NDN_LOG={shlex.quote(child_ndn_log)}",
            f"export NDNSF_SPEC191_RUN_ROOT={shlex.quote(str(output))}",
        ]
        if spec.name in {"compute", "uav1-camera", "uav2-camera", "uav3-camera"}:
            env.append("export NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS=" +
                       str(plan["settings"]["trackingHostRequestTimeoutMs"]))
        if main_mode:
            # Dedicated replay actors own no desktop bus/portal daemons. Keep GTK
            # local to the explicitly supplied X11 display and local config files.
            env.extend([
                "export DBUS_SESSION_BUS_ADDRESS=" + shlex.quote(
                    "unix:path=" + str(output / "private/no-desktop-session-bus")),
                "export NO_AT_BRIDGE=1", "export GTK_USE_PORTAL=0",
                "export GSETTINGS_BACKEND=memory", "export GIO_USE_VFS=local",
                "export GIO_USE_VOLUME_MONITOR=unix",
            ])
        if plan["settings"]["fault"] in {
                "user-cancel-terminal-response-race",
                "user-cancel-terminal-response-cancel-wins-race",
                "user-cancel-before-person-commit"}:
            # Keep the deterministic callback hold off the Face loop itself.
            env.append("export NDNSF_HANDLER_THREADS=2")
        if child_pythonpath:
            env.append(f"export PYTHONPATH={shlex.quote(child_pythonpath)}")
        if person_mode or main_mode:
            env.append("export PYTHONNOUSERSITE=1")
            if spec.name == "compute":
                env.extend(f"export {key}={shlex.quote(value)}"
                           for key, value in _person_application_environment(output).items())
                if plan["settings"]["fault"] == "user-cancel-during-person-worker":
                    gate = output / "private/spec219-person-worker-cancel"
                    env.append("export NDNSF_SPEC219_PERSON_WORKER_CANCEL_GATE=" +
                               shlex.quote(str(gate.resolve())))
        command = "; ".join(env + [
            f"cd {shlex.quote(str(spec.cwd.resolve()))}",
            f"exec {shlex.join(list(spec.command))}",
        ])
        process = getPopen(node, command, shell=True, stdout=log, stderr=subprocess.STDOUT)
        processes.append((spec.name, process, log))
        if resource_sampler is not None:
            resource_sampler.register(spec.name, process.pid)
            resource_sampler.sample(force=True)
        return process

    def wait_for(name: str, markers: tuple[str, ...], timeout: float,
                 *, same_line: bool = False) -> str:
        path = output / f"{name}.log"
        deadline = time.monotonic() + timeout
        if person_mode or main_mode:
            deadline = min(deadline, run_deadline)
        while time.monotonic() < deadline:
            if resource_sampler is not None:
                resource_sampler.sample()
            text = path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""
            if same_line:
                if _has_all_markers_on_one_line(text, markers):
                    return markers[-1]
            else:
                for marker in markers:
                    if marker in text:
                        return marker
            for process_name, process, _log in processes:
                if process_name == name and process.poll() is not None:
                    raise RunnerError(
                        f"{name} exited before readiness marker; returncode={process.returncode}")
                if name != display_name and process_name == display_name and \
                        process.poll() is not None:
                    raise RunnerError(
                        f"{display_name} exited before campaign completion; "
                        f"returncode={process.returncode}")
            time.sleep(0.1)
        raise RunnerError(f"{name} did not emit required marker(s) {markers!r}")

    def wait_for_control_file(path: Path, timeout: float) -> None:
        deadline = min(run_deadline, time.monotonic() + min(timeout, 30.0))
        while time.monotonic() < deadline:
            try:
                info = path.lstat()
            except FileNotFoundError:
                info = None
            if info is not None:
                if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid():
                    raise RunnerError(f"test gate is not an owned regular file: {path.name}")
                return
            for process_name, process, _log in processes:
                if process_name in ("compute", "ground-station") and process.poll() is not None:
                    raise RunnerError(
                        f"{process_name} exited while waiting for test gate {path.name}")
            time.sleep(0.05)
        raise RunnerError(f"bounded test gate did not open: {path.name}")

    def read_proc_record(pid: int) -> dict[str, Any] | None:
        proc = Path("/proc") / str(pid)
        try:
            stat_text = (proc / "stat").read_text(encoding="ascii")
            fields = stat_text[stat_text.rfind(")") + 2:].split()
            if len(fields) <= 19:
                return None
            argv = [part.decode("utf-8", errors="replace")
                    for part in (proc / "cmdline").read_bytes().split(b"\0") if part]
            cgroups = {}
            for line in (proc / "cgroup").read_text(encoding="ascii").splitlines():
                hierarchy, controllers, path = line.split(":", 2)
                cgroups[(hierarchy, controllers)] = path
            return {"state": fields[0], "ppid": int(fields[1]),
                    "startTimeTicks": int(fields[19]), "argv": argv, "cgroups": cgroups}
        except (OSError, ValueError, IndexError):
            return None

    def wait_for_person_worker_exit(pid: int, start_time_ticks: int) -> str:
        deadline = min(run_deadline, time.monotonic() + 5.0)
        last_state = "missing"
        while time.monotonic() < deadline:
            record = read_proc_record(pid)
            if record is None or record["startTimeTicks"] != start_time_ticks:
                return last_state
            last_state = record["state"]
            if last_state == "Z":
                return last_state
            time.sleep(0.05)
        raise RunnerError("cancelled person worker process remained alive")

    def configure_ndn() -> None:
        ndn.start()
        app_nodes = list(ndn.net.hosts)
        if {node.name for node in app_nodes} != set(plan["topology"]["nodes"]):
            raise RunnerError("MiniNDN host set does not match the five-node manifest")
        nfd_configs = {}
        cache_packets = _nfd_cache_packets(plan)
        for node in app_nodes:
            nfd = Nfd(node, csSize=cache_packets)
            old_socket = f"/run/nfd/{node.name}.sock"
            new_socket = str(nfd_sockets[node.name])
            conf_path = Path(nfd.confFile)
            conf_path.write_text(conf_path.read_text(encoding="utf-8").replace(
                old_socket, new_socket), encoding="utf-8")
            actual_cache = re.findall(r"^\s*cs_max_packets\s+([0-9]+)\b",
                                      conf_path.read_text(encoding="utf-8"), re.MULTILINE)
            if actual_cache != [str(cache_packets)]:
                raise RunnerError("generated NFD cache configuration differs from the frozen profile")
            nfd_configs[node.name] = {**_main_asset(conf_path), "csMaxPackets": cache_packets}
            Path(nfd.clientConf).write_text(f"transport=unix://{new_socket}\n",
                                            encoding="utf-8")
            nfd_apps.append(nfd)
            nfd.start()
            if resource_sampler is not None:
                resource_sampler.register("nfd-" + node.name, nfd.process.pid)
                resource_sampler.sample(force=True)
        _write_json(output / "network-settings.json", {
            "schema": "spec228-uav-ndn-network-v1", "nfdCsMaxPackets": cache_packets,
            "nodes": nfd_configs})
        for node in app_nodes:
            socket = nfd_sockets[node.name]
            deadline = time.monotonic() + 10
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            if not socket.exists():
                raise RunnerError(f"NFD socket not ready: {socket}")
        time.sleep(1.0)
        created_faces = ndn.setupFaces()
        face_target_ips = {
            (node.name, peer_name): peer_ip
            for node, links in created_faces.items()
            for peer_name, peer_ip, _cost in links
        }
        next_hops = {
            "uav1": ["compute"], "uav2": ["compute"], "uav3": ["compute"],
            "compute": ["uav1", "uav2", "uav3", "gs"], "gs": ["compute"],
        }
        route_records = []
        for node in app_nodes:
            transport = f"unix://{nfd_sockets[node.name]}"
            for peer_name in next_hops[node.name]:
                peer_ip = face_target_ips[(node.name, peer_name)]
                face_list = node.cmd(f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)} nfdc face list")
                match = re.search(
                    rf"faceid=(\d+)\s+remote=(?:udp|udp4)://{re.escape(peer_ip)}:6363",
                    face_list)
                if match is None:
                    raise RunnerError(
                        f"NFD face missing node={node.name} peer={peer_name}: {face_list!r}")
                face_id = match.group(1)
                command = (f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)} nfdc route add "
                           f"/example/uav nexthop {face_id}")
                route_records.append({"node": node.name, "peer": peer_name,
                                      "faceId": int(face_id), "command": command,
                                      "result": node.cmd(command)})
            strategy = (f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)} nfdc strategy set "
                        "/example/uav /localhost/nfd/strategy/multicast")
            route_records.append({"node": node.name, "peer": None, "command": strategy,
                                  "result": node.cmd(strategy)})
        _write_json(output / "routes.json", {"schema": "spec191-routes/v1",
                                              "routes": route_records})

    try:
        configure_ndn()
        terminal_race_gate = None
        person_worker_cancel_gate = None
        if plan["settings"]["fault"] in {
                "user-cancel-terminal-response-race",
                "user-cancel-terminal-response-cancel-wins-race"}:
            terminal_race_gate = output / "private/spec210-terminal-response-race"
            terminal_race_gate.mkdir(parents=True, exist_ok=True)
            for gate_name in ("release-terminal", "release-ack",
                              "release-response-callback", "transaction-snapshot-ready",
                              "release-cancel", "cancel-sent"):
                (terminal_race_gate / gate_name).unlink(missing_ok=True)
        if plan["settings"]["fault"] == "user-cancel-during-person-worker":
            person_worker_cancel_gate = output / "private/spec219-person-worker-cancel"
            person_worker_cancel_gate.mkdir(parents=True, mode=0o700)
        specs = [ProcessSpec(item["name"], item["node"], item["identity"],
                             Path(item["cwd"]), tuple(item["command"]),
                             Path(item["privateDir"])) for item in plan["processes"]]
        by_name = {spec.name: spec for spec in specs}
        launch(by_name["controller"])
        # The process-start marker is not sufficient: App_ServiceController
        # publishes NDNSF_CONTROLLER_READY only after prefix registration and
        # its PUBPARAMS readiness barrier have completed.
        wait_for("controller", ("NDNSF_CONTROLLER_READY",), 45)
        observation = plan.get("observation", {})
        generation_ready_marker = _source_generation_ready_marker(observation)
        pusher_names = sorted(name for name in by_name if name.startswith("observation-push-"))
        for name in pusher_names:
            launch(by_name[name])
            markers = (("UAV_OBSERVATION_PUSHER_READY",) if generation_ready_marker is None else
                       ("UAV_OBSERVATION_PUSHER_READY", generation_ready_marker))
            wait_for(name, markers, 30, same_line=generation_ready_marker is not None)
        external_source_names = sorted(
            name for name in by_name if name.startswith("external-source-"))
        for name in external_source_names:
            launch(by_name[name])
            markers = (("UAV_OBSERVATION_SOURCE_ADAPTER_READY",) if generation_ready_marker is None else
                       ("UAV_OBSERVATION_SOURCE_ADAPTER_READY", generation_ready_marker))
            wait_for(name, markers, 30, same_line=generation_ready_marker is not None)
        for name in ("uav1-camera", "uav2-camera", "uav3-camera", "compute"):
            launch(by_name[name])
        for name in ("uav1-camera", "uav2-camera", "uav3-camera", "compute"):
            wait_for(name, ("DRONE_HEADLESS_READY" if main_mode else "SPEC191_TRACKING_NODE_READY",), 60)
        if not person_mode and not main_mode:
            launch(by_name[display_name])
            wait_for(display_name, ("DISPLAY_READY",), 30)
        launch(by_name["ground-station"])
        ready = ("GS_GUI_READY" if plan["mode"] == "windows" else "SPEC191_TRACKING_GS_REQUEST request=") \
            if main_mode else "SPEC191_TRACKING_GS_READY"
        wait_for("ground-station", (ready,), 30)
        if main_mode:
            return _observe_main_jobs(plan, output, processes, run_deadline, resource_sampler)
        if person_mode:
            launch(by_name[display_name])
            wait_for(display_name, ("DISPLAY_READY",), 30)
        if plan["settings"]["fault"] == "user-cancel-before-person-commit":
            wait_for("compute", ("SPEC219_PERSON_PRECOMMIT_READY window=window-0",),
                     wall_timeout_seconds)
            ground_station_process = next(
                process for name, process, _log in processes if name == "ground-station")
            ground_station_process.send_signal(signal.SIGUSR1)
            for marker in (
                "SPEC210_USER_CANCEL_ACK_QUEUED window=window-0 authority_revoked=true",
                "SPEC210_USER_CANCEL_CANDIDATE_DISCARDED window=window-0",
                "SPEC210_USER_CANCEL_ROLE_CLOSED",
            ):
                wait_for("compute", (marker,), wall_timeout_seconds)
            for marker in (
                "SPEC210_CANCEL_ACK_CONFIRMED window=0",
                "SPEC191_TRACKING_CANCELLED window=0 accepted=true",
            ):
                wait_for("ground-station", (marker,), wall_timeout_seconds)
        if plan["settings"]["fault"] == "user-cancel-during-person-worker":
            if person_worker_cancel_gate is None:
                raise RunnerError("person worker cancellation gate was not initialized")
            active_path = person_worker_cancel_gate / "detector-inference-active.json"
            wait_for_control_file(active_path, wall_timeout_seconds)
            with active_path.open("rb") as stream:
                active_payload = stream.read(4097)
            if not active_payload or len(active_payload) > 4096:
                raise RunnerError("person worker active marker exceeds byte bound")
            active = json.loads(active_payload)
            expected_fields = {"schema", "runId", "windowId", "sequence", "cameraId",
                               "phase", "pid", "startTimeTicks"}
            if (not isinstance(active, dict) or set(active) != expected_fields or
                    active.get("schema") != "spec219-person-worker-active-v1" or
                    active.get("runId") != output.name or active.get("windowId") != "window-0" or
                    active.get("sequence") != 1 or active.get("cameraId") != "UAV1" or
                    active.get("phase") != "after-first-detector-inference" or
                    type(active.get("pid")) is not int or active["pid"] <= 1 or
                    type(active.get("startTimeTicks")) is not int or
                    active["startTimeTicks"] <= 0):
                raise RunnerError("person worker active marker binding mismatch")
            worker_script = by_name["compute"].command[
                by_name["compute"].command.index("--worker-script") + 1]
            compute_process = next(process for name, process, _log in processes
                                   if name == "compute")
            worker = read_proc_record(active["pid"])
            compute = read_proc_record(compute_process.pid)
            worker_bindings = {}
            if worker is not None:
                for option in ("--run-id", "--mission-id", "--window-id", "--sequence"):
                    if option in worker["argv"]:
                        index = worker["argv"].index(option)
                        value = worker["argv"][index + 1] if index + 1 < len(worker["argv"]) else None
                        worker_bindings[option] = value
            compute_cgroup = None if compute is None else compute["cgroups"].get(("0", ""))
            worker_cgroup = None if worker is None else worker["cgroups"].get(("0", ""))
            if (worker is None or compute is None or worker["state"] in ("Z", "X") or
                    worker["startTimeTicks"] != active["startTimeTicks"] or
                    worker["argv"].count(worker_script) != 1 or
                    "--person-session" not in worker["argv"] or
                    worker_bindings != {"--run-id": output.name,
                                        "--mission-id": output.name,
                                        "--window-id": "window-0", "--sequence": "1"} or
                    not compute_cgroup or worker_cgroup != compute_cgroup):
                raise RunnerError("active person worker PID, command or cgroup mismatch")
            if "SPEC219_PERSON_SESSION_DONE jobId=" in \
                    (output / "compute.log").read_text(encoding="utf-8", errors="replace"):
                raise RunnerError("person worker completed before cancellation was armed")
            (person_worker_cancel_gate / "cancel-ready").touch(exist_ok=False)
            wait_for_control_file(person_worker_cancel_gate / "release-cancel", 30.0)
            current_worker = read_proc_record(active["pid"])
            if (current_worker is None or current_worker["startTimeTicks"] !=
                    active["startTimeTicks"] or current_worker["state"] in ("Z", "X") or
                    "SPEC219_PERSON_SESSION_DONE jobId=" in
                    (output / "compute.log").read_text(encoding="utf-8", errors="replace")):
                raise RunnerError("person worker left its active inference gate before cancel")
            ground_station_process = next(
                process for name, process, _log in processes if name == "ground-station")
            ground_station_process.send_signal(signal.SIGUSR1)
            (person_worker_cancel_gate / "cancel-sent").touch(exist_ok=False)
            for marker in (
                "SPEC210_USER_CANCEL_ACK_QUEUED window=window-0 authority_revoked=true",
                "SPEC210_USER_CANCEL_ROLE_CLOSED",
            ):
                wait_for("compute", (marker,), wall_timeout_seconds)
            for marker in (
                "SPEC210_CANCEL_ACK_CONFIRMED window=0",
                "SPEC191_TRACKING_CANCELLED window=0 accepted=true",
            ):
                wait_for("ground-station", (marker,), wall_timeout_seconds)
            worker_exit_state = wait_for_person_worker_exit(
                active["pid"], active["startTimeTicks"])
            _write_json(output / "person-worker-cancel-runtime.json", {
                "schema": "spec219-person-worker-cancel-runtime-v1",
                "workerPid": active["pid"], "workerStartTimeTicks": active["startTimeTicks"],
                "workerExitState": worker_exit_state,
                "workerExitedBeforeRunnerCleanup": True,
                "cancelSignalSent": (person_worker_cancel_gate / "cancel-sent").is_file(),
            })
        if terminal_race_gate is not None:
            cancellation_wins_race = (
                plan["settings"]["fault"] ==
                "user-cancel-terminal-response-cancel-wins-race")
            wait_for("compute", (
                "SPEC210_TERMINAL_RESPONSE_READY window=window-0" if cancellation_wins_race else
                "SPEC210_TERMINAL_RESPONSE_LOCKED window=window-0",
            ), wall_timeout_seconds)
            if cancellation_wins_race:
                (terminal_race_gate / "transaction-snapshot-ready").touch(exist_ok=False)
                wait_for_control_file(terminal_race_gate / "release-cancel", 30.0)
            ground_station_process = next(
                process for name, process, _log in processes if name == "ground-station")
            ground_station_process.send_signal(signal.SIGUSR1)
            if cancellation_wins_race:
                (terminal_race_gate / "cancel-sent").touch(exist_ok=False)
            wait_for("ground-station", (
                "SPEC210_CANCEL_NOTICE_QUEUED window=0 accepted=true",
            ), wall_timeout_seconds)
            wait_for("compute", (
                "SPEC210_USER_CANCEL_WAITING_FOR_TERMINAL_LOCK window=window-0",
            ), wall_timeout_seconds)
            if cancellation_wins_race:
                wait_for("compute", ("SPEC210_USER_CANCEL_ACK_HELD window=window-0",),
                         wall_timeout_seconds)
                (terminal_race_gate / "release-terminal").touch()
                wait_for("compute", ("SPEC210_USER_CANCEL_ROLE_CLOSED",),
                         wall_timeout_seconds)
                (terminal_race_gate / "release-ack").touch()
                wait_for("ground-station", (
                    "SPEC210_CANCEL_ACK_CONFIRMED window=0",
                    "SPEC191_TRACKING_CANCELLED window=0 accepted=true",
                ), wall_timeout_seconds)
            else:
                (terminal_race_gate / "release-terminal").touch()
                wait_for("compute", ("SPEC210_USER_CANCEL_ACK_HELD window=window-0",),
                         wall_timeout_seconds)
                wait_for("ground-station", (
                    "SPEC210_TERMINAL_RESPONSE_CALLBACK_HELD window=0",
                ), wall_timeout_seconds)
                (terminal_race_gate / "release-ack").touch()
                wait_for("ground-station", (
                    "SPEC210_CANCEL_RACE_WAITING_FOR_RESPONSE window=0",
                ), wall_timeout_seconds)
                (terminal_race_gate / "release-response-callback").touch()
                wait_for("ground-station", (
                    "SPEC210_TERMINAL_RESPONSE_DEFERRED window=0",
                ), wall_timeout_seconds)
        user_cancel_faults = {
            "user-cancel-during-active-reference-fetch": "window-0",
            "user-cancel-after-source-switch-during-active-reference-fetch": "window-1",
        }
        cancel_window = user_cancel_faults.get(plan["settings"]["fault"])
        if cancel_window is not None:
            wait_for("compute", (
                "SPEC210_ACTIVE_REFERENCE_FETCH_STARTED camera=UAV3 "
                f"window={cancel_window} category=POSE",
            ), wall_timeout_seconds)
            wait_for("uav3-camera", (
                "SPEC210_ACTIVE_REFERENCE_SOURCE_HELD camera=UAV3 "
                f"window={cancel_window} category=POSE",
            ), wall_timeout_seconds)
            ground_station = next(
                process for name, process, _log in processes if name == "ground-station")
            ground_station.send_signal(signal.SIGUSR1)
        result_marker = wait_for("ground-station",
                                 ("SPEC191_TRACKING_RESULT status=success",
                                  "SPEC191_TRACKING_RESULT status=failure",
                                  "SPEC191_TRACKING_PLAN_FAILED",
                                  "SPEC191_TRACKING_TIMEOUT",
                                  "SPEC191_TRACKING_CANCELLED"),
                                 wall_timeout_seconds)
        if cancel_window is not None:
            wait_for("compute", (
                f"SPEC210_USER_CANCEL_LATE_WORKER_COMPLETED window={cancel_window}",
                f"SPEC210_USER_CANCEL_CANDIDATE_DISCARDED window={cancel_window}",
                "SPEC210_USER_CANCEL_FAILURE_SUPPRESSED",
            ), wall_timeout_seconds)
        success = result_marker == "SPEC191_TRACKING_RESULT status=success"
        if success and person_mode:
            final_index = plan["settings"]["windowCount"] - 1
            presentation = output / f"private/ground-station/presentation/window-{final_index}/presentation.json"
            if not 0 < presentation.stat().st_size <= 4 * 1024 * 1024:
                raise RunnerError("GS final presentation is outside byte bound")
            result_digest = json.loads(presentation.read_bytes())["resultDigest"]
            ground_station_process = next(p for name, p, _ in processes if name == "ground-station")
            if ground_station_process.wait(timeout=max(.001, min(5, run_deadline - time.monotonic()))) != 0:
                raise RunnerError("GS exited unsuccessfully after its result marker")
            while time.monotonic() < run_deadline:
                if resource_sampler is not None:
                    resource_sampler.sample()
                if _person_final_display_shown(
                        (output / f"{display_name}.log").read_text(errors="replace"),
                        run_id=output.name, epoch=plan["observation"]["sourceEpoch"],
                        window_index=final_index, result_digest=result_digest):
                    break
                display_process = next(p for name, p, _ in processes if name == display_name)
                if display_process.poll() is not None:
                    raise RunnerError("GS display exited before showing the final verified window")
                time.sleep(.05)
            else:
                raise RunnerError("GS display did not show all three final verified frames")
        elif person_mode and result_marker in {
                "SPEC191_TRACKING_RESULT status=failure", "SPEC191_TRACKING_CANCELLED"}:
            # A terminal failure must reach the operator before its renderer is torn down.
            presentation_deadline = min(run_deadline, time.monotonic() + 5.0)
            ground_station_process = next(p for name, p, _ in processes if name == "ground-station")
            if ground_station_process.wait(timeout=max(.001, presentation_deadline - time.monotonic())) != 1:
                raise RunnerError("GS exited unexpectedly after its failure marker")
            presentation = output / "private/ground-station/presentation/latest.json"
            with presentation.open("rb") as stream:
                payload = stream.read(4 * 1024 * 1024 + 1)
            if not 0 < len(payload) <= 4 * 1024 * 1024:
                raise RunnerError("GS failure presentation is outside byte bound")
            failure = json.loads(payload)
            window = re.fullmatch(r"window-(0|[1-9][0-9]{0,3})", str(failure.get("windowId")))
            descriptor = json.loads(Path(plan["personTracking"]["computationManifest"]).read_bytes())
            if (failure.get("schema") != "uav-person-display-v1" or
                    failure.get("state") != "failure" or failure.get("runId") != output.name or
                    failure.get("missionId") != output.name or window is None or
                    int(window[1]) >= plan["settings"]["windowCount"] or
                    failure.get("profileDigest") != plan["personTracking"]["profileDigest"] or
                    failure.get("computationDigest") != descriptor["computationDigest"]):
                raise RunnerError("GS failure presentation scope mismatch")
            projection_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
            display_command = next(p["command"] for p in plan["processes"] if p["name"] == display_name)
            mode = display_command[display_command.index("--mode") + 1]
            display_process = next(p for name, p, _ in processes if name == display_name)
            shown_at = None
            while time.monotonic() < presentation_deadline:
                if resource_sampler is not None:
                    resource_sampler.sample()
                if display_process.poll() is not None:
                    raise RunnerError("GS display exited before completing its failure presentation")
                now = time.monotonic()
                if shown_at is None and _person_failure_display_presented(
                        (output / f"{display_name}.log").read_text(errors="replace"),
                        run_id=output.name, window_index=int(window[1]),
                        projection_digest=projection_digest, mode=mode):
                    shown_at = now
                if shown_at is not None and (mode == "headless" or now - shown_at >= 2.0):
                    break
                time.sleep(.05)
            else:
                raise RunnerError("GS display did not complete its bounded failure presentation")
        if success:
            for name in pusher_names:
                wait_for(name, ("UAV_OBSERVATION_PUSHER_COMPLETE",), wall_timeout_seconds)
        outcome = ("collaboration-result" if success else
                   "collaboration-timeout" if result_marker == "SPEC191_TRACKING_TIMEOUT" else
                   "collaboration-cancelled" if result_marker == "SPEC191_TRACKING_CANCELLED" else
                   "collaboration-failed")
        _write_json(output / "lifecycle.json", {
            "schema": "spec191-uav-lifecycle/v2",
            "terminal": True,
            "outcome": outcome,
            "marker": result_marker,
            "elapsedSeconds": round(time.monotonic() - started, 3),
        })
        return 0 if success else 1
    except Exception as error:
        display_log = output / f"{display_name}.log"
        try:
            display_text = display_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            display_text = ""
        display_marker = min(
            (marker for marker in
             ("DISPLAY_FAILED", "DISPLAY_LOST", "DISPLAY_WINDOW_CLOSED")
             if marker in display_text),
            key=display_text.find,
            default=None)
        display_outcome = ({"DISPLAY_FAILED": "display-failed",
                            "DISPLAY_LOST": "display-lost",
                            "DISPLAY_WINDOW_CLOSED": "display-window-closed"}
                           .get(display_marker))
        _write_json(output / "lifecycle.json", {
            "schema": "spec191-uav-lifecycle/v2",
            "terminal": display_marker is not None,
            "outcome": display_outcome or "launcher-failed",
            **({"marker": display_marker} if display_marker is not None else {}),
            "error": f"{type(error).__name__}: {error}",
        })
        return 1
    finally:
        cleanup_started_ns = time.monotonic_ns()
        cleanup_errors = []
        if resource_sampler is not None:
            resource_sampler.sample(force=True)
        for _name, process, _log in reversed(processes):
            if process.poll() is None:
                try:
                    process.send_signal(signal.SIGINT)
                except ProcessLookupError:
                    pass
        cleanup_deadline = time.monotonic() + (12 if main_mode else 5)
        for _name, process, _log in reversed(processes):
            try:
                process.wait(timeout=max(.001, cleanup_deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
        for _name, process, _log in reversed(processes):
            if process.poll() is None:
                try:
                    process.kill()
                except ProcessLookupError:
                    pass
        reap_deadline = time.monotonic() + 2
        for _name, process, _log in reversed(processes):
            try:
                process.wait(timeout=max(.001, reap_deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
        for _name, _process, log in processes:
            log.close()
        nfd_reap_deadline = cleanup_started_ns / 1_000_000_000 + (16 if main_mode else 9)
        for nfd in reversed(nfd_apps):
            process = nfd.process
            if process is not None:
                try:
                    nfd.stop()
                    process.wait(timeout=max(.001, nfd_reap_deadline - time.monotonic()))
                except Exception as error:
                    cleanup_errors.append(f"NFD stop/reap: {type(error).__name__}: {error}"[:512])
        for label, cleanup_action in (("stop", ndn.stop), ("cleanUp", Minindn.cleanUp)):
            try:
                cleanup_action()
            except Exception as error:
                cleanup_errors.append(f"{label}: {type(error).__name__}: {error}"[:512])
        try:
            shutil.rmtree(socket_root)
        except Exception as error:
            cleanup_errors.append(
                f"NFD socket directory cleanup: {type(error).__name__}: {error}"[:512])
        if person_mode or main_mode:
            resources = resource_sampler.finish()
            _write_json(output / ("main-resources.json" if main_mode else "person-resources.json"), resources)
            from person_run_receipt import audit_person_processes
            audit = audit_person_processes(output, resources["processes"])
            ended_ns = time.monotonic_ns()
            _write_json(output / ("main-cleanup.json" if main_mode else "person-cleanup.json"), {
                "schema": "uav-person-cleanup-v1", "clockDomain": "CLOCK_MONOTONIC_NS",
                "runStartedNs": started_ns, "cleanupStartedNs": cleanup_started_ns,
                "cleanupEndedNs": ended_ns,
                "cleanupSeconds": (ended_ns - cleanup_started_ns) / 1_000_000_000,
                "cleanupErrors": cleanup_errors,
                "applicationExitCodes": {name: process.poll() for name, process, _ in processes},
                **audit})


@contextmanager
def _uav_run_ownership(output: Path):
    """Exclude other nonperson owners before changing this run's public or private files."""
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with (output / ".uav-run.lock").open("a") as owner:
        try:
            fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RunnerError("UAV run root is already owned") from error
        yield


def run(plan: dict[str, Any], output: Path, *, headless: bool,
        wall_timeout_seconds: float) -> int:
    if plan.get("trackingMain", {}).get("preparedSequence"):
        with _uav_run_ownership(output):
            return _run_main_sequence(plan, output.resolve(), headless=headless,
                                      wall_timeout_seconds=wall_timeout_seconds)
    if "personTracking" in plan:
        return _run_person(plan, output.resolve(), headless=headless,
                           wall_timeout_seconds=wall_timeout_seconds)
    with _uav_run_ownership(output):
        return _run_tracking(plan, output, headless=headless,
                             wall_timeout_seconds=wall_timeout_seconds)


def _run_tracking(plan: dict[str, Any], output: Path, *, headless: bool,
                  wall_timeout_seconds: float) -> int:
    """Run a nonperson plan while its CLI or public API caller owns the run root."""
    check = preflight(plan, run=True, headless=headless)
    _write_json(output / "preflight.json", check)
    if not check["ok"]:
        return 2
    security = plan.get("security", {})
    trust_schema = Path(security.get("trustSchema", ""))
    if not trust_schema.is_file() or _sha256_bytes(trust_schema.read_bytes()) != \
            security.get("trustSchemaDigest"):
        raise RunnerError("trust schema changed or disappeared after the run plan was created")
    if security.get("trustSchemaSource") == "maintained-fixed-root":
        security.update(_provision_run_security(output))
        _write_json(output / "manifest.json", plan)
        for certificate in security["publicCertificates"]:
            if digest_file(Path(certificate["path"])) != certificate["digest"]:
                raise RunnerError("provisioned public certificate changed")
    observation = plan.get("observation", {})
    if observation.get("enabled"):
        profile = _snapshot_observation_profile(observation, output)
        generation_path = Path(observation["sourceGenerationManifest"])
        _write_json(generation_path, observation["sourceGenerations"])
        generations = load_source_generation_manifest(
            generation_path, int(plan["settings"]["windowCount"]))
        if _sha256_bytes(generation_path.read_bytes()) != \
                observation.get("sourceGenerationManifestSha256"):
            raise RunnerError("source generation manifest changed after the run plan was created")
        source_root = Path(observation["source"]["root"])
        if observation.get("deliveryMode") != "live-push":
            _prepare_source_frame_files(
                source_root, output, window_count=int(plan["settings"]["windowCount"]),
                profile=profile, source_ids=observation["sourceIds"],
                source_epoch=int(observation["sourceEpoch"]),
                source_generations=generations)
        if observation.get("deliveryMode") == "live-push":
            for camera in ("UAV1", "UAV2", "UAV3"):
                fifo_dir = output / f"private/observation-pipes/{camera}"
                fifo_dir.mkdir(parents=True, exist_ok=True)
                for window in range(int(plan["settings"]["windowCount"])):
                    fifo = fifo_dir / f"window-{window}.jsonl"
                    if fifo.exists():
                        fifo.unlink()
                    os.mkfifo(fifo, 0o600)
    else:
        source_root = Path(plan["assets"]["videos"][0]["path"]).parent
        _prepare_source_frame_files(source_root, output,
                                    window_count=int(plan["settings"]["windowCount"]),
                                    legacy=True)
    if plan["settings"].get("fault") == "missing-view":
        try:
            if observation.get("enabled") and observation.get("deliveryMode") == "live-push":
                missing = output / "private/observation-pipes/UAV2/window-1.jsonl"
            elif observation.get("enabled"):
                missing = output / "private/observations/UAV2/window-1.jsonl"
            else:
                missing = output / "private/frames/UAV2/window-1.jpg"
            missing.unlink()
        except FileNotFoundError:
            pass
    if plan.get("applicationMode") == "main":
        _freeze_main_sources(plan)
        _mark_main_network_attempt(plan, output)
        _write_json(output / "manifest.json", plan)
    code = _run_minindn(plan, output, wall_timeout_seconds=wall_timeout_seconds)
    if plan.get("applicationMode") == "main":
        return _collect_main_run(plan, output, code)
    return code


def _collect_main_run(plan: dict[str, Any], output: Path, code: int) -> int:
    from person_run_receipt import build_main_run_receipt
    closure_verified = True
    try:
        _verify_main_plan(plan, prepared=True)
        if "personTracking" in plan:
            _verify_person_plan(plan)
    except (OSError, ValueError, KeyError, RunnerError):
        closure_verified = False
    try:
        receipt = build_main_run_receipt(output, plan, closure_verified=closure_verified)
    except (OSError, ValueError, KeyError, TypeError) as error:
        try:
            lifecycle_path = output / "lifecycle.json"
            with lifecycle_path.open("rb") as stream:
                raw = stream.read(65537)
            if len(raw) > 65536:
                raise ValueError("lifecycle artifact exceeds bound")
            lifecycle = json.loads(raw)
        except (OSError, ValueError) as lifecycle_error:
            lifecycle = {"terminal": False, "outcome": "missing-or-invalid-lifecycle",
                         "error": str(lifecycle_error)[:512]}
        receipt = {"schema": "uav-main-tracking-receipt-v1", "runId": output.name,
            "applicationMode": "main", "complete": False,
            "errors": ["receipt evidence missing/invalid: " + str(error)[:1024]],
            "installedClosureVerified": closure_verified,
            "lifecycle": lifecycle,
            "scientificAccuracyClaimAllowed": False}
    _write_json(output / "main-run-receipt.json", receipt)
    return code if receipt["complete"] else 1


def _run_person(plan: dict[str, Any], output: Path, *, headless: bool,
                wall_timeout_seconds: float) -> int:
    with (output / ".person-source.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        _verify_person_plan(plan)
        security = plan["security"]
        if digest_file(Path(security["trustSchema"])) != security["trustSchemaDigest"]:
            raise RunnerError("person trust schema changed after plan creation")
        check = preflight(plan, run=True, headless=headless)
        _write_json(output / "preflight.json", check)
        if not check["ok"]:
            print(json.dumps(check, indent=2), file=sys.stderr)
            return 2
        # One network attempt per immutable source epoch/protection context.
        with (output / "person-network-attempt.json").open("x") as attempt:
            json.dump({"sourceEpoch": plan["observation"]["sourceEpoch"],
                       "startedAt": datetime.now(timezone.utc).isoformat()}, attempt)
        security.update(_provision_run_security(output))
        _write_json(output / "manifest.json", plan)
        _verify_person_plan(plan)
        for certificate in security["publicCertificates"]:
            if digest_file(Path(certificate["path"])) != certificate["digest"]:
                raise RunnerError("provisioned public certificate changed")
        if plan.get("applicationMode") == "main":
            _freeze_main_sources(plan)
            _mark_main_network_attempt(plan, output)
            _write_json(output / "manifest.json", plan)
        code = _run_minindn(plan, output, wall_timeout_seconds=wall_timeout_seconds)
        if plan.get("applicationMode") == "main":
            return _collect_main_run(plan, output, code)
        from person_run_receipt import build_person_run_receipt
        closure_verified = True
        try:
            _verify_person_plan(plan)
        except RunnerError:
            closure_verified = False
        receipt = build_person_run_receipt(output, plan, closure_verified=closure_verified)
        _write_json(output / "person-run-receipt.json", receipt)
        return code


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application-mode", choices=("compat", "main"), default="compat",
                        help="main runs the installed NDNSF-UAV GS/Drone containers and GTK Tracking tab")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--tracking-profile", type=Path,
                        help="strict indoor person profile with pinned model digests and device")
    parser.add_argument("--source-manifest", type=Path,
                        help="three distinct indoor videos and declared common media interval")
    parser.add_argument("--reid-model", type=Path, help="pretrained OSNet checkpoint for person mode")
    parser.add_argument("--worker-python", type=Path, default=Path(sys.executable),
                        help="explicit installed model interpreter for person or main vehicle mode")
    parser.add_argument("--license", default="")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--run-seconds", type=float, default=60.0)
    parser.add_argument("--window-count", type=int, default=None)
    parser.add_argument("--source-switch-after-window", type=int, action="append", default=[],
                        metavar="N",
                        help="start a new sourceId/epoch after N completed windows; repeat in increasing order")
    parser.add_argument("--global-deadline-ms", type=int, default=60000)
    parser.add_argument("--trust-schema", type=Path, default=DEFAULT_TRUST_SCHEMA,
                        help="NDN trust schema for the controller, sources, compute, and ground station")
    parser.add_argument("--fault", choices=sorted(FAULTS), default="none")
    parser.add_argument("--motion-fixture", type=Path,
                        help="explicitly select the legacy Spec191 motion-fixture path")
    parser.add_argument("--motion-profile", choices=MOTION_PROFILES,
                        help="generate the explicit legacy motion profile")
    parser.add_argument("--legacy-mode", action="store_true",
                        help="run the legacy Spec191 path without Spec210 observations; motion stays disabled unless a fixture/profile is selected")
    parser.add_argument("--observation-profile", type=Path,
                        help="explicit Spec210 observation profile; cannot be combined with legacy motion")
    parser.add_argument("--observation-mode", choices=("simulated", "replay", "live-push"),
                        default="simulated")
    parser.add_argument("--external-source-adapter", type=Path,
                        help="executable that prints UAV_OBSERVATION_SOURCE_ADAPTER_READY, then writes uav-source-event-v2 JSONL to Spec210 FIFOs; on source cutover also echo generation-manifest-sha256=sha256:<hex>")
    args = parser.parse_args(argv)
    if not (args.prepare or args.preflight or args.run):
        parser.error("one of --prepare, --preflight, or --run is required")
    try:
        owner = (nullcontext() if any((args.tracking_profile, args.source_manifest, args.reid_model))
                 else _uav_run_ownership(args.output_dir))
        with owner:
            person = None
            if any((args.tracking_profile, args.source_manifest, args.reid_model)):
                if not all((args.tracking_profile, args.source_manifest, args.reid_model)):
                    raise RunnerError("person mode requires tracking-profile, source-manifest and reid-model")
                if (args.legacy_mode or args.motion_fixture or args.motion_profile or args.observation_profile or
                        args.external_source_adapter or args.source_switch_after_window or
                        args.observation_mode not in {"simulated", "replay"} or
                        args.trust_schema != DEFAULT_TRUST_SCHEMA):
                    raise RunnerError("person mode cannot use legacy or custom observation/trust inputs")
                if (args.output_dir / "person-network-attempt.json").exists():
                    raise RunnerError("person output already has a network attempt; use its own new run root")
                from person_profile import load_person_profile
                profile = load_person_profile(args.tracking_profile.resolve())
                receipt = prepare_person_sources(
                    args.source_manifest, args.output_dir, profile,
                    source_restart_after_window_zero=(
                        args.fault == PERSON_SOURCE_EPOCH_RESTART_FAULT))
                person = {"profile": receipt["trackingProfile"],
                          "reidModel": str(args.reid_model.resolve()),
                          "workerPython": str(args.worker_python.absolute())}
                with (args.output_dir / ".person-source.lock").open("a") as lock:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    person.update(_describe_person_computation(person, args.model, args.output_dir,
                        application_mode=args.application_mode))
            plan = build_plan(source=args.source, model=args.model, output=args.output_dir,
                              license_text=args.license, headless=args.headless,
                              run_seconds=args.run_seconds,
                              window_count=args.window_count, fault=args.fault,
                              global_deadline_ms=args.global_deadline_ms,
                              motion_fixture=args.motion_fixture,
                              motion_profile=args.motion_profile,
                              legacy_mode=args.legacy_mode,
                              observation_profile=args.observation_profile,
                              observation_mode=args.observation_mode,
                              source_switch_after_window=args.source_switch_after_window,
                              external_source_adapter=args.external_source_adapter,
                              trust_schema=args.trust_schema, person_tracking=person,
                              application_mode=args.application_mode, worker_python=args.worker_python)
            _write_json(args.output_dir / "manifest.json", plan)
            if args.prepare:
                print(json.dumps({"ok": True, "manifest": str(args.output_dir / "manifest.json")},
                                 indent=2))
            if (args.preflight or args.run) and not (person is not None and args.run):
                check = preflight(plan, run=args.run, headless=args.headless)
                _write_json(args.output_dir / "preflight.json", check)
                print(json.dumps(check, indent=2))
                if not check["ok"]:
                    return 2
            return ((run if person is not None else _run_tracking)(
                        plan, args.output_dir, headless=args.headless,
                        wall_timeout_seconds=args.global_deadline_ms / 1000) if args.run else 0)
    except (RunnerError, OSError, ValueError) as error:
        print(f"Spec191 runner failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
