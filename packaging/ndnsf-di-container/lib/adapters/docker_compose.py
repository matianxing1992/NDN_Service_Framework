"""Docker Compose execution adapter for long-lived NDNSF-DI cloud nodes."""

from __future__ import annotations

import json
import importlib.util
import os
from pathlib import Path
import stat
import subprocess
import sys
from typing import Any, Callable, Mapping, Sequence

from adapters.base import Adapter


def _load_common(name: str):
    module_name = f"ndnsf_container_{name}"
    module = sys.modules.get(module_name)
    if module is not None:
        return module
    path = Path(__file__).resolve().parents[1] / f"{name}.py"
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise ImportError(name)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


_profile = _load_common("profile")
_release = _load_common("release")
validate_profile = _profile.validate_profile
load_release_manifest = _release.load_release_manifest
materialization_record = _release.materialization_record


class ComposeAdapterError(RuntimeError):
    """Compose prerequisite, lifecycle, mount, or readiness failure."""


RunCommand = Callable[..., subprocess.CompletedProcess]


def _cpu_image(release: Mapping[str, Any]) -> Mapping[str, Any]:
    matches = [image for image in release["images"].values() if image.get("backend") == "cpu"]
    if len(matches) != 1:
        raise ComposeAdapterError(f"COMPOSE_CPU_IMAGE_COUNT:{len(matches)}")
    return matches[0]


def _socket_path(profile: Mapping[str, Any]) -> Path:
    configured = Path(profile["compose"]["nfdSocket"])
    endpoint = profile["network"].get("localEndpoint", "")
    if endpoint != "unix://" + str(configured):
        raise ComposeAdapterError("COMPOSE_NFD_SOCKET_ENDPOINT_MISMATCH")
    if not configured.is_absolute():
        raise ComposeAdapterError("COMPOSE_NFD_SOCKET_NOT_ABSOLUTE")
    return configured


def render_environment(profile: Mapping[str, Any], release: Mapping[str, Any]) -> dict[str, str]:
    if profile["runtime"]["kind"] != "docker-compose":
        raise ComposeAdapterError("COMPOSE_PROFILE_ADAPTER_MISMATCH")
    socket_path = _socket_path(profile)
    image = _cpu_image(release)
    return {
        "COMPOSE_PROJECT_NAME": profile["compose"]["projectName"],
        "NDNSF_OCI_IMAGE": image["reference"],
        "NDNSF_IDENTITY_ROOT": profile["identity"]["reference"],
        "NDNSF_PROJECT_ROOT": profile["storage"]["projectRoot"],
        "NDNSF_EVIDENCE_ROOT": profile["storage"]["evidenceRoot"],
        "NDNSF_NFD_RUN_DIR": str(socket_path.parent),
        "NDNSF_CONTAINER_UID": str(os.geteuid()),
        "NDNSF_CONTAINER_GID": str(os.getegid()),
    }


def _validate_private_path(path: Path, *, label: str, require_exists: bool) -> None:
    if require_exists and not path.exists():
        raise ComposeAdapterError(f"COMPOSE_{label}_MISSING:{path}")
    if not path.exists():
        return
    mode = stat.S_IMODE(path.stat().st_mode)
    if mode & stat.S_IWOTH:
        raise ComposeAdapterError(f"COMPOSE_{label}_WORLD_WRITABLE:{path}")
    if path.stat().st_uid not in {0, os.geteuid()}:
        raise ComposeAdapterError(f"COMPOSE_{label}_OWNER_INVALID:{path}")


