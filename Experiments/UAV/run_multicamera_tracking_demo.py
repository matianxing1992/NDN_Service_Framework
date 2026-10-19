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
from dataclasses import dataclass
import copy
from contextlib import contextmanager
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
import subprocess
import sys
import time
from typing import Any, Iterable, Sequence

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SOURCE = Path(os.environ.get(
    "NDNSF_SPEC191_ASSET_ROOT", "/home/tianxing/.cache/ndnsf/spec191-assets"))
DEFAULT_MODEL = DEFAULT_SOURCE / "3UAVs.pt"
DEFAULT_OUTPUT = ROOT / "results/uav-tracking/spec191-local"
DEFAULT_OBSERVATION_PROFILE = ROOT / "NDNSF-UAV-APP/configs/uav_observation_demo.json"
DEFAULT_TRUST_SCHEMA = ROOT / "examples/trust-any.conf"
TOPOLOGY = Path(__file__).with_name("topology.conf")
MAX_REPLAY_WINDOWS = 600
DEFAULT_SAMPLE_RATE_HZ = 5.0
DEFAULT_FRAME_COUNT = 100
REQUIRED_NATIVE_PROGRAMS = (
    "UavTrackingNode",
    "App_ServiceController",
    "UavTrackingGroundStation",
)
DEFAULT_TRACKING_LIBEXEC = Path(
    os.environ.get("NDNSF_UAV_TRACKING_LIBEXEC",
                   "/usr/local/libexec/ndnsf-uav/tracking"))

sys.path.insert(0, str(Path(__file__).parent))
from tracking_artifacts import (ArtifactError, validate_live_source_model,
                                validate_real_assets,
                                validate_video_reference_assets)  # noqa: E402
from tracking_topology import TopologyError, load_topology  # noqa: E402
from motion_fixture import MOTION_PROFILES, write_fixture  # noqa: E402
from observation_source import (load_demo_profile, load_observation_profile, parse_observation_profile,
                                load_source_generation_manifest,
                                source_generation_for_window,
                                write_profile_frame)  # noqa: E402


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