def validate_mount_contract(profile: Mapping[str, Any], *, require_socket: bool,
                            require_paths: bool = True) -> dict[str, str]:
    if not profile["identity"].get("readOnly"):
        raise ComposeAdapterError("COMPOSE_IDENTITY_NOT_READ_ONLY")
    identity = Path(profile["identity"]["reference"])
    project = Path(profile["storage"]["projectRoot"])
    socket_path = _socket_path(profile)
    run_dir = socket_path.parent
    _validate_private_path(identity, label="IDENTITY", require_exists=True)
    _validate_private_path(project, label="PROJECT", require_exists=require_paths)
    _validate_private_path(run_dir, label="NFD_RUN_DIR", require_exists=require_paths)
    if require_paths:
        required = (
            project / "config/nfd",
            project / "config/policies.conf",
            project / "config/trust-schema.conf",
            project / "config/native-execution-plan.json",
            project / "config/native-service-manifest.json",
        )
        missing = [str(path) for path in required if not path.exists()]
        if missing:
            raise ComposeAdapterError("COMPOSE_PROJECT_CONFIG_MISSING:" + ",".join(missing))
    if require_socket:
        try:
            socket_stat = socket_path.stat()
        except FileNotFoundError as exc:
            raise ComposeAdapterError(f"COMPOSE_NFD_SOCKET_MISSING:{socket_path}") from exc
        if not stat.S_ISSOCK(socket_stat.st_mode):
            raise ComposeAdapterError(f"COMPOSE_NFD_SOCKET_NOT_SOCKET:{socket_path}")
        if socket_stat.st_uid not in {0, os.geteuid()}:
            raise ComposeAdapterError(f"COMPOSE_NFD_SOCKET_OWNER_INVALID:{socket_path}")
    return {"identity": str(identity), "project": str(project), "nfdSocket": str(socket_path)}


class DockerComposeAdapter(Adapter):
    def __init__(self, *, runner: RunCommand = subprocess.run) -> None:
        self._runner = runner

    @staticmethod
    def _compose_file(profile: Mapping[str, Any]) -> str:
        return profile["compose"].get("composeFile") or str(
            Path(__file__).resolve().parents[2] / "adapters/docker-compose/compose.yaml")

    def _run(self, command: Sequence[str], *, environment: Mapping[str, str] | None = None) -> subprocess.CompletedProcess:
        merged = dict(os.environ)
        if environment:
            merged.update(environment)
        result = self._runner(list(command), text=True, capture_output=True, check=False, env=merged)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise ComposeAdapterError(f"COMPOSE_COMMAND_FAILED:{command[0]}:{detail}")
        return result

    def _context(self, profile: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any], dict[str, str], list[str]]:
        validate_profile(profile)
        release = load_release_manifest(profile["releaseManifest"])
        environment = render_environment(profile, release)
        compose = ["docker", "compose", "--file", self._compose_file(profile),
                   "--project-name", profile["compose"]["projectName"]]
        return profile, release, environment, compose

    def preflight(self, profile: dict[str, Any]) -> dict[str, Any]:
        profile, release, environment, compose = self._context(profile)
        validate_mount_contract(profile, require_socket=False)
        docker = self._run(["docker", "version", "--format", "{{json .}}"])
        compose_version = self._run(["docker", "compose", "version", "--short"])
        self._run([*compose, "config", "--quiet"], environment=environment)
        return {"status": "PASS", "adapter": "docker-compose", "releaseId": release["releaseId"],
                "dockerVersion": docker.stdout.strip(), "composeVersion": compose_version.stdout.strip()}

    def materialize(self, profile: dict[str, Any]) -> dict[str, Any]:
        profile, release, environment, _ = self._context(profile)
        image = _cpu_image(release)
        self._run(["docker", "pull", image["reference"]], environment=environment)
        repo_digests_result = self._run(
            ["docker", "image", "inspect", image["reference"], "--format", "{{json .RepoDigests}}"])
        try:
            repo_digests = json.loads(repo_digests_result.stdout)
        except json.JSONDecodeError as exc:
            raise ComposeAdapterError("COMPOSE_REPO_DIGESTS_INVALID") from exc
        if not isinstance(repo_digests, list) or image["reference"] not in repo_digests:
            raise ComposeAdapterError("COMPOSE_PULLED_DIGEST_MISMATCH")
        inspected = self._run(["docker", "image", "inspect", image["reference"], "--format", "{{.Id}}"])
        image_id = inspected.stdout.strip()
        if not image_id.startswith("sha256:"):
            raise ComposeAdapterError("COMPOSE_IMAGE_ID_INVALID")
        return materialization_record(adapter="docker-compose", oci_reference=image["reference"],
                                      materialization_type="docker-image", materialization_id=image_id,
                                      runtime_version="docker", path=None)

    install = materialize

    def start(self, profile: dict[str, Any]) -> dict[str, Any]:
        profile, release, environment, compose = self._context(profile)
        validate_mount_contract(profile, require_socket=False)
        self._run([*compose, "up", "--detach", "--wait"], environment=environment)
        validate_mount_contract(profile, require_socket=True)
        return {"status": "PASS", "releaseId": release["releaseId"],
                "projectName": profile["compose"]["projectName"]}

    def status(self, reference: str | dict[str, Any]) -> dict[str, Any]:
        if not isinstance(reference, dict):
            raise ComposeAdapterError("COMPOSE_STATUS_REQUIRES_PROFILE")
        profile, release, environment, compose = self._context(reference)
        result = self._run([*compose, "ps", "--format", "json"], environment=environment)
        services = _parse_compose_services(result.stdout)
        expected = {"nfd", "controller", "provider"}
        observed = {str(item.get("Service", "")) for item in services}
        if observed != expected:
            raise ComposeAdapterError(
                "COMPOSE_SERVICE_SET_INVALID:expected=" + ",".join(sorted(expected)) +
                ":observed=" + ",".join(sorted(observed)))
        unhealthy = [str(item.get("Service")) for item in services
                     if str(item.get("State", "")).lower() != "running" or
                     str(item.get("Health", "")).lower() not in {"healthy", ""}]
        if unhealthy:
            raise ComposeAdapterError("COMPOSE_SERVICES_NOT_READY:" + ",".join(sorted(unhealthy)))
        return {"status": "PASS", "releaseId": release["releaseId"], "services": services}

    def logs(self, reference: str | dict[str, Any]) -> dict[str, Any]:
        if not isinstance(reference, dict):
            raise ComposeAdapterError("COMPOSE_LOGS_REQUIRES_PROFILE")
        profile, _, environment, compose = self._context(reference)
        result = self._run([*compose, "logs", "--no-color", "--timestamps"], environment=environment)
        return {"status": "PASS", "logs": result.stdout}

    def evidence(self, reference: str | dict[str, Any]) -> dict[str, Any]:
        if not isinstance(reference, dict):
            raise ComposeAdapterError("COMPOSE_EVIDENCE_REQUIRES_PROFILE")
        status = self.status(reference)
        mounts = validate_mount_contract(reference, require_socket=True)
        return {"kind": "docker-compose", "projectName": reference["compose"]["projectName"],
                "services": status["services"], "health": "PASS", "routes": [], "mounts": mounts}

    def stop(self, reference: str | dict[str, Any]) -> dict[str, Any]:
        if not isinstance(reference, dict):
            raise ComposeAdapterError("COMPOSE_STOP_REQUIRES_PROFILE")
        profile, release, environment, compose = self._context(reference)
        self._run([*compose, "stop"], environment=environment)
        return {"status": "PASS", "releaseId": release["releaseId"], "statePreserved": True}


def _parse_compose_services(output: str) -> list[dict[str, Any]]:
    text = output.strip()
    if not text:
        return []
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        try:
            value = [json.loads(line) for line in text.splitlines() if line.strip()]
        except json.JSONDecodeError as exc:
            raise ComposeAdapterError("COMPOSE_PS_JSON_INVALID") from exc
    if isinstance(value, dict):
        value = [value]
    if not isinstance(value, list) or not all(isinstance(item, dict) for item in value):
        raise ComposeAdapterError("COMPOSE_PS_JSON_INVALID")
    return value