FAULTS = frozenset({
    *REFERENCE_IDENTITY_CONTENT_FAULTS,
    "none", "missing-view", "bad-content", "bad-identity", "bad-reference-digest",
    "bad-reference-digest-clock", "bad-reference-digest-calibration",
    "bad-reference-digest-ground", "bad-reference-digest-pose",
    "missing-reference-clock",
    "duplicate-reference-pose", "overlimit-reference-pose",
    "stale-epoch", "worker-failure", "response-loss", "late-response-loss", "cancel",
    "cancel-during-active-fetch", "cancel-during-active-reference-fetch",
    "user-cancel-during-active-reference-fetch", "recover-active-reference-fetch",
    "user-cancel-after-source-switch-during-active-reference-fetch",
    "user-cancel-terminal-response-race",
    "user-cancel-terminal-response-cancel-wins-race",
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
                   observation_enabled: bool = True) -> list[ProcessSpec]:
    common = ("--run-id", output.name, "--topology", str(TOPOLOGY),
              "--window-count", str(window_count))
    display = "headless" if headless else "windows"
    fault_args = () if fault == "none" else ("--fault", fault)
    display_fault_args = () if fault in {
        "cancel-during-active-fetch", "cancel-during-active-reference-fetch",
        "user-cancel-during-active-reference-fetch", "recover-active-reference-fetch",
    "user-cancel-after-source-switch-during-active-reference-fetch",
    "user-cancel-terminal-response-race",
    "user-cancel-terminal-response-cancel-wins-race"} else fault_args
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
        () if observation_enabled and observation_profile.get("deliveryMode") == "live-push"
        else ("--video-source", str(source / f"uav{camera[-1]}_1min.mp4")))
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
        ProcessSpec("controller", "gs", "/example/uav/controller", root,
                    (controller, "--node", "gs", "--controller-prefix",
                     "/example/uav/controller", "--policy-file",
                     str(root / "Experiments/UAV/spec191.policies"),
                     *trust_args,
                     "--ensure-identities",
                     "/example/uav/drone/UAV1,/example/uav/drone/UAV2,"
                     "/example/uav/drone/UAV3,/example/uav/compute,/example/uav/gs",
                     *common),
                    output / "private/controller"),
        ProcessSpec("ground-station", "gs", "/example/uav/gs", root,
                    (ground_station, "--node", "gs", "--display", display,
                     "--group-prefix", "/example/uav", "--controller-prefix",
                     "/example/uav/controller", *trust_args, *motion_args,
                     *observation_request_args,
                     "--request-timeout-ms",
                     str(request_timeout_ms), *common, *fault_args),
                    output / "private/ground-station"),
        ProcessSpec("uav1-camera", "uav1", "/example/uav/drone/UAV1", root,
                    (tracking_node, "--role", "source", "--camera", "UAV1",
                     *video_args("UAV1"),
                     "--frame-dir", str(output / "private/frames/UAV1"),
                     *observation_args("UAV1"),
                     *motion_args,
                     *trust_args,
                     "--model", str(model), *common, *fault_args),
                    output / "private/uav1"),
        ProcessSpec("uav2-camera", "uav2", "/example/uav/drone/UAV2", root,
                    (tracking_node, "--role", "source", "--camera", "UAV2",
                     *video_args("UAV2"),
                     "--frame-dir", str(output / "private/frames/UAV2"),
                     *observation_args("UAV2"),
                     *motion_args,
                     *trust_args,
                     "--model", str(model), *common, *fault_args),
                    output / "private/uav2"),
        ProcessSpec("uav3-camera", "uav3", "/example/uav/drone/UAV3", root,
                    (tracking_node, "--role", "source", "--camera", "UAV3",
                     *video_args("UAV3"),
                     "--frame-dir", str(output / "private/frames/UAV3"),
                     *observation_args("UAV3"),
                     *motion_args,
                     *trust_args,
                     "--model", str(model), *common, *fault_args),
                    output / "private/uav3"),
        ProcessSpec("compute", "compute", "/example/uav/compute", root,
                    (tracking_node, "--role", "compute", "--display", display,
                     "--model", str(model), "--worker-python", sys.executable,
                     "--worker-script",
                     str(_installed_tracking_script("tracking_worker.py")),
                     *observation_runtime_args,
                     *motion_args,
                     *trust_args,
                     *common, *fault_args),
                    output / "private/compute"),
        ProcessSpec("compute-display", "compute", "/example/uav/compute/display", root,
                    (sys.executable, display_runner, "--socket",
                     str(output / "private/compute-display/display.sock"),
                     "--mode", display, "--replay-json",
                     str(worker_root / "tracking-result.json"), "--replay-dir",
                     str(worker_root / "annotated"), *display_clock_args, *display_scope_args,
                     *display_fault_args),
                    output / "private/compute-display"),
    ]
    # Source adapters are local producers attached to existing UAV nodes; they
    # do not add network nodes or alter the five-node forwarding topology.
    return [specs[0], *pushers, *external_sources, *specs[1:]]


def _check_python_dependencies() -> list[str]:
    missing: list[str] = []
    for module in ("cv2", "numpy", "ultralytics", "lap"):
        try:
            __import__(module)
        except Exception as error:  # pragma: no cover - host-dependent
            missing.append(f"python:{module}:{type(error).__name__}")
    return missing


def _application_library_environment_command() -> str:
    override = os.environ.get("NDNSF_UAV_LIBRARY_PATH")
    return ("unset LD_LIBRARY_PATH" if override is None else
            f"export LD_LIBRARY_PATH={shlex.quote(override)}")


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
               trust_schema: Path = DEFAULT_TRUST_SCHEMA) -> dict[str, Any]:
    if window_count is not None and type(window_count) is not int:
        raise RunnerError("window_count must be an integer")
    model = model.expanduser().resolve()
    output = output.expanduser().resolve()
    trust_schema = trust_schema.expanduser().resolve()
    if not model.is_file():
        raise RunnerError(f"model checkpoint is not available: {model}")
    if not trust_schema.is_file():
        raise RunnerError(f"trust schema is not a regular file: {trust_schema}")
    trust_schema_digest = _sha256_bytes(trust_schema.read_bytes())
    output.mkdir(parents=True, exist_ok=True)
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
                           observation_enabled=not legacy_compatibility)
    nodes = sorted(topology.nodes)
    if {item.node for item in specs} != set(nodes):
        raise RunnerError("process plan does not cover the exact topology nodes")
    if len(nodes) != 5 or "controller" in nodes:
        raise RunnerError("Controller must remain a gs-local process, not a MiniNDN node")
    return {
        "schema": "spec191-uav-tracking-run/v1",
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
            "trustSchemaSource": "repo-demo-default"
            if trust_schema == DEFAULT_TRUST_SCHEMA else "explicit-input",
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
            "sampleFps": sample_rate_hz,
            "sourceSeconds": 60.0,
            "windowSeconds": 1.0 / sample_rate_hz,
            "windowCount": window_count,
            "sourceSwitchAfterWindows": list(source_switch_after_window),
            "fault": fault,
            "ackTimeoutMs": 1000,
            "globalDeadlineMs": global_deadline_ms,
            "runSeconds": run_seconds,
            "retryReselection": False,
            "cpuOnly": True,
        },
        "scientificAccuracyClaimAllowed": False,
    }


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


def _video_presentation_timestamps(video: Path) -> list[int]:
    """Read decoded video-frame PTS through ffprobe; never infer time from FPS."""
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
         "-show_entries", "frame=best_effort_timestamp_time", "-of", "json", str(video)],
        check=False, capture_output=True, text=True)
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
                                legacy: bool = False) -> dict[str, list[str]]:
    """Create bounded JPEG JSONL bundles for source-owned observation replay."""
    if legacy:
        return _prepare_legacy_source_frame_files(source, output, window_count=window_count)
    if profile is None or source_ids is None or source_generations is None:
        raise RunnerError("v2 observation replay requires its profile and source identities")
    import cv2

    sample_rate = float(profile.get("sampleRateHz", DEFAULT_SAMPLE_RATE_HZ))
    if not math.isfinite(sample_rate) or not 1.0 <= sample_rate <= 30.0:
        raise RunnerError("observation sampleRateHz must be in 1..30")
    period_us = int((Decimal(1_000_000) / Decimal(str(sample_rate)))
                    .quantize(Decimal("1"), rounding=ROUND_HALF_UP))
    observation_dir = output / ("private/push-events" if profile.get("deliveryMode") == "live-push"
                                else "private/observations")
    observation_dir.mkdir(parents=True, exist_ok=True)
    records: dict[str, list[str]] = {}
    sampling: dict[str, dict[str, Any]] = {}
    for camera in ("UAV1", "UAV2", "UAV3"):
        video = source / f"uav{camera[-1]}_1min.mp4"
        if not video.is_file():
            raise RunnerError(f"video is unavailable for {camera}: {video}")
        timestamps = _video_presentation_timestamps(video)
        origin_us = timestamps[0]
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
            selected.append(cursor)

        capture = cv2.VideoCapture(str(video))
        camera_dir = observation_dir / camera
        camera_dir.mkdir(parents=True, exist_ok=True)
        paths: list[str] = []
        decoded_index = -1
        frame_indices: list[int] = []
        capture_times: list[int] = []
        target_offsets: list[int] = []
        try:
            for window, frame_index in enumerate(selected):
                while decoded_index < frame_index:
                    ok, frame = capture.read()
                    if not ok or frame is None:
                        raise RunnerError(f"cannot decode PTS-selected frame {window} for {camera}: {video}")
                    decoded_index += 1
                if frame.ndim < 2 or frame.shape[0] <= 0 or frame.shape[1] <= 0:
                    raise RunnerError(f"decoded frame has invalid dimensions for {camera}: {video}")
                encoded, data = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                if not encoded or data.nbytes == 0:
                    raise RunnerError(f"cannot encode observation frame {window} for {camera}")
                target = camera_dir / f"window-{window}.jsonl"
                temporary = target.with_suffix(".jsonl.tmp")
                generation = source_generation_for_window(source_generations, window)
                with temporary.open("wb") as stream:
                    write_profile_frame(
                        stream, profile, camera, window + 1, timestamps[frame_index] - origin_us,
                        data.tobytes(), source_id=str(generation["sourceIds"][camera]),
                        epoch=int(generation["sourceEpoch"]),
                        source_time_origin_us=0, image_width=int(frame.shape[1]),
                        image_height=int(frame.shape[0]))
                os.replace(temporary, target)
                paths.append(str(target))
                frame_indices.append(frame_index)
                capture_times.append(timestamps[frame_index] - origin_us)
                target_offsets.append(timestamps[frame_index] - origin_us - targets[window])
        finally:
            capture.release()
        records[camera] = paths
        sampling[camera] = {
            "sourcePresentationTimeOriginUs": origin_us,
            "periodUs": period_us,
            "frameIndices": frame_indices,
            "targetTimeUs": targets,
            "captureTimeUs": capture_times,
            "targetOffsetUs": target_offsets,
        }
    _write_json(output / "source-frame-inputs.json", {
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


def preflight(plan: dict[str, Any], *, run: bool, headless: bool) -> dict[str, Any]:
    errors: list[str] = []
    errors.extend(_check_python_dependencies())
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
            for name in REQUIRED_NATIVE_PROGRAMS
            if shutil.which(name) is None)
        errors.extend(
            f"missing installed tracking script: {path}"
            for path in (_installed_tracking_script("tracking_worker.py"),
                         _installed_tracking_script("tracking_display.py"),
                         _installed_tracking_script("motion_estimator.py"),
                         _installed_tracking_script("motion_schema.py"),
                         _installed_tracking_script("observation_schema.py"),
                         _installed_tracking_script("observation_source.py"))
            if not path.is_file())
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
        socket = Path(node.params["params"]["homeDir"]) / f"{node.name}.sock"
        child_ndn_log = os.environ.get("SPEC191_CHILD_NDN_LOG", "*=WARN").strip() or "*=WARN"
        child_pythonpath = _child_pythonpath()
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
        if plan["settings"]["fault"] in {
                "user-cancel-terminal-response-race",
                "user-cancel-terminal-response-cancel-wins-race"}:
            # Keep the deterministic callback hold off the Face loop itself.
            env.append("export NDNSF_HANDLER_THREADS=2")
        if child_pythonpath:
            env.append(f"export PYTHONPATH={shlex.quote(child_pythonpath)}")
        command = "; ".join(env + [
            f"cd {shlex.quote(str(spec.cwd.resolve()))}",
            f"exec {shlex.join(list(spec.command))}",
        ])
        process = getPopen(node, command, shell=True, stdout=log, stderr=subprocess.STDOUT)
        processes.append((spec.name, process, log))
        return process

    def wait_for(name: str, markers: tuple[str, ...], timeout: float,
                 *, same_line: bool = False) -> str:
        path = output / f"{name}.log"
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
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
                if name != "compute-display" and process_name == "compute-display" and \
                        process.poll() is not None:
                    raise RunnerError(
                        "compute-display exited before campaign completion; "
                        f"returncode={process.returncode}")
            time.sleep(0.1)
        raise RunnerError(f"{name} did not emit required marker(s) {markers!r}")

    def configure_ndn() -> None:
        ndn.start()
        app_nodes = list(ndn.net.hosts)
        if {node.name for node in app_nodes} != set(plan["topology"]["nodes"]):
            raise RunnerError("MiniNDN host set does not match the five-node manifest")
        for node in app_nodes:
            nfd = Nfd(node)
            home_dir = Path(node.params["params"]["homeDir"])
            old_socket = f"/run/nfd/{node.name}.sock"
            new_socket = str(home_dir / f"{node.name}.sock")
            conf_path = Path(nfd.confFile)
            conf_path.write_text(conf_path.read_text(encoding="utf-8").replace(
                old_socket, new_socket), encoding="utf-8")
            Path(nfd.clientConf).write_text(f"transport=unix://{new_socket}\n",
                                            encoding="utf-8")
            nfd.start()
            nfd_apps.append(nfd)
        for node in app_nodes:
            socket = Path(node.params["params"]["homeDir"]) / f"{node.name}.sock"
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
            transport = f"unix://{Path(node.params['params']['homeDir']) / f'{node.name}.sock'}"
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
        if plan["settings"]["fault"] in {
                "user-cancel-terminal-response-race",
                "user-cancel-terminal-response-cancel-wins-race"}:
            terminal_race_gate = output / "private/spec210-terminal-response-race"
            terminal_race_gate.mkdir(parents=True, exist_ok=True)
            for gate_name in ("release-terminal", "release-ack",
                              "release-response-callback"):
                (terminal_race_gate / gate_name).unlink(missing_ok=True)
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
            wait_for(name, ("SPEC191_TRACKING_NODE_READY",), 60)
        launch(by_name["compute-display"])
        wait_for("compute-display", ("DISPLAY_READY",), 30)
        launch(by_name["ground-station"])
        wait_for("ground-station", ("SPEC191_TRACKING_GS_READY",), 30)
        if terminal_race_gate is not None:
            cancellation_wins_race = (
                plan["settings"]["fault"] ==
                "user-cancel-terminal-response-cancel-wins-race")
            wait_for("compute", (
                "SPEC210_TERMINAL_RESPONSE_READY window=window-0" if cancellation_wins_race else
                "SPEC210_TERMINAL_RESPONSE_LOCKED window=window-0",
            ), wall_timeout_seconds)
            ground_station_process = next(
                process for name, process, _log in processes if name == "ground-station")
            ground_station_process.send_signal(signal.SIGUSR1)
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
        display_log = output / "compute-display.log"
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
        for _name, process, _log in reversed(processes):
            if process.poll() is None:
                try:
                    process.send_signal(signal.SIGINT)
                    process.wait(timeout=5)
                except Exception:
                    process.kill()
        for _name, _process, log in processes:
            log.close()
        try:
            ndn.stop()
            Minindn.cleanUp()
        except Exception:
            pass


def run(plan: dict[str, Any], output: Path, *, headless: bool,
        wall_timeout_seconds: float) -> int:
    check = preflight(plan, run=True, headless=headless)
    _write_json(output / "preflight.json", check)
    if not check["ok"]:
        return 2
    security = plan.get("security", {})
    trust_schema = Path(security.get("trustSchema", ""))
    if not trust_schema.is_file() or _sha256_bytes(trust_schema.read_bytes()) != \
            security.get("trustSchemaDigest"):
        raise RunnerError("trust schema changed or disappeared after the run plan was created")
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
    return _run_minindn(plan, output, wall_timeout_seconds=wall_timeout_seconds)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
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
                          trust_schema=args.trust_schema)
        _write_json(args.output_dir / "manifest.json", plan)
        if args.prepare:
            print(json.dumps({"ok": True, "manifest": str(args.output_dir / "manifest.json")},
                             indent=2))
        if args.preflight or args.run:
            check = preflight(plan, run=args.run, headless=args.headless)
            _write_json(args.output_dir / "preflight.json", check)
            print(json.dumps(check, indent=2))
            if not check["ok"]:
                return 2
        return (run(plan, args.output_dir, headless=args.headless,
                    wall_timeout_seconds=args.run_seconds) if args.run else 0)
    except (RunnerError, OSError, ValueError) as error:
        print(f"Spec191 runner failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
