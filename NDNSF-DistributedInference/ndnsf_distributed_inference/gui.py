"""Tkinter GUI for NDNSF-DistributedInference deployment workflows.

The GUI is intentionally a thin shell around the APP-level API and existing
command-line tools. It helps users create and inspect policy files, choose NDN
identities, run policy validation, and launch example controller/provider/user
processes without exposing low-level NDN packet details.
"""

from __future__ import annotations

import argparse
import csv
import html
import json
import os
import queue
import shlex
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Callable, Protocol, Sequence

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError as exc:  # pragma: no cover - depends on system Tk package
    raise RuntimeError("NDNSF-DI GUI requires Python tkinter") from exc

from .onnx_graph import analyze_onnx_graph, estimate_split_candidates
from .policy import explain_policy, load_config, load_or_generate_deployment


DEFAULT_POLICY = Path(
    "examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml"
)


@dataclass
class RuntimeRoleProfile:
    role: str
    config: str = str(DEFAULT_POLICY)
    example: str = "YOLO 2x2"
    generated_policy_dir: str = "/tmp/ndnsf-di-gui-policy"
    group: str = ""
    provider_id: str = "A"
    roles: str = "all"
    service: str = "/AI/YOLO/2x2Inference"
    ack_timeout_ms: str = "1500"
    timeout_ms: str = "60000"
    extra_args: str = ""

    @classmethod
    def from_mapping(cls, role: str, data: dict[str, Any] | None) -> "RuntimeRoleProfile":
        profile = cls(role=role)
        if not isinstance(data, dict):
            return profile
        allowed = set(asdict(profile))
        values = {key: str(value) for key, value in data.items() if key in allowed}
        values["role"] = role
        return cls(**values)


@dataclass
class RuntimeGuiProfile:
    controller: RuntimeRoleProfile
    provider: RuntimeRoleProfile
    user: RuntimeRoleProfile

    @classmethod
    def default(cls) -> "RuntimeGuiProfile":
        return cls(
            controller=RuntimeRoleProfile(role="controller"),
            provider=RuntimeRoleProfile(role="provider"),
            user=RuntimeRoleProfile(role="user"),
        )

    @classmethod
    def from_mapping(cls, data: dict[str, Any]) -> "RuntimeGuiProfile":
        return cls(
            controller=RuntimeRoleProfile.from_mapping("controller", data.get("controller")),
            provider=RuntimeRoleProfile.from_mapping("provider", data.get("provider")),
            user=RuntimeRoleProfile.from_mapping("user", data.get("user")),
        )

    def to_mapping(self) -> dict[str, Any]:
        return {
            "controller": asdict(self.controller),
            "provider": asdict(self.provider),
            "user": asdict(self.user),
        }


@dataclass
class NdnsfSvsEnvConfig:
    enable_ndnsd: bool = True
    disable_ndnsd: bool = False
    expected_rps: str = ""
    publication_fetch_retries: str = ""
    publication_fetch_inner_retries: str = ""
    publication_fetch_lifetime_ms: str = ""
    publication_fetch_backoff_ms: str = ""
    publication_fetch_max_backoff_ms: str = ""
    publication_fetch_window: str = ""
    max_suppression_ms: str = "1"
    periodic_sync_ms: str = ""
    parallel_sync: bool = True
    parallel_workers: str = ""
    parallel_queue: str = ""
    parallel_production: bool = True
    sync_batching: bool = False
    sync_batch_ms: str = ""

    def to_env(self) -> dict[str, str]:
        values: dict[str, str] = {}
        if self.enable_ndnsd:
            values["NDNSF_ENABLE_NDNSD"] = "1"
        if self.disable_ndnsd:
            values["NDNSF_DISABLE_NDNSD"] = "1"
        mapping = {
            "expected_rps": "NDNSF_SVS_EXPECTED_RPS",
            "publication_fetch_retries": "NDNSF_SVS_PUBLICATION_FETCH_RETRIES",
            "publication_fetch_inner_retries": "NDNSF_SVS_PUBLICATION_FETCH_INNER_RETRIES",
            "publication_fetch_lifetime_ms": "NDNSF_SVS_PUBLICATION_FETCH_LIFETIME_MS",
            "publication_fetch_backoff_ms": "NDNSF_SVS_PUBLICATION_FETCH_BACKOFF_MS",
            "publication_fetch_max_backoff_ms": "NDNSF_SVS_PUBLICATION_FETCH_MAX_BACKOFF_MS",
            "publication_fetch_window": "NDNSF_SVS_PUBLICATION_FETCH_WINDOW",
            "max_suppression_ms": "NDNSF_SVS_MAX_SUPPRESSION_MS",
            "periodic_sync_ms": "NDNSF_SVS_PERIODIC_SYNC_MS",
            "parallel_workers": "NDNSF_SVS_PARALLEL_WORKERS",
            "parallel_queue": "NDNSF_SVS_PARALLEL_QUEUE",
            "sync_batch_ms": "NDNSF_SVS_SYNC_BATCH_MS",
        }
        for field_name, env_name in mapping.items():
            value = str(getattr(self, field_name, "")).strip()
            if value:
                values[env_name] = value
        values["NDNSF_SVS_PARALLEL_SYNC"] = "1" if self.parallel_sync else "0"
        values["NDNSF_SVS_PARALLEL_PRODUCTION"] = "1" if self.parallel_production else "0"
        if self.sync_batching:
            values["NDNSF_SVS_SYNC_BATCHING"] = "1"
        return values

    @classmethod
    def from_mapping(cls, data: dict[str, Any] | None) -> "NdnsfSvsEnvConfig":
        if not isinstance(data, dict):
            return cls()
        allowed = set(cls.__dataclass_fields__)
        return cls(**{key: value for key, value in data.items() if key in allowed})


@dataclass
class SharedNdnsfConfig:
    group: str = "/NDNSF-DI/Tracer/group"
    controller: str = "/NDNSF-DI/Tracer/controller"
    trust_schema: str = "examples/trust-schema.conf"
    serve_certificates: bool = True

    @classmethod
    def from_mapping(cls, data: dict[str, Any] | None) -> "SharedNdnsfConfig":
        if not isinstance(data, dict):
            return cls()
        allowed = set(cls.__dataclass_fields__)
        return cls(**{key: value for key, value in data.items() if key in allowed})


@dataclass
class ControllerTabConfig:
    controller_prefix: str = "/NDNSF-DI/Tracer/controller"
    policy_file: str = "examples/hello.policies"
    trust_schema: str = "examples/trust-schema.conf"
    bootstrap_token_file: str = "examples/hello.bootstrap-tokens"
    bootstrap_identities: str = ""
    serve_certificates: bool = True

    @classmethod
    def from_mapping(cls, data: dict[str, Any] | None) -> "ControllerTabConfig":
        if not isinstance(data, dict):
            return cls()
        allowed = set(cls.__dataclass_fields__)
        return cls(**{key: value for key, value in data.items() if key in allowed})


@dataclass
class ProviderTabConfig:
    provider_id: str = "A"
    provider_prefix: str = "/NDNSF-DI/Tracer/provider"
    group: str = "/NDNSF-DI/Tracer/group"
    controller: str = "/NDNSF-DI/Tracer/controller"
    trust_schema: str = "examples/trust-schema.conf"
    bootstrap_token: str = ""
    service_name: str = "/HELLO"
    roles: str = "all"
    handler_threads: int = 4
    ack_threads: int = 2
    serve_certificates: bool = True
    handler_mode: str = "echo"
    static_response: str = "HELLO"
    ack_status: bool = True
    ack_message: str = "ready"
    ack_metadata_json: str = "{}"
    ndnsd_lifetime_seconds: int = 30
    ndnsd_meta_json: str = "{}"
    runtime_profile: str = "examples/di-native-tracer.runtime.json"
    service_manifest: str = ""
    native_plan: str = ""
    fragment_inventory_json: str = "{}"
    artifact_cache_dir: str = "/tmp/ndnsf-di-artifacts"
    memory_compute_profile_json: str = "{}"
    deployment_id: str = ""
    provider_probing: bool = False
    provider_probe_interval_s: int = 10

    @classmethod
    def from_mapping(cls, data: dict[str, Any] | None) -> "ProviderTabConfig":
        if not isinstance(data, dict):
            return cls()
        allowed = set(cls.__dataclass_fields__)
        return cls(**{key: value for key, value in data.items() if key in allowed})


@dataclass
class UserRequestConfig:
    service_name: str = "/HELLO"
    request_strategy: str = "first-responding"
    ack_timeout_ms: int = 1000
    timeout_ms: int = 10000
    payload_encoding: str = "text"
    payload: str = "HELLO"
    request_mode: str = "normal"
    collaboration_roles_json: str = "[]"
    key_scopes_json: str = "{}"
    dependencies_json: str = "[]"
    artifact_data_names_json: str = "{}"
    scope_key_data_names_json: str = "{}"
    role_scopes_json: str = "{}"
    deployment_id: str = ""

    @classmethod
    def from_mapping(cls, data: dict[str, Any] | None) -> "UserRequestConfig":
        if not isinstance(data, dict):
            return cls()
        allowed = set(cls.__dataclass_fields__)
        return cls(**{key: value for key, value in data.items() if key in allowed})


@dataclass
class UserTabConfig:
    user: str = "/NDNSF-DI/Tracer/user"
    group: str = "/NDNSF-DI/Tracer/group"
    controller: str = "/NDNSF-DI/Tracer/controller"
    trust_schema: str = "examples/trust-schema.conf"
    bootstrap_token: str = ""
    permission_wait_ms: int = 1500
    handler_threads: int = 2
    ack_threads: int = 2
    adaptive_admission: bool = False
    serve_certificates: bool = True
    request: UserRequestConfig = field(default_factory=UserRequestConfig)

    @classmethod
    def from_mapping(cls, data: dict[str, Any] | None) -> "UserTabConfig":
        if not isinstance(data, dict):
            return cls()
        values = dict(data)
        values["request"] = UserRequestConfig.from_mapping(values.get("request"))
        allowed = set(cls.__dataclass_fields__)
        return cls(**{key: value for key, value in values.items() if key in allowed})


@dataclass
class ThreeRoleGuiProfile:
    version: int = 2
    shared: SharedNdnsfConfig = field(default_factory=SharedNdnsfConfig)
    env: NdnsfSvsEnvConfig = field(default_factory=NdnsfSvsEnvConfig)
    controller: ControllerTabConfig = field(default_factory=ControllerTabConfig)
    provider: ProviderTabConfig = field(default_factory=ProviderTabConfig)
    user: UserTabConfig = field(default_factory=UserTabConfig)
    persist_tokens: bool = False

    @classmethod
    def from_mapping(cls, data: dict[str, Any]) -> "ThreeRoleGuiProfile":
        if "version" not in data and {"controller", "provider", "user"} <= set(data):
            return cls.from_legacy(RuntimeGuiProfile.from_mapping(data))
        return cls(
            version=int(data.get("version", 2)),
            shared=SharedNdnsfConfig.from_mapping(data.get("shared")),
            env=NdnsfSvsEnvConfig.from_mapping(data.get("env")),
            controller=ControllerTabConfig.from_mapping(data.get("controller")),
            provider=ProviderTabConfig.from_mapping(data.get("provider")),
            user=UserTabConfig.from_mapping(data.get("user")),
            persist_tokens=bool(data.get("persist_tokens", False)),
        )

    @classmethod
    def from_legacy(cls, legacy: RuntimeGuiProfile) -> "ThreeRoleGuiProfile":
        return cls(
            controller=ControllerTabConfig(),
            provider=ProviderTabConfig(
                group=legacy.provider.group or ProviderTabConfig.group,
                service_name=legacy.provider.service,
                provider_id=legacy.provider.provider_id,
                roles=legacy.provider.roles,
            ),
            user=UserTabConfig(
                group=legacy.user.group or UserTabConfig.group,
                request=UserRequestConfig(
                    service_name=legacy.user.service,
                    ack_timeout_ms=int(legacy.user.ack_timeout_ms or 1000),
                    timeout_ms=int(legacy.user.timeout_ms or 10000),
                ),
            ),
        )

    def to_mapping(self) -> dict[str, Any]:
        data = asdict(self)
        if not self.persist_tokens:
            data["provider"]["bootstrap_token"] = ""
            data["user"]["bootstrap_token"] = ""
        return data


def load_three_role_profile(path: str | Path) -> ThreeRoleGuiProfile:
    return ThreeRoleGuiProfile.from_mapping(json.loads(Path(path).read_text(encoding="utf-8")))


def write_three_role_profile(path: str | Path, profile: ThreeRoleGuiProfile) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(profile.to_mapping(), indent=2), encoding="utf-8")


def load_runtime_profile(path: str | Path) -> RuntimeGuiProfile:
    return RuntimeGuiProfile.from_mapping(json.loads(Path(path).read_text(encoding="utf-8")))


def write_runtime_profile(path: str | Path, profile: RuntimeGuiProfile) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(profile.to_mapping(), indent=2), encoding="utf-8")


SECRET_KEYS = {"token", "bootstrap_token", "password", "safebag"}


def redact_secret(value: str) -> str:
    if not value:
        return ""
    if len(value) <= 4:
        return "****"
    return value[:2] + "****" + value[-2:]


def redact_mapping(data: dict[str, Any]) -> dict[str, Any]:
    redacted: dict[str, Any] = {}
    for key, value in data.items():
        if any(secret in key.lower() for secret in SECRET_KEYS):
            redacted[key] = redact_secret(str(value))
        elif isinstance(value, dict):
            redacted[key] = redact_mapping(value)
        else:
            redacted[key] = value
    return redacted


def parse_json_field(value: str, *, default: Any) -> Any:
    text = str(value or "").strip()
    if not text:
        return default
    return json.loads(text)


def parse_int_field(value: Any, *, name: str, minimum: int = 0) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be an integer") from exc
    if parsed < minimum:
        raise ValueError(f"{name} must be >= {minimum}")
    return parsed


def payload_from_request(config: UserRequestConfig) -> bytes:
    value = config.payload
    if config.payload_encoding == "text":
        return value.encode("utf-8")
    if config.payload_encoding == "json":
        return json.dumps(json.loads(value), sort_keys=True).encode("utf-8")
    if config.payload_encoding == "hex":
        return bytes.fromhex(value.strip())
    if config.payload_encoding == "file":
        return Path(value).read_bytes()
    raise ValueError(f"unsupported payload encoding: {config.payload_encoding}")


class RuntimeFactory(Protocol):
    def create_controller(self, config: ControllerTabConfig): ...
    def create_provider(self, config: ProviderTabConfig): ...
    def create_user(self, config: UserTabConfig): ...


class RealRuntimeFactory:
    def create_controller(self, config: ControllerTabConfig):
        from ndnsf import ServiceController
        return ServiceController(
            controller_prefix=config.controller_prefix,
            policy_file=config.policy_file,
            trust_schema=config.trust_schema,
            bootstrap_identities=[
                item.strip() for item in config.bootstrap_identities.split(",")
                if item.strip()
            ],
            serve_certificates=config.serve_certificates,
            bootstrap_token_file=config.bootstrap_token_file,
        )

    def create_provider(self, config: ProviderTabConfig):
        from ndnsf import AckDecision, ServiceProvider
        provider = ServiceProvider(
            provider_id=config.provider_id,
            group=config.group,
            controller=config.controller,
            provider_prefix=config.provider_prefix,
            trust_schema=config.trust_schema,
            handler_threads=parse_int_field(config.handler_threads, name="handler_threads", minimum=1),
            ack_threads=parse_int_field(config.ack_threads, name="ack_threads", minimum=1),
            serve_certificates=config.serve_certificates,
            bootstrap_token=config.bootstrap_token,
        )

        def handler(payload: bytes):
            if config.handler_mode == "static":
                return config.static_response.encode("utf-8")
            if config.handler_mode == "dry-run":
                return b"DRY-RUN"
            return bytes(payload)

        def ack_handler(_payload: bytes):
            metadata = parse_json_field(config.ack_metadata_json, default={})
            message = config.ack_message
            if metadata:
                message = json.dumps({"message": message, "meta": metadata}, sort_keys=True)
            return AckDecision(status=bool(config.ack_status), message=message)

        provider.add_handler(config.service_name, handler)
        provider.set_ack_handler(config.service_name, ack_handler)
        return provider

    def create_user(self, config: UserTabConfig):
        from ndnsf import ServiceUser
        return ServiceUser(
            group=config.group,
            controller=config.controller,
            user=config.user,
            trust_schema=config.trust_schema,
            permission_wait_ms=parse_int_field(config.permission_wait_ms,
                                               name="permission_wait_ms",
                                               minimum=0),
            handler_threads=parse_int_field(config.handler_threads,
                                            name="handler_threads",
                                            minimum=1),
            ack_threads=parse_int_field(config.ack_threads,
                                        name="ack_threads",
                                        minimum=1),
            adaptive_admission=bool(config.adaptive_admission),
            serve_certificates=config.serve_certificates,
            bootstrap_token=config.bootstrap_token,
        )


class FakeRuntime:
    def __init__(self, role: str, response: bytes = b"HELLO") -> None:
        self.role = role
        self.response = response
        self.started = False
        self.stopped = False
        self.requests: list[tuple[str, bytes]] = []

    def start(self) -> None:
        self.started = True

    def stop(self) -> None:
        self.stopped = True
        self.started = False

    def start_background(self, service: str | None = None):
        del service
        self.start()
        return threading.current_thread()

    def run(self, service: str | None = None) -> int:
        del service
        self.start()
        return 0

    def request_service(self, service: str, payload: bytes, **_kwargs):
        self.requests.append((service, bytes(payload)))

        @dataclass
        class Response:
            success: bool = True
            status: bool = True
            message: str = "fake-ok"
            payload: bytes = b""

        return Response(payload=self.response)

    def get_allowed_services(self):
        return []

    def get_ndnsd_services(self):
        return []


class FakeRuntimeFactory:
    def __init__(self) -> None:
        self.created: dict[str, FakeRuntime] = {}

    def create_controller(self, config: ControllerTabConfig):
        del config
        self.created["controller"] = FakeRuntime("controller")
        return self.created["controller"]

    def create_provider(self, config: ProviderTabConfig):
        del config
        self.created["provider"] = FakeRuntime("provider")
        return self.created["provider"]

    def create_user(self, config: UserTabConfig):
        del config
        self.created["user"] = FakeRuntime("user")
        return self.created["user"]


class EnvOverlay:
    def __init__(self, values: dict[str, str]) -> None:
        self.values = {key: str(value) for key, value in values.items() if str(value) != ""}
        self.previous: dict[str, str | None] = {}

    def __enter__(self):
        for key, value in self.values.items():
            self.previous[key] = os.environ.get(key)
            os.environ[key] = value
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        for key, value in self.previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


class RoleRuntimeController:
    def __init__(self,
                 role: str,
                 factory: RuntimeFactory | None = None,
                 log_callback: Callable[[str], None] | None = None,
                 status_callback: Callable[[str], None] | None = None,
                 env_config: NdnsfSvsEnvConfig | None = None) -> None:
        self.role = role
        self.factory = factory or RealRuntimeFactory()
        self.log_callback = log_callback or (lambda _msg: None)
        self.status_callback = status_callback or (lambda _status: None)
        self.env_config = env_config or NdnsfSvsEnvConfig()
        self.runtime: Any = None
        self.thread: threading.Thread | None = None
        self.status = "stopped"
        self.last_error = ""
        self._lock = threading.Lock()

    def set_status(self, value: str) -> None:
        self.status = value
        self.status_callback(value)

    def log(self, message: str) -> None:
        self.log_callback(f"[{self.role}] {message}")

    def run(self, config: Any) -> None:
        with self._lock:
            if self.status in {"starting", "running"}:
                self.log("already running\n")
                return
            self.set_status("starting")
            self.last_error = ""
        self.thread = threading.Thread(target=self._run_thread, args=(config,), daemon=True)
        self.thread.start()

    def _run_thread(self, config: Any) -> None:
        try:
            with EnvOverlay(self.env_config.to_env()):
                if self.role == "controller":
                    self.runtime = self.factory.create_controller(config)
                    self.set_status("running")
                    if hasattr(self.runtime, "start_background"):
                        self.runtime.start_background()
                    else:
                        self.runtime.start()
                elif self.role == "provider":
                    self.runtime = self.factory.create_provider(config)
                    self.set_status("running")
                    if hasattr(self.runtime, "start_background"):
                        self.runtime.start_background(getattr(config, "service_name", None))
                    elif hasattr(self.runtime, "run"):
                        self.runtime.run(getattr(config, "service_name", None))
                    else:
                        self.runtime.start()
                elif self.role == "user":
                    self.runtime = self.factory.create_user(config)
                    self.runtime.start()
                    self.set_status("running")
                else:
                    raise ValueError(f"unknown role: {self.role}")
                self.log("runtime started\n")
        except Exception as exc:
            self.last_error = str(exc)
            self.set_status("failed")
            self.log(f"ERROR: {exc}\n")

    def stop(self) -> None:
        with self._lock:
            if self.status not in {"starting", "running"}:
                self.set_status("stopped")
                return
            self.set_status("stopping")
        try:
            if self.runtime is not None and hasattr(self.runtime, "stop"):
                self.runtime.stop()
            self.set_status("stopped")
            self.log("runtime stopped\n")
        except Exception as exc:
            self.last_error = str(exc)
            self.set_status("failed")
            self.log(f"ERROR during stop: {exc}\n")

    def restart(self, config: Any) -> None:
        self.stop()
        self.run(config)

    def request_user(self, config: UserRequestConfig):
        if self.role != "user":
            raise RuntimeError("request_user is only valid for user role")
        if self.runtime is None or self.status != "running":
            raise RuntimeError("user runtime is not running")
        payload = payload_from_request(config)
        return self.runtime.request_service(
            config.service_name,
            payload,
            ack_timeout_ms=parse_int_field(config.ack_timeout_ms,
                                           name="ack_timeout_ms",
                                           minimum=0),
            timeout_ms=parse_int_field(config.timeout_ms,
                                       name="timeout_ms",
                                       minimum=1),
            strategy=config.request_strategy,
        )


def _load_json_mapping(path: str | Path) -> dict[str, Any]:
    value = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"configuration must be a JSON object: {path}")
    return value


def apply_role_config_file(profile: ThreeRoleGuiProfile,
                           role: str,
                           path: str | Path) -> None:
    """Merge a full profile or one role-specific JSON config into *profile*."""
    data = _load_json_mapping(path)
    if "version" in data and {"controller", "provider", "user"} <= set(data):
        loaded = ThreeRoleGuiProfile.from_mapping(data)
        setattr(profile, role, getattr(loaded, role))
        return
    if role in data and isinstance(data[role], dict):
        data = data[role]
    if role == "controller":
        profile.controller = ControllerTabConfig.from_mapping(data)
    elif role == "provider":
        profile.provider = ProviderTabConfig.from_mapping(data)
    elif role == "user":
        profile.user = UserTabConfig.from_mapping(data)
    else:
        raise ValueError(f"unknown role config: {role}")


def _response_to_mapping(response: Any) -> dict[str, Any]:
    payload = getattr(response, "payload", b"")
    if isinstance(payload, bytes):
        payload_text = payload.decode("utf-8", errors="replace")
        payload_hex = payload.hex()
        payload_size = len(payload)
    else:
        payload_text = str(payload)
        payload_hex = ""
        payload_size = len(payload_text.encode("utf-8"))
    return {
        "success": bool(getattr(response, "success", getattr(response, "status", False))),
        "status": bool(getattr(response, "status", getattr(response, "success", False))),
        "message": str(getattr(response, "message", getattr(response, "error", ""))),
        "payload_text": payload_text,
        "payload_hex": payload_hex,
        "payload_size": payload_size,
    }


def _wait_for_role(controller: RoleRuntimeController,
                   *,
                   timeout_s: float = 5.0) -> bool:
    deadline = time.time() + max(0.0, timeout_s)
    while time.time() < deadline:
        if controller.status in {"running", "failed"}:
            return controller.status == "running"
        time.sleep(0.02)
    if controller.thread is not None:
        controller.thread.join(timeout=0.1)
    return controller.status == "running"


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="NDNSF-DI Tk GUI and headless role automation")
    parser.add_argument("-headless", "--headless", action="store_true",
                        help="Run without creating a Tk window.")
    parser.add_argument("--profile", default="",
                        help="Full three-role GUI profile JSON.")
    parser.add_argument("-user_auto_run", "--user-auto-run", "--user_auto_run",
                        action="store_true", dest="user_auto_run",
                        help="Start the user role in headless mode.")
    parser.add_argument("-provider_auto_run", "--provider-auto-run",
                        "--provider_auto_run", action="store_true",
                        dest="provider_auto_run",
                        help="Start the provider role in headless mode.")
    parser.add_argument("-controller_auto_run", "--controller-auto-run",
                        "--controller_auto_run", action="store_true",
                        dest="controller_auto_run",
                        help="Start the controller role in headless mode.")
    parser.add_argument("-user_config", "--user-config", "--user_config",
                        default="", dest="user_config",
                        help="JSON config for the user role or a full profile.")
    parser.add_argument("-provider_config", "--provider-config",
                        "--provider_config", default="", dest="provider_config",
                        help="JSON config for the provider role or a full profile.")
    parser.add_argument("-controller_config", "--controller-config",
                        "--controller_config", default="", dest="controller_config",
                        help="JSON config for the controller role or a full profile.")
    parser.add_argument("--runtime-mode", choices=["direct", "fake"],
                        default="direct",
                        help="Use real Python wrapper runtimes or fake test runtimes.")
    parser.add_argument("--send-user-request", action="store_true",
                        help="After starting the user, send the configured request.")
    parser.add_argument("--duration-s", type=float, default=0.0,
                        help="Keep started roles alive for this many seconds before stop.")
    parser.add_argument("--startup-timeout-s", type=float, default=5.0,
                        help="Seconds to wait for each role to report running.")
    parser.add_argument("--output-json", default="",
                        help="Write headless run summary JSON to this path.")
    parser.add_argument("--headless-experiment",
                        choices=["roles", "qwen-minindn"],
                        default="roles",
                        help=("Headless execution mode. 'roles' starts the GUI "
                              "roles directly; 'qwen-minindn' runs the Qwen "
                              "NativeTracer MiniNDN experiment from GUI config."))
    parser.add_argument("--experiment-runtime-profile", default="",
                        help=("Runtime profile for --headless-experiment "
                              "qwen-minindn. Defaults to provider runtime profile."))
    parser.add_argument("--experiment-out", default="",
                        help="Output directory for the Qwen MiniNDN experiment.")
    parser.add_argument("--experiment-requests", type=int, default=0,
                        help="Override Qwen MiniNDN request count when > 0.")
    parser.add_argument("--experiment-concurrency", type=int, default=0,
                        help="Override Qwen MiniNDN concurrency when > 0.")
    parser.add_argument("--experiment-provider-check-timeout", type=int, default=0,
                        help="Override Qwen MiniNDN provider startup/check timeout.")
    parser.add_argument("--experiment-target-rps", type=float, default=-1.0,
                        help="Override Qwen MiniNDN target RPS when >= 0.")
    parser.add_argument("--experiment-open-loop-duration-s", type=float, default=-1.0,
                        help="Override Qwen MiniNDN open-loop duration when >= 0.")
    parser.add_argument("--experiment-open-loop-driver-mode",
                        choices=["child", "threaded", "process-pool"],
                        default="process-pool",
                        help=("Open-loop Qwen MiniNDN user-driver mode. "
                              "process-pool reuses worker users and is the "
                              "recommended GUI/headless sweep default."))
    parser.add_argument("--experiment-dependency-envelope-mode",
                        "--experiment-dependency-payload-mode",
                        dest="experiment_dependency_envelope_mode",
                        choices=["raw", "streamchunk"],
                        default="raw",
                        help=("Dependency envelope mode for Qwen MiniNDN providers. "
                              "raw keeps exact-name large-data fetches; streamchunk "
                              "is an opt-in metadata-envelope experiment. The old "
                              "--experiment-dependency-payload-mode flag is accepted "
                              "as a compatibility alias."))
    parser.add_argument("--experiment-dry-run", action="store_true",
                        help="Print/record the resolved Qwen MiniNDN command without running MiniNDN.")
    parser.add_argument("--experiment-extra-arg", action="append", default=[],
                        help="Additional argument appended to the Qwen MiniNDN harness; repeatable.")
    return parser


def load_headless_profile(args: argparse.Namespace) -> ThreeRoleGuiProfile:
    if args.profile:
        profile = load_three_role_profile(args.profile)
    else:
        profile = ThreeRoleGuiProfile()
    for role in ("controller", "provider", "user"):
        config_path = getattr(args, f"{role}_config", "")
        if config_path:
            apply_role_config_file(profile, role, config_path)
    return profile


def build_qwen_minindn_command(profile: ThreeRoleGuiProfile,
                               args: argparse.Namespace) -> tuple[list[str], Path]:
    runtime_profile = (
        args.experiment_runtime_profile or
        profile.provider.runtime_profile or
        "examples/di-native-tracer.runtime.json"
    )
    out_dir = Path(
        args.experiment_out or
        "/tmp/ndnsf-di-gui-qwen-headless-minindn"
    )
    command = [
        sys.executable,
        "Experiments/NDNSF_DI_NativeTracer_Minindn.py",
        "--runtime-profile", runtime_profile,
        "--out", str(out_dir),
        "--assignment", "llm-proportional",
        "--policy-bundle", "llm-proportional",
        "--llm-planner-mode", "proportional",
        "--no-local-execution-only",
        "--full-network",
        "--dependency-envelope-mode", args.experiment_dependency_envelope_mode,
    ]
    if args.experiment_requests > 0:
        command.extend(["--requests", str(args.experiment_requests)])
    if args.experiment_concurrency > 0:
        command.extend(["--concurrency", str(args.experiment_concurrency)])
    if args.experiment_provider_check_timeout > 0:
        command.extend([
            "--provider-check-timeout",
            str(args.experiment_provider_check_timeout),
        ])
    if args.experiment_target_rps >= 0:
        command.extend(["--target-rps", str(args.experiment_target_rps)])
    if args.experiment_open_loop_duration_s >= 0:
        command.extend([
            "--open-loop-duration-s",
            str(args.experiment_open_loop_duration_s),
        ])
    if args.experiment_open_loop_driver_mode:
        command.extend([
            "--open-loop-driver-mode",
            args.experiment_open_loop_driver_mode,
        ])
    if args.experiment_dry_run:
        command.append("--dry-run")
    command.extend(args.experiment_extra_arg)
    return command, out_dir


def run_headless_qwen_minindn(args: argparse.Namespace) -> dict[str, Any]:
    profile = load_headless_profile(args)
    command, out_dir = build_qwen_minindn_command(profile, args)
    started_at = time.time()
    proc = subprocess.run(
        command,
        cwd=str(repo_root()),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    harness_summary: dict[str, Any] = {}
    summary_path = out_dir / "summary.json"
    if summary_path.exists():
        try:
            harness_summary = json.loads(summary_path.read_text(encoding="utf-8"))
        except Exception as exc:
            harness_summary = {"status": "unreadable", "error": str(exc)}
    ok = (
        proc.returncode == 0 and
        (args.experiment_dry_run or harness_summary.get("status") == "SUCCESS")
    )
    summary = {
        "ok": ok,
        "headless_experiment": "qwen-minindn",
        "command": command,
        "returncode": proc.returncode,
        "out": str(out_dir),
        "summary_json": str(summary_path),
        "harness_status": harness_summary.get("status", "dry-run" if args.experiment_dry_run else "missing"),
        "miniNDNRun": harness_summary.get("miniNDNRun", ""),
        "runnerMode": harness_summary.get("runnerMode", ""),
        "userExecution": harness_summary.get("userExecution", {}),
        "dependencyExecution": harness_summary.get("dependencyExecution", {}),
        "dependencyEnvelopeMode": harness_summary.get(
            "dependencyEnvelopeMode",
            harness_summary.get("dependencyPayloadMode", args.experiment_dependency_envelope_mode),
        ),
        "dependencyPayloadMode": harness_summary.get(
            "dependencyPayloadMode",
            harness_summary.get("dependencyEnvelopeMode", args.experiment_dependency_envelope_mode),
        ),
        "coreEnvelopeSummary": harness_summary.get("coreEnvelopeSummary", {}),
        "providerAckRuntimeHints": harness_summary.get("providerAckRuntimeHints", {}),
        "streamChunkDependencyCounters": harness_summary.get("streamChunkDependencyCounters", {}),
        "providerUtilization": harness_summary.get("providerUtilization", {}),
        "failureReason": harness_summary.get("failureReason", ""),
        "elapsed_ms": round((time.time() - started_at) * 1000.0, 3),
        "stdout_tail": proc.stdout[-8000:],
    }
    if args.output_json:
        Path(args.output_json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.output_json).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary


def format_core_envelope_summary(core_summary: dict[str, Any],
                                 provider_ack_hints: dict[str, Any] | None = None) -> str:
    """Format core envelope evidence for the Qwen MiniNDN GUI panel."""
    if not isinstance(core_summary, dict) or not core_summary:
        return "No core envelope summary available yet."

    def counter_line(label: str, values: Any) -> str:
        if not isinstance(values, dict) or not values:
            return f"{label}: none"
        return f"{label}: " + ", ".join(
            f"{key}={value}" for key, value in sorted(values.items())
        )

    lines = [
        "Core envelope summary",
        f"ACK events scanned: {core_summary.get('eventCount', 0)}",
        counter_line("Envelopes", core_summary.get("envelopeCounts", {})),
        counter_line("Provider readiness", core_summary.get("providerReadiness", {})),
        counter_line("Reason codes", core_summary.get("reasonCodes", {})),
        counter_line("Service payload schemas", core_summary.get("servicePayloadSchemas", {})),
        counter_line("Operation states", core_summary.get("operationStates", {})),
        "",
        "Latest providers:",
    ]
    latest = core_summary.get("latestProviders", {})
    if isinstance(latest, dict) and latest:
        for provider, item in sorted(latest.items()):
            if not isinstance(item, dict):
                continue
            lines.append(
                "- "
                f"{provider}: ready={item.get('ready', '')} "
                f"queue={item.get('queueLength', 0)} "
                f"active={item.get('activeWorkCount', 0)} "
                f"reason={item.get('reasonCode', '') or 'none'} "
                f"schema={item.get('servicePayloadSchema', '') or 'none'}"
            )
    else:
        lines.append("- none")

    if isinstance(provider_ack_hints, dict) and provider_ack_hints:
        lines.extend(["", "Legacy ACK runtime hints:"])
        providers = provider_ack_hints.get("providers", {})
        if isinstance(providers, dict) and providers:
            for provider, item in sorted(providers.items()):
                if not isinstance(item, dict):
                    continue
                latest_hint = item.get("latest", {})
                latest_hint = latest_hint if isinstance(latest_hint, dict) else {}
                lines.append(
                    "- "
                    f"{provider}: ack={item.get('ackEvents', 0)} "
                    f"success={item.get('successfulAckEvents', 0)} "
                    f"negative={item.get('negativeAckEvents', 0)} "
                    f"queue={latest_hint.get('queue', 0)} "
                    f"runtime={latest_hint.get('runtimeStatus', '') or 'unknown'}"
                )
        else:
            lines.append("- none")
    return "\n".join(lines)


def run_headless(args: argparse.Namespace) -> dict[str, Any]:
    if args.headless_experiment == "qwen-minindn":
        return run_headless_qwen_minindn(args)
    profile = load_headless_profile(args)
    factory: RuntimeFactory
    factory = FakeRuntimeFactory() if args.runtime_mode == "fake" else RealRuntimeFactory()
    logs: list[str] = []
    status_events: list[dict[str, str]] = []

    def make_controller(role: str) -> RoleRuntimeController:
        return RoleRuntimeController(
            role,
            factory=factory,
            log_callback=logs.append,
            status_callback=lambda status, role=role: status_events.append({
                "role": role,
                "status": status,
            }),
            env_config=profile.env,
        )

    controllers = {
        "controller": make_controller("controller"),
        "provider": make_controller("provider"),
        "user": make_controller("user"),
    }
    configs = {
        "controller": profile.controller,
        "provider": profile.provider,
        "user": profile.user,
    }
    auto_order = [
        role for role in ("controller", "provider", "user")
        if getattr(args, f"{role}_auto_run")
    ]
    errors: list[str] = []
    request_result: dict[str, Any] = {}
    started_at = time.time()
    for role in auto_order:
        controllers[role].run(configs[role])
        if not _wait_for_role(controllers[role], timeout_s=args.startup_timeout_s):
            errors.append(f"{role} failed to start: {controllers[role].last_error}")

    if args.send_user_request:
        if "user" not in auto_order:
            errors.append("--send-user-request requires --user-auto-run")
        elif controllers["user"].status != "running":
            errors.append("user request skipped because user is not running")
        else:
            req_started = time.time()
            try:
                response = controllers["user"].request_user(profile.user.request)
                request_result = _response_to_mapping(response)
                request_result["elapsed_ms"] = round((time.time() - req_started) * 1000.0, 3)
            except Exception as exc:
                errors.append(f"user request failed: {exc}")

    if args.duration_s > 0:
        time.sleep(args.duration_s)
    for role in reversed(auto_order):
        controllers[role].stop()

    summary = {
        "ok": not errors,
        "runtime_mode": args.runtime_mode,
        "auto_run_roles": auto_order,
        "statuses": {role: controllers[role].status for role in controllers},
        "last_errors": {
            role: controllers[role].last_error
            for role in controllers
            if controllers[role].last_error
        },
        "status_events": status_events,
        "request": request_result,
        "errors": errors,
        "elapsed_ms": round((time.time() - started_at) * 1000.0, 3),
        "logs": logs[-50:],
    }
    if args.output_json:
        Path(args.output_json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.output_json).write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary


def split_extra_args(value: str) -> list[str]:
    return shlex.split(value) if value.strip() else []


@dataclass
class RoleProcessState:
    label: str
    status: str = "stopped"
    pid: int | None = None
    returncode: int | None = None

    def mark_starting(self) -> str:
        self.status = "starting"
        self.pid = None
        self.returncode = None
        return self.status

    def mark_running(self, pid: int | None) -> str:
        self.status = f"running pid={pid}" if pid is not None else "running"
        self.pid = pid
        self.returncode = None
        return self.status

    def mark_stopping(self) -> str:
        self.status = "stopping"
        return self.status

    def mark_exited(self, returncode: int) -> str:
        state = "exited" if returncode == 0 else "failed"
        self.status = f"{state} rc={returncode}"
        self.returncode = returncode
        self.pid = None
        return self.status


def build_role_command(
    *,
    role: str,
    script_path: str | Path,
    config: str,
    generated_policy_dir: str,
    group: str = "",
    provider_id: str = "",
    roles: str = "",
    ack_timeout_ms: str = "",
    timeout_ms: str = "",
    extra_args: str = "",
    python_executable: str = sys.executable,
) -> list[str]:
    args = [
        python_executable,
        str(script_path),
        "--config", config,
        "--generated-policy-dir", generated_policy_dir,
    ]
    if group and role != "controller":
        args.extend(["--group", group])
    if role == "provider":
        if provider_id:
            args.extend(["--provider-id", provider_id])
        if roles:
            args.extend(["--roles", roles])
    elif role == "user":
        if ack_timeout_ms:
            args.extend(["--ack-timeout-ms", ack_timeout_ms])
        if timeout_ms:
            args.extend(["--timeout-ms", timeout_ms])
    args.extend(split_extra_args(extra_args))
    return args


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def run_command(args: list[str], *, cwd: Path | None = None) -> tuple[int, str]:
    proc = subprocess.run(
        args,
        cwd=str(cwd or repo_root()),
        text=True,
        capture_output=True,
    )
    output = proc.stdout
    if proc.stderr:
        output += ("\n" if output else "") + proc.stderr
    return proc.returncode, output


def load_policy_text(path: str | Path) -> str:
    return Path(path).read_text(encoding="utf-8")


def write_policy_text(path: str | Path, text: str) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(text, encoding="utf-8")


def read_text_file(path: str | Path) -> str:
    return Path(path).read_text(encoding="utf-8")


def write_text_file(path: str | Path, text: str) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(text, encoding="utf-8")


def summarize_policy_file(path: str | Path) -> str:
    out_dir = Path(tempfile.mkdtemp(prefix="ndnsf-di-gui-policy-"))
    deployment = load_or_generate_deployment(path, out_dir)
    return explain_policy(deployment)


def policy_service_names(path: str | Path) -> list[str]:
    config = load_config(path)
    return [
        str(service.get("name", ""))
        for service in config.get("services", [])
        if isinstance(service, dict) and service.get("name")
    ]


def make_basic_policy(
    *,
    application: str,
    controller: str,
    group: str,
    user_identity: str,
    provider_prefix: str,
    service_name: str,
    provider_ids: list[str],
    roles: list[str],
    model_path: str = "",
    model_kind: str = "onnx-model",
    backend: str = "onnxruntime",
) -> dict[str, Any]:
    providers = []
    prefix = provider_prefix.rstrip("/")
    if prefix:
        providers.append({"identity": prefix, "roles": "all"})
    for provider_id in provider_ids:
        provider_id = provider_id.strip("/")
        if provider_id:
            providers.append({"identity": f"{prefix}/{provider_id}", "roles": "all"})
    artifacts = []
    if model_path and roles:
        artifacts.append({
            "role": roles[0],
            "path": model_path,
            "artifact": service_name.rstrip("/") + "/ARTIFACT/" + roles[0].strip("/"),
            "filename": Path(model_path).name,
            "kind": model_kind,
            "backend": backend,
        })
    return {
        "application": application,
        "controller": controller,
        "group": group,
        "runtime": {
            "user_identity": user_identity,
            "provider_prefix": provider_prefix,
        },
        "trust": {
            "app_roots": ["/" + controller.strip("/").split("/")[0]]
            if controller.strip("/") else [],
        },
        "artifact_security": {
            "allowlist": [],
            "sandbox": {"kind": ""},
        },
        "authorization_summary": {
            "users": [{"identity": user_identity, "services": [service_name]}],
            "providers": [
                {"identity": provider["identity"], "services": [{
                    "service": service_name,
                    "roles": provider["roles"],
                }]}
                for provider in providers
            ],
        },
        "services": [{
            "name": service_name,
            "model": service_name.rstrip("/") + "/Model/v1",
            "users": [user_identity],
            "providers": providers,
            "roles": roles,
            "dependencies": _linear_dependencies(roles),
            "artifacts": artifacts,
            "input": {"codec": "npz"},
            "output": {"codec": "npz"},
        }],
    }


def _linear_dependencies(roles: list[str]) -> list[dict[str, Any]]:
    dependencies = []
    for index in range(len(roles) - 1):
        dependencies.append({
            "producers": [roles[index]],
            "consumers": [roles[index + 1]],
            "key_scope": f"stage{index}-to-stage{index + 1}",
            "topic_prefix": "/activation",
        })
    return dependencies


def policy_to_yaml(policy: dict[str, Any]) -> str:
    try:
        import yaml  # type: ignore
    except ImportError:
        return json.dumps(policy, indent=2)
    return yaml.safe_dump(policy, sort_keys=False)


class TextPane(ttk.Frame):
    def __init__(self, parent, *, height: int = 20):
        super().__init__(parent)
        self.text = tk.Text(self, wrap="word", height=height)
        scroll = ttk.Scrollbar(self, orient="vertical", command=self.text.yview)
        self.text.configure(yscrollcommand=scroll.set)
        self.text.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")

    def set(self, value: str) -> None:
        self.text.delete("1.0", "end")
        self.text.insert("1.0", value)

    def get(self) -> str:
        return self.text.get("1.0", "end-1c")


class WizardTab(ttk.Frame):
    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.fields: dict[str, tk.StringVar] = {}
        self._build()

    def _field(self, row: int, label: str, key: str, value: str = "",
               *, browse: bool = False) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        var = tk.StringVar(value=value)
        self.fields[key] = var
        entry = ttk.Entry(self, textvariable=var)
        entry.grid(row=row, column=1, sticky="ew", padx=6, pady=4)
        if browse:
            ttk.Button(self, text="Browse", command=lambda: self._browse_file(key)).grid(
                row=row, column=2, sticky="ew", padx=6, pady=4)

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        self._field(0, "Model file", "model", "", browse=True)
        self._field(1, "Application", "application", "di-gui-project")
        self._field(2, "Service name", "service", "/AI/Model/Inference")
        self._field(3, "Controller", "controller", "/NDNSF-DistributeInference/example/controller")
        self._field(4, "Group", "group", "/NDNSF-DistributeInference/example/group")
        self._field(5, "Runtime user identity", "user", "/NDNSF-DistributeInference/example/user")
        self._field(6, "Provider prefix", "provider_prefix", "/NDNSF-DistributeInference/example/provider")
        self._field(7, "Provider IDs", "provider_ids", "A,B")
        self._field(8, "Roles", "roles", "/Stage/0,/Stage/1")
        self._field(9, "Output policy", "output", "examples/python/NDNSF-DistributedInference/gui_policy.yaml",
                    browse=True)
        buttons = ttk.Frame(self)
        buttons.grid(row=10, column=0, columnspan=3, sticky="ew", padx=6, pady=8)
        ttk.Button(buttons, text="Generate Policy", command=self.generate_policy).pack(side="left")
        ttk.Button(buttons, text="Open in Policy Editor",
                   command=self.generate_to_editor).pack(side="left", padx=6)
        self.preview = TextPane(self, height=18)
        self.preview.grid(row=11, column=0, columnspan=3, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(11, weight=1)

    def _browse_file(self, key: str) -> None:
        if key == "output":
            path = filedialog.asksaveasfilename(
                title="Policy YAML",
                defaultextension=".yaml",
                filetypes=[("YAML", "*.yaml *.yml"), ("JSON", "*.json"), ("All files", "*")],
            )
        else:
            path = filedialog.askopenfilename(title="Select file")
        if path:
            self.fields[key].set(path)

    def _policy_text(self) -> str:
        policy = make_basic_policy(
            application=self.fields["application"].get(),
            controller=self.fields["controller"].get(),
            group=self.fields["group"].get(),
            user_identity=self.fields["user"].get(),
            provider_prefix=self.fields["provider_prefix"].get(),
            service_name=self.fields["service"].get(),
            provider_ids=[item.strip() for item in self.fields["provider_ids"].get().split(",")],
            roles=[item.strip() for item in self.fields["roles"].get().split(",") if item.strip()],
            model_path=self.fields["model"].get(),
        )
        return policy_to_yaml(policy)

    def generate_policy(self) -> None:
        text = self._policy_text()
        output = self.fields["output"].get()
        if output:
            write_policy_text(output, text)
            self.app.set_status(f"Generated policy: {output}")
        self.preview.set(text)

    def generate_to_editor(self) -> None:
        text = self._policy_text()
        self.preview.set(text)
        self.app.policy_editor.set_policy_text(text)
        output = self.fields["output"].get()
        if output:
            self.app.policy_editor.path_var.set(output)
        self.app.select_tab("Policy Editor")


class PolicyEditorTab(ttk.Frame):
    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.path_var = tk.StringVar(value=str(DEFAULT_POLICY))
        self._build()

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        self.rowconfigure(1, weight=1)
        top = ttk.Frame(self)
        top.grid(row=0, column=0, columnspan=3, sticky="ew", padx=6, pady=4)
        ttk.Label(top, text="Policy").pack(side="left")
        ttk.Entry(top, textvariable=self.path_var).pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(top, text="Open", command=self.open_policy).pack(side="left")
        ttk.Button(top, text="Save", command=self.save_policy).pack(side="left", padx=4)
        ttk.Button(top, text="Validate", command=self.validate_policy).pack(side="left")
        ttk.Button(top, text="Explain", command=self.explain_policy).pack(side="left", padx=4)

        self.tree = ttk.Treeview(self, columns=("kind",), show="tree")
        self.tree.grid(row=1, column=0, sticky="nsew", padx=6, pady=6)
        self.editor = TextPane(self)
        self.editor.grid(row=1, column=1, sticky="nsew", padx=6, pady=6)
        self.summary = TextPane(self)
        self.summary.grid(row=1, column=2, sticky="nsew", padx=6, pady=6)
        self.columnconfigure(0, weight=1)
        self.columnconfigure(1, weight=3)
        self.columnconfigure(2, weight=2)
        self.open_policy()

    def set_policy_text(self, text: str) -> None:
        self.editor.set(text)
        self.refresh_tree_from_text(text)

    def open_policy(self) -> None:
        path = self.path_var.get()
        if not path or not Path(path).exists():
            path = filedialog.askopenfilename(
                title="Open policy",
                filetypes=[("YAML/JSON", "*.yaml *.yml *.json"), ("All files", "*")],
            )
            if not path:
                return
            self.path_var.set(path)
        text = load_policy_text(path)
        self.set_policy_text(text)
        self.app.set_status(f"Loaded policy: {path}")

    def save_policy(self) -> None:
        path = self.path_var.get() or filedialog.asksaveasfilename(
            title="Save policy",
            defaultextension=".yaml",
        )
        if not path:
            return
        write_policy_text(path, self.editor.get())
        self.path_var.set(path)
        self.app.set_status(f"Saved policy: {path}")

    def validate_policy(self) -> None:
        self.save_policy()
        try:
            summary = summarize_policy_file(self.path_var.get())
        except Exception as exc:
            self.summary.set(f"Validation failed:\n{exc}")
            messagebox.showerror("Policy validation failed", str(exc))
            return
        self.summary.set(summary)
        self.refresh_tree_from_path(self.path_var.get())
        self.app.set_status("Policy validation passed")

    def explain_policy(self) -> None:
        self.validate_policy()

    def refresh_tree_from_text(self, text: str) -> None:
        tmp = Path(tempfile.mkdtemp(prefix="ndnsf-di-gui-open-")) / "policy.yaml"
        tmp.write_text(text, encoding="utf-8")
        self.refresh_tree_from_path(tmp)

    def refresh_tree_from_path(self, path: str | Path) -> None:
        for item in self.tree.get_children():
            self.tree.delete(item)
        try:
            config = load_config(path)
        except Exception:
            return
        users_node = self.tree.insert("", "end", text="Users", open=True)
        providers_node = self.tree.insert("", "end", text="Providers", open=True)
        services_node = self.tree.insert("", "end", text="Services", open=True)
        users = set()
        providers = set()
        for service in config.get("services", []) or []:
            if not isinstance(service, dict):
                continue
            service_node = self.tree.insert(services_node, "end", text=str(service.get("name", "")))
            for role in service.get("roles", []) or []:
                self.tree.insert(service_node, "end", text=f"role {role}")
            for user in service.get("users", []) or []:
                users.add(str(user))
            for provider in service.get("providers", []) or []:
                if isinstance(provider, dict):
                    providers.add(str(provider.get("identity", "")))
        for user in sorted(users):
            self.tree.insert(users_node, "end", text=user)
        for provider in sorted(providers):
            self.tree.insert(providers_node, "end", text=provider)


class ModelSplitTab(ttk.Frame):
    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.model_var = tk.StringVar()
        self._build()

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        self.rowconfigure(1, weight=1)
        top = ttk.Frame(self)
        top.grid(row=0, column=0, columnspan=2, sticky="ew", padx=6, pady=4)
        ttk.Label(top, text="ONNX model").pack(side="left")
        ttk.Entry(top, textvariable=self.model_var).pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(top, text="Browse", command=self.browse).pack(side="left")
        ttk.Button(top, text="Analyze", command=self.analyze).pack(side="left", padx=4)
        ttk.Button(top, text="Use Top 2-Stage Policy Skeleton",
                   command=self.use_two_stage).pack(side="left")
        self.output = TextPane(self)
        self.output.grid(row=1, column=0, columnspan=2, sticky="nsew", padx=6, pady=6)

    def browse(self) -> None:
        path = filedialog.askopenfilename(
            title="Select ONNX model",
            filetypes=[("ONNX", "*.onnx"), ("All files", "*")],
        )
        if path:
            self.model_var.set(path)

    def analyze(self) -> None:
        path = self.model_var.get()
        if not path:
            messagebox.showwarning("Missing model", "Select an ONNX model first.")
            return
        try:
            summary = analyze_onnx_graph(path)
            candidates = estimate_split_candidates(summary, max_candidates=10)
        except Exception as exc:
            self.output.set(f"ONNX analysis failed:\n{exc}")
            return
        lines = [
            f"Model: {path}",
            f"Inputs: {', '.join(summary.inputs)}",
            f"Outputs: {', '.join(summary.outputs)}",
            f"Nodes: {len(summary.nodes)}",
            "",
            "Top split candidates:",
        ]
        for candidate in candidates[:10]:
            lines.append(
                f"cut_after_node={candidate.cut_after_node} "
                f"boundary_tensors={len(candidate.boundary_tensors)} "
                f"known_boundary_bytes={candidate.known_boundary_bytes} "
                f"unknown_size_tensors={len(candidate.unknown_size_tensors)}")
        self.output.set("\n".join(lines))

    def use_two_stage(self) -> None:
        self.app.wizard.fields["model"].set(self.model_var.get())
        self.app.wizard.fields["roles"].set("/Stage/0,/Stage/1")
        self.app.select_tab("Project Wizard")


class CertificateTab(ttk.Frame):
    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.identity_var = tk.StringVar()
        self.request_path_var = tk.StringVar(value="/tmp/ndnsf-di-identity.req")
        self.safebag_var = tk.StringVar()
        self.password_var = tk.StringVar()
        self._build()

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        row = 0
        ttk.Button(self, text="Refresh ndnsec list", command=self.refresh).grid(
            row=row, column=0, sticky="ew", padx=6, pady=4)
        ttk.Button(self, text="Use Selected As Runtime User",
                   command=self.use_selected_identity).grid(
            row=row, column=1, sticky="ew", padx=6, pady=4)
        row += 1
        self.identities = tk.Listbox(self, height=8)
        self.identities.grid(row=row, column=0, columnspan=2, sticky="nsew", padx=6, pady=4)
        self.rowconfigure(row, weight=1)
        row += 1
        ttk.Label(self, text="Identity").grid(row=row, column=0, sticky="w", padx=6)
        ttk.Entry(self, textvariable=self.identity_var).grid(row=row, column=1, sticky="ew", padx=6)
        row += 1
        ttk.Label(self, text="Key request output").grid(row=row, column=0, sticky="w", padx=6)
        ttk.Entry(self, textvariable=self.request_path_var).grid(row=row, column=1, sticky="ew", padx=6)
        row += 1
        ttk.Button(self, text="Generate Key Request",
                   command=self.generate_key_request).grid(row=row, column=0, columnspan=2,
                                                           sticky="ew", padx=6, pady=4)
        row += 1
        ttk.Label(self, text="Safebag file").grid(row=row, column=0, sticky="w", padx=6)
        ttk.Entry(self, textvariable=self.safebag_var).grid(row=row, column=1, sticky="ew", padx=6)
        row += 1
        ttk.Label(self, text="Safebag password").grid(row=row, column=0, sticky="w", padx=6)
        ttk.Entry(self, textvariable=self.password_var, show="*").grid(row=row, column=1,
                                                                       sticky="ew", padx=6)
        row += 1
        ttk.Button(self, text="Import Safebag",
                   command=self.import_safebag).grid(row=row, column=0, columnspan=2,
                                                     sticky="ew", padx=6, pady=4)
        row += 1
        self.output = TextPane(self, height=10)
        self.output.grid(row=row, column=0, columnspan=2, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(row, weight=1)
        self.refresh()

    def refresh(self) -> None:
        code, output = run_command(["ndnsec", "list"])
        self.output.set(output or f"ndnsec list exited with {code}")
        self.identities.delete(0, "end")
        for line in output.splitlines():
            value = line.strip()
            if value.startswith("/"):
                self.identities.insert("end", value.split()[0])

    def _selected_identity(self) -> str:
        selection = self.identities.curselection()
        if selection:
            return self.identities.get(selection[0])
        return self.identity_var.get()

    def use_selected_identity(self) -> None:
        identity = self._selected_identity()
        if identity:
            self.identity_var.set(identity)
            self.app.wizard.fields["user"].set(identity)
            self.app.set_status(f"Selected runtime user identity: {identity}")

    def generate_key_request(self) -> None:
        identity = self.identity_var.get()
        output_path = self.request_path_var.get()
        if not identity or not output_path:
            messagebox.showwarning("Missing fields", "Identity and output path are required.")
            return
        code, output = run_command(["ndnsec", "key-gen", "-n", "-t", "r", identity])
        if code == 0:
            write_policy_text(output_path, output)
            self.output.set(f"Wrote key request to {output_path}\n\n{output}")
        else:
            self.output.set(output)

    def import_safebag(self) -> None:
        safebag = self.safebag_var.get()
        password = self.password_var.get()
        if not safebag or not password:
            messagebox.showwarning("Missing fields", "Safebag file and password are required.")
            return
        code, output = run_command(["ndnsec", "import", "-P", password, safebag])
        self.output.set(output or f"ndnsec import exited with {code}")
        self.refresh()


class DeploymentRunnerTab(ttk.Frame):
    REGRESSION_CASES = {
        "auto-split": ("YOLO_SPLIT_RESULT", "ok=true"),
        "yolo-2x2": ("YOLO_2X2_RESULT", "ok=true"),
        "yolo-layout": ("YOLO_LAYOUT_DYNAMIC_PROVISIONING_MININDN_OK", ""),
        "yolo-layout-local": ("YOLO_LAYOUT_SMOKE_OK", ""),
        "onnx-executor": ("ONNX_EXECUTOR_FANIN_FANOUT_OK", ""),
        "app-api": ("APP_API_SERVICE_PLAN_OK", ""),
        "all": ("NDNSF_DI_REGRESSION_SUITE_OK", ""),
    }

    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.config_var = tk.StringVar(value=str(DEFAULT_POLICY))
        self.provider_id_var = tk.StringVar(value="A")
        self.regression_case_var = tk.StringVar(value="yolo-2x2")
        self.profile_path_var = tk.StringVar(value="examples/python/NDNSF-DistributedInference/gui_runtime_profile.json")
        self.processes: dict[str, subprocess.Popen[str]] = {}
        self.process_states: dict[str, RoleProcessState] = {}
        self.status_callbacks: dict[str, Callable[[str], None]] = {}
        self.queue: queue.Queue[str | tuple[str, str] | tuple[str, str, str]] = queue.Queue()
        self._build()
        self.after(200, self._drain_queue)

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        ttk.Label(self, text="Config").grid(row=0, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(self, textvariable=self.config_var).grid(row=0, column=1, sticky="ew", padx=6)
        ttk.Button(self, text="Browse", command=self.browse).grid(row=0, column=2, padx=6)
        ttk.Label(self, text="Provider ID").grid(row=1, column=0, sticky="w", padx=6)
        ttk.Entry(self, textvariable=self.provider_id_var).grid(row=1, column=1, sticky="ew", padx=6)
        ttk.Label(self, text="Regression").grid(row=2, column=0, sticky="w", padx=6, pady=4)
        ttk.Combobox(
            self,
            textvariable=self.regression_case_var,
            values=list(self.REGRESSION_CASES.keys()),
            state="readonly",
        ).grid(row=2, column=1, sticky="ew", padx=6, pady=4)
        buttons = ttk.Frame(self)
        buttons.grid(row=3, column=0, columnspan=3, sticky="ew", padx=6, pady=4)
        ttk.Button(buttons, text="Run Controller", command=self.run_controller).pack(side="left")
        ttk.Button(buttons, text="Run Provider", command=self.run_provider).pack(side="left", padx=4)
        ttk.Button(buttons, text="Run User", command=self.run_user).pack(side="left")
        ttk.Button(buttons, text="Run Selected Regression",
                   command=self.run_selected_regression).pack(side="left", padx=4)
        ttk.Button(buttons, text="Run YOLO 2x2 MiniNDN Smoke",
                   command=self.run_yolo_2x2_smoke).pack(side="left", padx=4)
        ttk.Button(buttons, text="Stop Processes", command=self.stop_processes).pack(side="left")
        profile = ttk.Frame(self)
        profile.grid(row=4, column=0, columnspan=3, sticky="ew", padx=6, pady=4)
        ttk.Label(profile, text="Runtime profile").pack(side="left")
        ttk.Entry(profile, textvariable=self.profile_path_var).pack(side="left", fill="x",
                                                                    expand=True, padx=6)
        ttk.Button(profile, text="Load Profile", command=self.load_profile).pack(side="left")
        ttk.Button(profile, text="Save Profile", command=self.save_profile).pack(side="left", padx=4)
        ttk.Button(profile, text="Start All", command=self.start_all).pack(side="left")
        ttk.Button(profile, text="Stop All", command=self.stop_processes).pack(side="left", padx=4)
        ttk.Button(profile, text="Clear Logs", command=self.clear_logs).pack(side="left")
        self.log = TextPane(self, height=25)
        self.log.grid(row=5, column=0, columnspan=3, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(5, weight=1)

    def browse(self) -> None:
        path = filedialog.askopenfilename(
            title="Select config",
            filetypes=[("YAML/JSON", "*.yaml *.yml *.json"), ("All files", "*")],
        )
        if path:
            self.config_var.set(path)

    def _script_for_config(self, role: str) -> list[str]:
        config = self.config_var.get()
        if "yolo_split" in config:
            base = "examples/python/NDNSF-DistributedInference/yolo_split"
        elif "pytorch_eager_2x2" in config:
            base = "examples/python/NDNSF-DistributedInference/pytorch_eager_2x2"
        else:
            base = "examples/python/NDNSF-DistributedInference/yolo_2x2"
        script = repo_root() / base / f"{role}.py"
        args = [sys.executable, str(script), "--config", config]
        if role == "provider":
            args.extend(["--provider-id", self.provider_id_var.get()])
        return args

    def run_controller(self) -> None:
        self._start(self._script_for_config("controller"), "controller")

    def run_provider(self) -> None:
        self._start(self._script_for_config("provider"), "provider")

    def run_user(self) -> None:
        self._start(self._script_for_config("user"), "user")

    def run_selected_regression(self) -> None:
        case = self.regression_case_var.get()
        self._append(
            f"Running DI regression case '{case}'. The selected case starts "
            "MiniNDN, runs distributed inference, and checks its success marker.\n"
        )
        self._start(
            [sys.executable, "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
             "--case", case],
            f"regression-{case}",
            success_markers=self.REGRESSION_CASES.get(case),
        )

    def run_yolo_2x2_smoke(self) -> None:
        self._start([sys.executable, "Experiments/NDNSF_DI_Yolo2x2_Minindn.py"],
                    "yolo-2x2-minindn",
                    success_markers=("YOLO_2X2_RESULT", "ok=true"))

    def register_status_callback(self, label: str, callback: Callable[[str], None]) -> None:
        self.status_callbacks[label] = callback

    def _start(self, args: list[str], label: str,
               success_markers: tuple[str, str] | None = None) -> None:
        if label in self.processes and self.processes[label].poll() is None:
            self.queue.put(("__STATUS__", f"{label} is already running"))
            self.queue.put(("__ROLE_STATUS__", label, "running"))
            return
        self._append(f"$ {' '.join(args)}\n")
        state = self.process_states.setdefault(label, RoleProcessState(label))
        self.queue.put(("__ROLE_STATUS__", label, state.mark_starting()))
        proc = subprocess.Popen(
            args,
            cwd=str(repo_root()),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.processes[label] = proc
        self.queue.put(("__ROLE_STATUS__", label, state.mark_running(proc.pid)))
        threading.Thread(
            target=self._read_process,
            args=(proc, label, success_markers),
            daemon=True,
        ).start()

    def _read_process(self, proc: subprocess.Popen[str], label: str,
                      success_markers: tuple[str, str] | None = None) -> None:
        assert proc.stdout is not None
        saw_first = False
        saw_second = False
        for line in proc.stdout:
            self.queue.put(f"[{label}] {line}")
            if success_markers is not None:
                first, second = success_markers
                saw_first = saw_first or bool(first and first in line)
                saw_second = saw_second or not second or second in line
        proc.wait()
        self.queue.put(f"[{label}] exited with {proc.returncode}\n")
        state = self.process_states.setdefault(label, RoleProcessState(label))
        self.queue.put(("__ROLE_STATUS__", label, state.mark_exited(proc.returncode)))
        if success_markers is not None:
            if proc.returncode == 0 and saw_first and saw_second:
                self.queue.put(f"[{label}] RESULT ok=true\n")
                self.queue.put(("__STATUS__", f"{label} completed: ok=true"))
            else:
                self.queue.put(
                    f"[{label}] RESULT ok=false expected={success_markers} "
                    f"returncode={proc.returncode}\n"
                )
                self.queue.put(("__STATUS__", f"{label} completed: ok=false"))

    def _drain_queue(self) -> None:
        while True:
            try:
                item = self.queue.get_nowait()
            except queue.Empty:
                break
            if isinstance(item, tuple) and item and item[0] == "__STATUS__":
                self.app.set_status(str(item[1]))
            elif isinstance(item, tuple) and item and item[0] == "__ROLE_STATUS__":
                _, label, status = item
                callback = self.status_callbacks.get(str(label))
                if callback is not None:
                    callback(str(status))
            else:
                self._append(str(item))
        self.after(200, self._drain_queue)

    def _append(self, text: str) -> None:
        self.log.text.insert("end", text)
        self.log.text.see("end")

    def stop_processes(self) -> None:
        for label, proc in list(self.processes.items()):
            if proc.poll() is None:
                state = self.process_states.setdefault(label, RoleProcessState(label))
                self.queue.put(("__ROLE_STATUS__", label, state.mark_stopping()))
                proc.terminate()
        self.processes = {
            label: proc for label, proc in self.processes.items()
            if proc.poll() is None
        }
        self._append("Stop requested for running processes.\n")

    def stop_role(self, label: str) -> None:
        proc = self.processes.get(label)
        if proc is None or proc.poll() is not None:
            self.queue.put(("__ROLE_STATUS__", label, "stopped"))
            return
        state = self.process_states.setdefault(label, RoleProcessState(label))
        self.queue.put(("__ROLE_STATUS__", label, state.mark_stopping()))
        proc.terminate()
        self._append(f"Stop requested for {label}.\n")

    def clear_logs(self) -> None:
        self.log.set("")

    def start_all(self) -> None:
        self.app.controller_runtime.run_role()
        self.app.provider_runtime.run_role()
        self.app.user_runtime.run_role()

    def profile(self) -> RuntimeGuiProfile:
        return RuntimeGuiProfile(
            controller=self.app.controller_runtime.profile(),
            provider=self.app.provider_runtime.profile(),
            user=self.app.user_runtime.profile(),
        )

    def apply_profile(self, profile: RuntimeGuiProfile) -> None:
        self.app.controller_runtime.apply_profile(profile.controller)
        self.app.provider_runtime.apply_profile(profile.provider)
        self.app.user_runtime.apply_profile(profile.user)
        self.config_var.set(profile.provider.config)
        self.provider_id_var.set(profile.provider.provider_id)
        self.app.set_status("Runtime profile loaded")

    def load_profile(self) -> None:
        path = self.profile_path_var.get().strip()
        if not path or not Path(path).exists():
            path = filedialog.askopenfilename(
                title="Load runtime profile",
                filetypes=[("JSON", "*.json"), ("All files", "*")],
            )
            if not path:
                return
            self.profile_path_var.set(path)
        try:
            self.apply_profile(load_runtime_profile(path))
        except Exception as exc:
            messagebox.showerror("Load profile failed", str(exc))

    def save_profile(self) -> None:
        path = self.profile_path_var.get().strip()
        if not path:
            path = filedialog.asksaveasfilename(
                title="Save runtime profile",
                defaultextension=".json",
                filetypes=[("JSON", "*.json"), ("All files", "*")],
            )
            if not path:
                return
            self.profile_path_var.set(path)
        try:
            write_runtime_profile(path, self.profile())
        except Exception as exc:
            messagebox.showerror("Save profile failed", str(exc))
            return
        self.app.set_status(f"Saved runtime profile: {path}")


class ControllerCertificateFrame(ttk.LabelFrame):
    """Controller-side root and certificate signing helper.

    This widget intentionally wraps ndnsec commands instead of inventing a new
    certificate format. Operators can paste a key request from a User/Provider
    tab, sign it with the controller/root identity, and copy the signed
    certificate back to the requester.
    """

    def __init__(self, parent):
        super().__init__(parent, text="Controller certificate authority")
        self.root_identity_var = tk.StringVar(value="/NDNSF-DistributeInference/example")
        self.root_cert_path_var = tk.StringVar(value="/tmp/ndnsf-di-root.cert")
        self.issuer_id_var = tk.StringVar(value="ROOT")
        self.request_path_var = tk.StringVar()
        self.signed_cert_path_var = tk.StringVar(value="/tmp/ndnsf-di-signed.cert")
        self._build()

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        row = 0
        self._entry(row, "Root identity", self.root_identity_var)
        row += 1
        self._entry(row, "Root cert output", self.root_cert_path_var, browse_save=True)
        row += 1
        ttk.Button(self, text="Generate / Refresh Root Cert",
                   command=self.generate_root_cert).grid(row=row, column=0, columnspan=3,
                                                         sticky="ew", padx=6, pady=4)
        row += 1
        self._entry(row, "Issuer ID", self.issuer_id_var)
        row += 1
        self._entry(row, "Key request file", self.request_path_var, browse_open=True)
        row += 1
        ttk.Label(self, text="Pasted key request").grid(row=row, column=0, sticky="nw", padx=6)
        self.request_text = tk.Text(self, height=5, wrap="word")
        self.request_text.grid(row=row, column=1, columnspan=2, sticky="nsew", padx=6, pady=4)
        row += 1
        self._entry(row, "Signed cert output", self.signed_cert_path_var, browse_save=True)
        row += 1
        ttk.Button(self, text="Sign Request",
                   command=self.sign_request).grid(row=row, column=0, columnspan=3,
                                                   sticky="ew", padx=6, pady=4)
        row += 1
        ttk.Label(self, text="Signed certificate").grid(row=row, column=0, sticky="nw", padx=6)
        self.output_text = tk.Text(self, height=5, wrap="word")
        self.output_text.grid(row=row, column=1, columnspan=2, sticky="nsew", padx=6, pady=4)
        self.rowconfigure(row, weight=1)

    def _entry(self, row: int, label: str, variable: tk.StringVar,
               *, browse_open: bool = False, browse_save: bool = False) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(self, textvariable=variable).grid(row=row, column=1, sticky="ew", padx=6, pady=4)
        if browse_open:
            ttk.Button(self, text="Browse", command=lambda: self._browse_open(variable)).grid(
                row=row, column=2, padx=6, pady=4)
        elif browse_save:
            ttk.Button(self, text="Save As", command=lambda: self._browse_save(variable)).grid(
                row=row, column=2, padx=6, pady=4)

    def _browse_open(self, variable: tk.StringVar) -> None:
        path = filedialog.askopenfilename(title="Select key request")
        if path:
            variable.set(path)
            try:
                self.request_text.delete("1.0", "end")
                self.request_text.insert("1.0", read_text_file(path))
            except OSError:
                pass

    def _browse_save(self, variable: tk.StringVar) -> None:
        path = filedialog.asksaveasfilename(title="Select output file")
        if path:
            variable.set(path)

    def generate_root_cert(self) -> None:
        identity = self.root_identity_var.get().strip()
        output_path = self.root_cert_path_var.get().strip()
        if not identity or not output_path:
            messagebox.showwarning("Missing fields", "Root identity and output path are required.")
            return
        code, output = run_command(["ndnsec", "key-gen", "-t", "r", identity])
        if code == 0:
            write_text_file(output_path, output)
            self._set_output(f"Wrote root cert to {output_path}\n\n{output}")
        else:
            self._set_output(output or f"ndnsec key-gen exited with {code}")

    def sign_request(self) -> None:
        signer = self.root_identity_var.get().strip()
        issuer = self.issuer_id_var.get().strip() or "ROOT"
        output_path = self.signed_cert_path_var.get().strip()
        request_text = self.request_text.get("1.0", "end-1c").strip()
        request_path = self.request_path_var.get().strip()
        if not signer or not output_path:
            messagebox.showwarning("Missing fields", "Root identity and signed cert output are required.")
            return
        if request_text:
            tmp = tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False,
                                             prefix="ndnsf-di-csr-", suffix=".req")
            with tmp:
                tmp.write(request_text)
            request_path = tmp.name
        if not request_path:
            messagebox.showwarning("Missing request", "Paste a request or select a request file.")
            return
        code, output = run_command(["ndnsec", "cert-gen", "-s", signer, "-i", issuer, request_path])
        if code == 0:
            write_text_file(output_path, output)
            self._set_output(f"Wrote signed cert to {output_path}\n\n{output}")
        else:
            self._set_output(output or f"ndnsec cert-gen exited with {code}")

    def _set_output(self, value: str) -> None:
        self.output_text.delete("1.0", "end")
        self.output_text.insert("1.0", value)


class ParticipantCertificateFrame(ttk.LabelFrame):
    """User/Provider-side key request and certificate install helper."""

    def __init__(self, parent, role: str):
        super().__init__(parent, text=f"{role.title()} certificate request / install")
        role_suffix = role.lower()
        self.identity_var = tk.StringVar(value=f"/NDNSF-DistributeInference/example/{role_suffix}")
        self.request_path_var = tk.StringVar(value=f"/tmp/ndnsf-di-{role_suffix}.req")
        self.cert_path_var = tk.StringVar(value=f"/tmp/ndnsf-di-{role_suffix}.cert")
        self._build()

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        row = 0
        self._entry(row, "Identity", self.identity_var)
        row += 1
        self._entry(row, "Key request output", self.request_path_var, browse_save=True)
        row += 1
        ttk.Button(self, text="Generate Key Request",
                   command=self.generate_key_request).grid(row=row, column=0, columnspan=3,
                                                           sticky="ew", padx=6, pady=4)
        row += 1
        ttk.Label(self, text="Copy this request to Controller").grid(row=row, column=0,
                                                                     sticky="nw", padx=6)
        self.request_text = tk.Text(self, height=5, wrap="word")
        self.request_text.grid(row=row, column=1, columnspan=2, sticky="nsew", padx=6, pady=4)
        row += 1
        self._entry(row, "Signed cert file", self.cert_path_var, browse_open=True)
        row += 1
        ttk.Label(self, text="Or paste signed cert").grid(row=row, column=0, sticky="nw", padx=6)
        self.cert_text = tk.Text(self, height=5, wrap="word")
        self.cert_text.grid(row=row, column=1, columnspan=2, sticky="nsew", padx=6, pady=4)
        row += 1
        ttk.Button(self, text="Install Signed Cert",
                   command=self.install_signed_cert).grid(row=row, column=0, columnspan=3,
                                                          sticky="ew", padx=6, pady=4)
        row += 1
        self.status = tk.StringVar(value="")
        ttk.Label(self, textvariable=self.status, anchor="w").grid(row=row, column=0,
                                                                   columnspan=3, sticky="ew",
                                                                   padx=6, pady=4)

    def _entry(self, row: int, label: str, variable: tk.StringVar,
               *, browse_open: bool = False, browse_save: bool = False) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(self, textvariable=variable).grid(row=row, column=1, sticky="ew", padx=6, pady=4)
        if browse_open:
            ttk.Button(self, text="Browse", command=lambda: self._browse_open(variable)).grid(
                row=row, column=2, padx=6, pady=4)
        elif browse_save:
            ttk.Button(self, text="Save As", command=lambda: self._browse_save(variable)).grid(
                row=row, column=2, padx=6, pady=4)

    def _browse_open(self, variable: tk.StringVar) -> None:
        path = filedialog.askopenfilename(title="Select signed certificate")
        if path:
            variable.set(path)
            try:
                self.cert_text.delete("1.0", "end")
                self.cert_text.insert("1.0", read_text_file(path))
            except OSError:
                pass

    def _browse_save(self, variable: tk.StringVar) -> None:
        path = filedialog.asksaveasfilename(title="Select output file")
        if path:
            variable.set(path)

    def generate_key_request(self) -> None:
        identity = self.identity_var.get().strip()
        output_path = self.request_path_var.get().strip()
        if not identity or not output_path:
            messagebox.showwarning("Missing fields", "Identity and output path are required.")
            return
        code, output = run_command(["ndnsec", "key-gen", "-n", "-t", "r", identity])
        if code == 0:
            write_text_file(output_path, output)
            self.request_text.delete("1.0", "end")
            self.request_text.insert("1.0", output)
            self.status.set(f"Wrote key request to {output_path}")
        else:
            self.status.set(output or f"ndnsec key-gen exited with {code}")

    def install_signed_cert(self) -> None:
        cert_text = self.cert_text.get("1.0", "end-1c").strip()
        cert_path = self.cert_path_var.get().strip()
        if cert_text:
            write_text_file(cert_path, cert_text)
        if not cert_path:
            messagebox.showwarning("Missing certificate", "Paste or select a signed certificate.")
            return
        code, output = run_command(["ndnsec", "cert-install", "-f", cert_path])
        self.status.set(output or f"ndnsec cert-install exited with {code}")


class RoleRuntimeTab(ttk.Frame):
    """Role-specific APP runtime launcher.

    One physical node can run any combination of these roles. The tab only
    prepares and launches the corresponding APP-level process; permissions,
    identities, artifacts, and service dependency graph still come from the
    selected policy file.
    """

    EXAMPLE_BASES = {
        "YOLO 2-stage": "examples/python/NDNSF-DistributedInference/yolo_split",
        "YOLO 2x2": "examples/python/NDNSF-DistributedInference/yolo_2x2",
        "PyTorch 2x2": "examples/python/NDNSF-DistributedInference/pytorch_eager_2x2",
    }

    def __init__(self, parent, app: "DistributedInferenceGui", role: str):
        super().__init__(parent)
        self.app = app
        self.role = role
        self.config_var = tk.StringVar(value=str(DEFAULT_POLICY))
        self.example_var = tk.StringVar(value="YOLO 2x2")
        self.generated_dir_var = tk.StringVar(value="/tmp/ndnsf-di-gui-policy")
        self.group_var = tk.StringVar(value="")
        self.provider_id_var = tk.StringVar(value="A")
        self.roles_var = tk.StringVar(value="all")
        self.service_var = tk.StringVar(value="/AI/YOLO/2x2Inference")
        self.ack_timeout_var = tk.StringVar(value="1500")
        self.timeout_var = tk.StringVar(value="60000")
        self.extra_args_var = tk.StringVar(value="")
        self.status_var = tk.StringVar(value="stopped")
        if role == "controller":
            self.identity_tools: ttk.Widget = ControllerCertificateFrame(self)
        else:
            self.identity_tools = ParticipantCertificateFrame(self, role)
        self._build()
        self.app.runner.register_status_callback(role, self.status_var.set)

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        row = 0
        ttk.Label(self, text=f"{self.role.title()} runtime").grid(
            row=row, column=0, columnspan=3, sticky="w", padx=6, pady=6)
        row += 1
        self._entry(row, "Policy config", self.config_var, browse=True)
        row += 1
        self._combo(row, "Example app", self.example_var, list(self.EXAMPLE_BASES))
        row += 1
        self._entry(row, "Generated policy dir", self.generated_dir_var)
        row += 1
        self._entry(row, "SVS group override", self.group_var)
        row += 1

        if self.role == "controller":
            self._entry(row, "Extra controller args", self.extra_args_var)
            row += 1
        elif self.role == "provider":
            self._entry(row, "Provider ID", self.provider_id_var)
            row += 1
            self._entry(row, "Roles", self.roles_var)
            row += 1
            self._entry(row, "Extra provider args", self.extra_args_var)
            row += 1
        else:
            self._entry(row, "Service name (policy reference)", self.service_var)
            row += 1
            self._entry(row, "ACK timeout ms", self.ack_timeout_var)
            row += 1
            self._entry(row, "Total timeout ms", self.timeout_var)
            row += 1
            self._entry(row, "Extra user args", self.extra_args_var)
            row += 1

        buttons = ttk.Frame(self)
        buttons.grid(row=row, column=0, columnspan=3, sticky="ew", padx=6, pady=8)
        ttk.Button(buttons, text=f"Start {self.role.title()}",
                   command=self.run_role).pack(side="left")
        ttk.Button(buttons, text="Stop",
                   command=self.stop_role).pack(side="left", padx=6)
        ttk.Button(buttons, text="Restart",
                   command=self.restart_role).pack(side="left")
        ttk.Button(buttons, text="Show Command",
                   command=self.show_command).pack(side="left", padx=6)
        ttk.Button(buttons, text="Open Deployment Logs",
                   command=lambda: self.app.select_tab("Deployment Runner")).pack(side="left")
        row += 1
        ttk.Label(self, text="Status").grid(row=row, column=0, sticky="w", padx=6)
        ttk.Label(self, textvariable=self.status_var, anchor="w").grid(
            row=row, column=1, columnspan=2, sticky="ew", padx=6, pady=4)
        row += 1

        self.output = TextPane(self, height=18)
        self.output.grid(row=row, column=0, columnspan=3, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(row, weight=1)
        row += 1

        self.identity_tools.grid(row=row, column=0, columnspan=3, sticky="nsew", padx=6, pady=6)

    def _entry(self, row: int, label: str, variable: tk.StringVar,
               *, browse: bool = False) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(self, textvariable=variable).grid(row=row, column=1, sticky="ew", padx=6, pady=4)
        if browse:
            ttk.Button(self, text="Browse", command=self._browse_config).grid(
                row=row, column=2, padx=6, pady=4)

    def _combo(self, row: int, label: str, variable: tk.StringVar,
               values: list[str]) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        ttk.Combobox(self, textvariable=variable, values=values, state="readonly").grid(
            row=row, column=1, sticky="ew", padx=6, pady=4)

    def _browse_config(self) -> None:
        path = filedialog.askopenfilename(
            title="Select policy config",
            filetypes=[("YAML/JSON", "*.yaml *.yml *.json"), ("All files", "*")],
        )
        if path:
            self.config_var.set(path)

    def _script_path(self) -> Path:
        base = self.EXAMPLE_BASES[self.example_var.get()]
        return repo_root() / base / f"{self.role}.py"

    def command(self) -> list[str]:
        return build_role_command(
            role=self.role,
            script_path=self._script_path(),
            config=self.config_var.get(),
            generated_policy_dir=self.generated_dir_var.get(),
            group=self.group_var.get(),
            provider_id=self.provider_id_var.get(),
            roles=self.roles_var.get(),
            ack_timeout_ms=self.ack_timeout_var.get(),
            timeout_ms=self.timeout_var.get(),
            extra_args=self.extra_args_var.get(),
        )

    def show_command(self) -> None:
        command = shlex.join(self.command())
        self.output.set(command)
        self.app.set_status(f"{self.role.title()} command prepared")

    def run_role(self) -> None:
        self.output.set("Starting through Deployment Runner log pane:\n" +
                        shlex.join(self.command()))
        self.app.runner._start(self.command(), self.role)
        self.app.select_tab("Deployment Runner")

    def stop_role(self) -> None:
        self.app.runner.stop_role(self.role)

    def restart_role(self) -> None:
        self.stop_role()
        self.after(300, self.run_role)

    def profile(self) -> RuntimeRoleProfile:
        return RuntimeRoleProfile(
            role=self.role,
            config=self.config_var.get(),
            example=self.example_var.get(),
            generated_policy_dir=self.generated_dir_var.get(),
            group=self.group_var.get(),
            provider_id=self.provider_id_var.get(),
            roles=self.roles_var.get(),
            service=self.service_var.get(),
            ack_timeout_ms=self.ack_timeout_var.get(),
            timeout_ms=self.timeout_var.get(),
            extra_args=self.extra_args_var.get(),
        )

    def apply_profile(self, profile: RuntimeRoleProfile) -> None:
        self.config_var.set(profile.config)
        if profile.example in self.EXAMPLE_BASES:
            self.example_var.set(profile.example)
        self.generated_dir_var.set(profile.generated_policy_dir)
        self.group_var.set(profile.group)
        self.provider_id_var.set(profile.provider_id)
        self.roles_var.set(profile.roles)
        self.service_var.set(profile.service)
        self.ack_timeout_var.set(profile.ack_timeout_ms)
        self.timeout_var.set(profile.timeout_ms)
        self.extra_args_var.set(profile.extra_args)
        self.status_var.set("stopped")


class JsonTextPane(TextPane):
    def get_json(self, *, default: Any) -> Any:
        return parse_json_field(self.get(), default=default)

    def set_json(self, value: Any) -> None:
        self.set(json.dumps(value, indent=2, sort_keys=True))


class DirectRoleTab(ttk.Frame):
    def __init__(self,
                 parent,
                 app: "DistributedInferenceGui",
                 role: str,
                 factory: RuntimeFactory | None = None) -> None:
        super().__init__(parent)
        self.app = app
        self.role = role
        self.queue: queue.Queue[str | tuple[str, str]] = queue.Queue()
        self.status_var = tk.StringVar(value="stopped")
        self.persist_tokens_var = tk.BooleanVar(value=False)
        self.env_config = NdnsfSvsEnvConfig()
        self._drain_after_id: str | None = None
        self.controller = RoleRuntimeController(
            role,
            factory=factory,
            log_callback=self._queue_log,
            status_callback=self._queue_status,
            env_config=self.env_config,
        )
        self.fields: dict[str, tk.StringVar | tk.BooleanVar] = {}
        self.advanced_json: dict[str, JsonTextPane] = {}
        self._build()
        self._drain_after_id = self.after(200, self._drain_queue)

    def _queue_log(self, message: str) -> None:
        self.queue.put(message)

    def _queue_status(self, status: str) -> None:
        self.queue.put(("status", status))

    def _drain_queue(self) -> None:
        while True:
            try:
                item = self.queue.get_nowait()
            except queue.Empty:
                break
            if isinstance(item, tuple) and item[0] == "status":
                self.status_var.set(item[1])
                self.app.set_status(f"{self.role.title()}: {item[1]}")
            elif isinstance(item, tuple) and item[0] == "response":
                response_pane = getattr(self, "response_pane", None)
                if response_pane is not None:
                    response_pane.set(item[1])
                else:
                    self.log(item[1])
            else:
                self.log(str(item))
        self._drain_after_id = self.after(200, self._drain_queue)

    def cancel_periodic_callbacks(self) -> None:
        if self._drain_after_id is None:
            return
        try:
            self.after_cancel(self._drain_after_id)
        except Exception:
            pass
        self._drain_after_id = None

    def destroy(self) -> None:
        self.cancel_periodic_callbacks()
        super().destroy()

    def _field(self, parent, row: int, label: str, key: str, value: str = "",
               *, browse: str = "") -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=3)
        var = tk.StringVar(value=value)
        self.fields[key] = var
        ttk.Entry(parent, textvariable=var).grid(row=row, column=1, sticky="ew", padx=6, pady=3)
        if browse:
            command = self._browse_save if browse == "save" else self._browse_open
            ttk.Button(parent, text="Browse", command=lambda: command(key)).grid(
                row=row, column=2, sticky="ew", padx=6, pady=3)

    def _check(self, parent, row: int, label: str, key: str, value: bool = False) -> None:
        var = tk.BooleanVar(value=value)
        self.fields[key] = var
        ttk.Checkbutton(parent, text=label, variable=var).grid(
            row=row, column=0, columnspan=3, sticky="w", padx=6, pady=3)

    def _json_field(self, parent, row: int, label: str, key: str, value: str = "{}",
                    *, height: int = 4) -> int:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="nw", padx=6, pady=3)
        pane = JsonTextPane(parent, height=height)
        pane.set(value)
        pane.grid(row=row, column=1, columnspan=2, sticky="nsew", padx=6, pady=3)
        parent.rowconfigure(row, weight=1)
        self.advanced_json[key] = pane
        return row

    def _browse_open(self, key: str) -> None:
        path = filedialog.askopenfilename(title=f"Select {key}")
        if path and isinstance(self.fields.get(key), tk.StringVar):
            self.fields[key].set(path)  # type: ignore[union-attr]

    def _browse_save(self, key: str) -> None:
        path = filedialog.asksaveasfilename(title=f"Select {key}")
        if path and isinstance(self.fields.get(key), tk.StringVar):
            self.fields[key].set(path)  # type: ignore[union-attr]

    def value(self, key: str) -> str:
        var = self.fields[key]
        return str(var.get())

    def bool_value(self, key: str) -> bool:
        var = self.fields[key]
        return bool(var.get())

    def int_value(self, key: str, *, minimum: int = 0) -> int:
        return parse_int_field(self.value(key), name=key, minimum=minimum)

    def _common_env_frame(self, parent) -> ttk.LabelFrame:
        frame = ttk.LabelFrame(parent, text="Advanced NDNSF/SVS environment")
        frame.columnconfigure(1, weight=1)
        row = 0
        self._check(frame, row, "Enable NDNSD", "env_enable_ndnsd", True); row += 1
        self._check(frame, row, "Disable NDNSD", "env_disable_ndnsd", False); row += 1
        self._field(frame, row, "Expected RPS", "env_expected_rps", ""); row += 1
        self._field(frame, row, "SVS fetch retries", "env_fetch_retries", ""); row += 1
        self._field(frame, row, "SVS inner retries", "env_inner_retries", ""); row += 1
        self._field(frame, row, "SVS lifetime ms", "env_lifetime_ms", ""); row += 1
        self._field(frame, row, "SVS fetch window", "env_fetch_window", ""); row += 1
        self._field(frame, row, "SVS suppression ms", "env_suppression_ms", "1"); row += 1
        self._field(frame, row, "SVS periodic sync ms", "env_periodic_ms", ""); row += 1
        self._check(frame, row, "Parallel Sync", "env_parallel_sync", True); row += 1
        self._check(frame, row, "Parallel Production", "env_parallel_production", True); row += 1
        self._check(frame, row, "Sync batching", "env_sync_batching", False); row += 1
        self._field(frame, row, "Sync batch ms", "env_sync_batch_ms", ""); row += 1
        return frame

    def env_from_fields(self) -> NdnsfSvsEnvConfig:
        return NdnsfSvsEnvConfig(
            enable_ndnsd=self.bool_value("env_enable_ndnsd"),
            disable_ndnsd=self.bool_value("env_disable_ndnsd"),
            expected_rps=self.value("env_expected_rps"),
            publication_fetch_retries=self.value("env_fetch_retries"),
            publication_fetch_inner_retries=self.value("env_inner_retries"),
            publication_fetch_lifetime_ms=self.value("env_lifetime_ms"),
            publication_fetch_window=self.value("env_fetch_window"),
            max_suppression_ms=self.value("env_suppression_ms"),
            periodic_sync_ms=self.value("env_periodic_ms"),
            parallel_sync=self.bool_value("env_parallel_sync"),
            parallel_production=self.bool_value("env_parallel_production"),
            sync_batching=self.bool_value("env_sync_batching"),
            sync_batch_ms=self.value("env_sync_batch_ms"),
        )

    def _build(self) -> None:
        raise NotImplementedError

    def log(self, message: str) -> None:
        self.log_pane.text.insert("end", message)
        self.log_pane.text.see("end")

    def clear_log(self) -> None:
        self.log_pane.set("")

    def run_role(self) -> None:
        config = self.config()
        self.controller.env_config = self.env_from_fields()
        self.controller.run(config)

    def stop_role(self) -> None:
        self.controller.stop()

    def restart_role(self) -> None:
        self.controller.restart(self.config())

    def config(self):
        raise NotImplementedError

    def apply_env(self, env: NdnsfSvsEnvConfig) -> None:
        self.fields["env_enable_ndnsd"].set(env.enable_ndnsd)  # type: ignore[union-attr]
        self.fields["env_disable_ndnsd"].set(env.disable_ndnsd)  # type: ignore[union-attr]
        self.fields["env_expected_rps"].set(env.expected_rps)  # type: ignore[union-attr]
        self.fields["env_fetch_retries"].set(env.publication_fetch_retries)  # type: ignore[union-attr]
        self.fields["env_inner_retries"].set(env.publication_fetch_inner_retries)  # type: ignore[union-attr]
        self.fields["env_lifetime_ms"].set(env.publication_fetch_lifetime_ms)  # type: ignore[union-attr]
        self.fields["env_fetch_window"].set(env.publication_fetch_window)  # type: ignore[union-attr]
        self.fields["env_suppression_ms"].set(env.max_suppression_ms)  # type: ignore[union-attr]
        self.fields["env_periodic_ms"].set(env.periodic_sync_ms)  # type: ignore[union-attr]
        self.fields["env_parallel_sync"].set(env.parallel_sync)  # type: ignore[union-attr]
        self.fields["env_parallel_production"].set(env.parallel_production)  # type: ignore[union-attr]
        self.fields["env_sync_batching"].set(env.sync_batching)  # type: ignore[union-attr]
        self.fields["env_sync_batch_ms"].set(env.sync_batch_ms)  # type: ignore[union-attr]


class ControllerDirectTab(DirectRoleTab):
    def _build(self) -> None:
        self.columnconfigure(0, weight=1)
        top = ttk.LabelFrame(self, text="Controller configuration")
        top.grid(row=0, column=0, sticky="ew", padx=6, pady=6)
        top.columnconfigure(1, weight=1)
        self._field(top, 0, "Controller prefix", "controller_prefix",
                    "/NDNSF-DI/Tracer/controller")
        self._field(top, 1, "Policy file", "policy_file", "examples/hello.policies",
                    browse="open")
        self._field(top, 2, "Trust schema", "trust_schema", "examples/trust-schema.conf",
                    browse="open")
        self._field(top, 3, "Bootstrap token file", "bootstrap_token_file",
                    "examples/hello.bootstrap-tokens", browse="open")
        self._field(top, 4, "Bootstrap identities (,)", "bootstrap_identities", "")
        self._check(top, 5, "Serve certificates", "serve_certificates", True)
        self._check(top, 6, "Save tokens in GUI profile", "persist_tokens", False)
        buttons = ttk.Frame(top)
        buttons.grid(row=7, column=0, columnspan=3, sticky="ew", padx=6, pady=6)
        ttk.Button(buttons, text="Run Controller", command=self.run_role).pack(side="left")
        ttk.Button(buttons, text="Stop", command=self.stop_role).pack(side="left", padx=4)
        ttk.Button(buttons, text="Validate Policy", command=self.validate_policy).pack(side="left")
        ttk.Button(buttons, text="Open/Create Token File",
                   command=self.open_token_file).pack(side="left", padx=4)
        ttk.Label(top, text="Status").grid(row=8, column=0, sticky="w", padx=6)
        ttk.Label(top, textvariable=self.status_var).grid(row=8, column=1, sticky="ew", padx=6)
        env = self._common_env_frame(self)
        env.grid(row=1, column=0, sticky="ew", padx=6, pady=6)
        token = ttk.LabelFrame(self, text="Token table helper")
        token.grid(row=2, column=0, sticky="ew", padx=6, pady=6)
        token.columnconfigure(1, weight=1)
        self._field(token, 0, "Identity name", "token_identity", "")
        self._field(token, 1, "Token", "token_value", "")
        ttk.Button(token, text="Add/Update Token", command=self.add_token).grid(
            row=2, column=0, columnspan=3, sticky="ew", padx=6, pady=4)
        self.log_pane = TextPane(self, height=12)
        self.log_pane.grid(row=3, column=0, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(3, weight=1)

    def config(self) -> ControllerTabConfig:
        return ControllerTabConfig(
            controller_prefix=self.value("controller_prefix"),
            policy_file=self.value("policy_file"),
            trust_schema=self.value("trust_schema"),
            bootstrap_token_file=self.value("bootstrap_token_file"),
            bootstrap_identities=self.value("bootstrap_identities"),
            serve_certificates=self.bool_value("serve_certificates"),
        )

    def validate_policy(self) -> None:
        path = self.value("policy_file")
        if not Path(path).exists():
            self.log(f"Policy file not found: {path}\n")
            return
        self.log(f"Policy file exists: {path}\n")

    def open_token_file(self) -> None:
        path = Path(self.value("bootstrap_token_file"))
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            path.write_text("", encoding="utf-8")
        self.log(f"Token file ready: {path}\n")

    def add_token(self) -> None:
        identity = self.value("token_identity").strip()
        token = self.value("token_value").strip()
        if not identity or not token:
            self.log("Token identity and token are required.\n")
            return
        path = Path(self.value("bootstrap_token_file"))
        path.parent.mkdir(parents=True, exist_ok=True)
        existing = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
        prefix = identity + " "
        lines = [line for line in existing if not line.startswith(prefix)]
        lines.append(f"{identity} {token}")
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        self.log(f"Stored token for {identity}: {redact_secret(token)}\n")

    def apply_config(self, config: ControllerTabConfig, env: NdnsfSvsEnvConfig) -> None:
        for key, value in asdict(config).items():
            if key in self.fields:
                self.fields[key].set(value)  # type: ignore[union-attr]
        self.apply_env(env)


class ProviderDirectTab(DirectRoleTab):
    def _build(self) -> None:
        self.columnconfigure(0, weight=1)
        basic = ttk.LabelFrame(self, text="Provider configuration")
        basic.grid(row=0, column=0, sticky="ew", padx=6, pady=6)
        basic.columnconfigure(1, weight=1)
        rows = [
            ("Provider ID", "provider_id", "A"),
            ("Provider prefix", "provider_prefix", "/NDNSF-DI/Tracer/provider"),
            ("Sync group", "group", "/NDNSF-DI/Tracer/group"),
            ("Controller", "controller", "/NDNSF-DI/Tracer/controller"),
            ("Trust schema", "trust_schema", "examples/trust-schema.conf"),
            ("Bootstrap token", "bootstrap_token", ""),
            ("Service name", "service_name", "/HELLO"),
            ("Roles", "roles", "all"),
            ("Handler threads", "handler_threads", "4"),
            ("ACK threads", "ack_threads", "2"),
            ("Handler mode", "handler_mode", "echo"),
            ("Static response", "static_response", "HELLO"),
            ("ACK message", "ack_message", "ready"),
        ]
        for row, (label, key, value) in enumerate(rows):
            browse = "open" if key == "trust_schema" else ""
            self._field(basic, row, label, key, value, browse=browse)
        self._check(basic, len(rows), "Serve certificates", "serve_certificates", True)
        self._check(basic, len(rows) + 1, "ACK status true", "ack_status", True)
        buttons = ttk.Frame(basic)
        buttons.grid(row=len(rows) + 2, column=0, columnspan=3, sticky="ew", padx=6, pady=6)
        ttk.Button(buttons, text="Run Provider", command=self.run_role).pack(side="left")
        ttk.Button(buttons, text="Stop", command=self.stop_role).pack(side="left", padx=4)
        ttk.Button(buttons, text="Publish Service Info", command=self.publish_info).pack(side="left")
        ttk.Button(buttons, text="Start Probing", command=self.start_provider_probing).pack(side="left", padx=4)
        ttk.Label(basic, text="Status").grid(row=len(rows) + 3, column=0, sticky="w", padx=6)
        ttk.Label(basic, textvariable=self.status_var).grid(row=len(rows) + 3, column=1, sticky="ew", padx=6)
        advanced = ttk.LabelFrame(self, text="Provider metadata and DI runtime")
        advanced.grid(row=1, column=0, sticky="nsew", padx=6, pady=6)
        advanced.columnconfigure(1, weight=1)
        self._field(advanced, 0, "Runtime profile", "runtime_profile", "examples/di-native-tracer.runtime.json", browse="open")
        self._field(advanced, 1, "Service manifest", "service_manifest", "", browse="open")
        self._field(advanced, 2, "Native plan", "native_plan", "", browse="open")
        self._field(advanced, 3, "Artifact cache dir", "artifact_cache_dir", "/tmp/ndnsf-di-artifacts")
        self._field(advanced, 4, "Deployment ID", "deployment_id", "")
        self._field(advanced, 5, "NDNSD lifetime seconds", "ndnsd_lifetime_seconds", "30")
        self._check(advanced, 6, "Provider probing", "provider_probing", False)
        self._field(advanced, 7, "Probe interval seconds", "provider_probe_interval_s", "10")
        self._json_field(advanced, 8, "ACK metadata JSON", "ack_metadata_json", "{}", height=3)
        self._json_field(advanced, 9, "NDNSD metadata JSON", "ndnsd_meta_json", "{}", height=3)
        self._json_field(advanced, 10, "Fragment inventory JSON", "fragment_inventory_json", "{}", height=3)
        self._json_field(advanced, 11, "Memory/compute profile JSON", "memory_compute_profile_json", "{}", height=3)
        env = self._common_env_frame(self)
        env.grid(row=2, column=0, sticky="ew", padx=6, pady=6)
        self.log_pane = TextPane(self, height=10)
        self.log_pane.grid(row=3, column=0, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(1, weight=1)
        self.rowconfigure(3, weight=1)

    def config(self) -> ProviderTabConfig:
        return ProviderTabConfig(
            provider_id=self.value("provider_id"),
            provider_prefix=self.value("provider_prefix"),
            group=self.value("group"),
            controller=self.value("controller"),
            trust_schema=self.value("trust_schema"),
            bootstrap_token=self.value("bootstrap_token"),
            service_name=self.value("service_name"),
            roles=self.value("roles"),
            handler_threads=self.int_value("handler_threads", minimum=1),
            ack_threads=self.int_value("ack_threads", minimum=1),
            serve_certificates=self.bool_value("serve_certificates"),
            handler_mode=self.value("handler_mode"),
            static_response=self.value("static_response"),
            ack_status=self.bool_value("ack_status"),
            ack_message=self.value("ack_message"),
            ack_metadata_json=self.advanced_json["ack_metadata_json"].get(),
            ndnsd_lifetime_seconds=self.int_value("ndnsd_lifetime_seconds", minimum=1),
            ndnsd_meta_json=self.advanced_json["ndnsd_meta_json"].get(),
            runtime_profile=self.value("runtime_profile"),
            service_manifest=self.value("service_manifest"),
            native_plan=self.value("native_plan"),
            fragment_inventory_json=self.advanced_json["fragment_inventory_json"].get(),
            artifact_cache_dir=self.value("artifact_cache_dir"),
            memory_compute_profile_json=self.advanced_json["memory_compute_profile_json"].get(),
            deployment_id=self.value("deployment_id"),
            provider_probing=self.bool_value("provider_probing"),
            provider_probe_interval_s=self.int_value("provider_probe_interval_s", minimum=1),
        )

    def publish_info(self) -> None:
        runtime = self.controller.runtime
        if runtime is None or not hasattr(runtime, "publish_service_info"):
            self.log("Provider runtime is not running or does not support publish_service_info.\n")
            return
        config = self.config()
        meta = parse_json_field(config.ndnsd_meta_json, default={})
        runtime.publish_service_info(config.service_name, config.ndnsd_lifetime_seconds, meta)
        self.log(f"Published service info for {config.service_name}\n")

    def start_provider_probing(self) -> None:
        runtime = self.controller.runtime
        if runtime is None:
            self.log("Provider runtime is not running.\n")
            return
        config = self.config()
        for method_name in ("start_provider_probing", "startProviderProbing"):
            method = getattr(runtime, method_name, None)
            if callable(method):
                method(config.service_name, config.provider_probe_interval_s)
                self.log(
                    f"Started provider probing for {config.service_name} "
                    f"every {config.provider_probe_interval_s}s\n"
                )
                return
        self.log("Provider probing is configured, but the current Python wrapper does not expose a probing start method.\n")

    def apply_config(self, config: ProviderTabConfig, env: NdnsfSvsEnvConfig) -> None:
        data = asdict(config)
        for key, value in data.items():
            if key in self.fields:
                self.fields[key].set(value)  # type: ignore[union-attr]
            if key in self.advanced_json:
                self.advanced_json[key].set(str(value))
        self.apply_env(env)


class UserDirectTab(DirectRoleTab):
    def _build(self) -> None:
        self.columnconfigure(0, weight=1)
        basic = ttk.LabelFrame(self, text="User configuration")
        basic.grid(row=0, column=0, sticky="ew", padx=6, pady=6)
        basic.columnconfigure(1, weight=1)
        rows = [
            ("User identity", "user", "/NDNSF-DI/Tracer/user"),
            ("Sync group", "group", "/NDNSF-DI/Tracer/group"),
            ("Controller", "controller", "/NDNSF-DI/Tracer/controller"),
            ("Trust schema", "trust_schema", "examples/trust-schema.conf"),
            ("Bootstrap token", "bootstrap_token", ""),
            ("Permission wait ms", "permission_wait_ms", "1500"),
            ("Handler threads", "handler_threads", "2"),
            ("ACK threads", "ack_threads", "2"),
        ]
        for row, (label, key, value) in enumerate(rows):
            browse = "open" if key == "trust_schema" else ""
            self._field(basic, row, label, key, value, browse=browse)
        self._check(basic, len(rows), "Adaptive admission", "adaptive_admission", False)
        self._check(basic, len(rows) + 1, "Serve certificates", "serve_certificates", True)
        buttons = ttk.Frame(basic)
        buttons.grid(row=len(rows) + 2, column=0, columnspan=3, sticky="ew", padx=6, pady=6)
        ttk.Button(buttons, text="Run User", command=self.run_role).pack(side="left")
        ttk.Button(buttons, text="Stop", command=self.stop_role).pack(side="left", padx=4)
        ttk.Button(buttons, text="Refresh Permissions", command=self.refresh_permissions).pack(side="left")
        ttk.Button(buttons, text="Discover Services", command=self.discover_services).pack(side="left", padx=4)
        ttk.Label(basic, text="Status").grid(row=len(rows) + 3, column=0, sticky="w", padx=6)
        ttk.Label(basic, textvariable=self.status_var).grid(row=len(rows) + 3, column=1, sticky="ew", padx=6)

        request = ttk.LabelFrame(self, text="Request / Response")
        request.grid(row=1, column=0, sticky="nsew", padx=6, pady=6)
        request.columnconfigure(1, weight=1)
        request_rows = [
            ("Service name", "request_service_name", "/HELLO"),
            ("Request strategy", "request_strategy", "first-responding"),
            ("ACK timeout ms", "request_ack_timeout_ms", "1000"),
            ("Total timeout ms", "request_timeout_ms", "10000"),
            ("Payload encoding", "payload_encoding", "text"),
            ("Request mode", "request_mode", "normal"),
            ("Deployment ID", "deployment_id", ""),
        ]
        for row, (label, key, value) in enumerate(request_rows):
            self._field(request, row, label, key, value)
        ttk.Label(request, text="Payload").grid(row=len(request_rows), column=0, sticky="nw", padx=6)
        self.payload_pane = TextPane(request, height=5)
        self.payload_pane.set("HELLO")
        self.payload_pane.grid(row=len(request_rows), column=1, columnspan=2, sticky="nsew", padx=6, pady=3)
        row = len(request_rows) + 1
        self._json_field(request, row, "Collaboration roles JSON", "collaboration_roles_json", "[]", height=3); row += 1
        self._json_field(request, row, "Key scopes JSON", "key_scopes_json", "{}", height=3); row += 1
        self._json_field(request, row, "Dependencies JSON", "dependencies_json", "[]", height=3); row += 1
        self._json_field(request, row, "Artifact Data names JSON", "artifact_data_names_json", "{}", height=3); row += 1
        self._json_field(request, row, "Scope-key Data names JSON", "scope_key_data_names_json", "{}", height=3); row += 1
        self._json_field(request, row, "Role scopes JSON", "role_scopes_json", "{}", height=3); row += 1
        ttk.Button(request, text="Send Request", command=self.send_request).grid(
            row=row, column=0, sticky="ew", padx=6, pady=5)
        ttk.Button(request, text="Send Async Request", command=self.send_async_request).grid(
            row=row, column=1, sticky="ew", padx=6, pady=5)
        ttk.Button(request, text="Clear Response", command=lambda: self.response_pane.set("")).grid(
            row=row, column=2, sticky="ew", padx=6, pady=5)
        row += 1
        ttk.Button(request, text="Send Collaboration Request",
                   command=self.send_collaboration_request).grid(
            row=row, column=0, columnspan=3, sticky="ew", padx=6, pady=5)
        row += 1
        self.response_pane = TextPane(request, height=8)
        self.response_pane.grid(row=row, column=0, columnspan=3, sticky="nsew", padx=6, pady=6)
        request.rowconfigure(row, weight=1)
        env = self._common_env_frame(self)
        env.grid(row=2, column=0, sticky="ew", padx=6, pady=6)
        self.log_pane = TextPane(self, height=8)
        self.log_pane.grid(row=3, column=0, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(1, weight=2)
        self.rowconfigure(3, weight=1)

    def request_config(self) -> UserRequestConfig:
        return UserRequestConfig(
            service_name=self.value("request_service_name"),
            request_strategy=self.value("request_strategy"),
            ack_timeout_ms=self.int_value("request_ack_timeout_ms", minimum=0),
            timeout_ms=self.int_value("request_timeout_ms", minimum=1),
            payload_encoding=self.value("payload_encoding"),
            payload=self.payload_pane.get(),
            request_mode=self.value("request_mode"),
            collaboration_roles_json=self.advanced_json["collaboration_roles_json"].get(),
            key_scopes_json=self.advanced_json["key_scopes_json"].get(),
            dependencies_json=self.advanced_json["dependencies_json"].get(),
            artifact_data_names_json=self.advanced_json["artifact_data_names_json"].get(),
            scope_key_data_names_json=self.advanced_json["scope_key_data_names_json"].get(),
            role_scopes_json=self.advanced_json["role_scopes_json"].get(),
            deployment_id=self.value("deployment_id"),
        )

    def config(self) -> UserTabConfig:
        return UserTabConfig(
            user=self.value("user"),
            group=self.value("group"),
            controller=self.value("controller"),
            trust_schema=self.value("trust_schema"),
            bootstrap_token=self.value("bootstrap_token"),
            permission_wait_ms=self.int_value("permission_wait_ms", minimum=0),
            handler_threads=self.int_value("handler_threads", minimum=1),
            ack_threads=self.int_value("ack_threads", minimum=1),
            adaptive_admission=self.bool_value("adaptive_admission"),
            serve_certificates=self.bool_value("serve_certificates"),
            request=self.request_config(),
        )

    def send_request(self) -> None:
        try:
            config = self.request_config()
        except Exception as exc:
            self.queue.put(("response", f"Request failed: {exc}\n"))
            return
        threading.Thread(target=self._send_request_thread, args=(config,), daemon=True).start()

    def send_async_request(self) -> None:
        runtime = self.controller.runtime
        if runtime is None or not hasattr(runtime, "request_service_async"):
            self.log("Async request unavailable; user runtime is not running or wrapper lacks request_service_async.\n")
            return
        try:
            req = self.request_config()
            started = time.time()

            def on_response(response) -> None:
                elapsed_ms = (time.time() - started) * 1000.0
                payload = getattr(response, "payload", b"")
                payload_text = payload.decode("utf-8", errors="replace") if isinstance(payload, bytes) else str(payload)
                self.queue.put(
                    ("response",
                     "Async response\n"
                     f"status: {getattr(response, 'status', '')}\n"
                     f"message: {getattr(response, 'message', getattr(response, 'error', ''))}\n"
                     f"elapsed_ms: {elapsed_ms:.3f}\n"
                     f"payload:\n{payload_text}\n")
                )

            def on_timeout(reason: str) -> None:
                self.queue.put(("response", f"Async request timeout: {reason}\n"))

            runtime.request_service_async(
                req.service_name,
                payload_from_request(req),
                on_response=on_response,
                on_timeout=on_timeout,
                ack_timeout_ms=req.ack_timeout_ms,
                timeout_ms=req.timeout_ms,
                strategy=req.request_strategy,
            )
            self.log(f"Async request submitted for {req.service_name}\n")
        except Exception as exc:
            self.log(f"Async request failed: {exc}\n")

    def _send_request_thread(self, config: UserRequestConfig) -> None:
        started = time.time()
        try:
            response = self.controller.request_user(config)
            elapsed_ms = (time.time() - started) * 1000.0
            payload = getattr(response, "payload", b"")
            if isinstance(payload, bytes):
                payload_text = payload.decode("utf-8", errors="replace")
            else:
                payload_text = str(payload)
            status = getattr(response, "status", getattr(response, "success", ""))
            message = getattr(response, "message", getattr(response, "error", ""))
            self.queue.put((
                "response",
                "Response\n"
                f"status: {status}\n"
                f"message: {message}\n"
                f"elapsed_ms: {elapsed_ms:.3f}\n"
                f"payload:\n{payload_text}\n",
            ))
        except Exception as exc:
            self.queue.put(("response", f"Request failed: {exc}\n"))

    def send_collaboration_request(self) -> None:
        try:
            req = self.request_config()
            collaboration_args = {
                "roles": parse_json_field(req.collaboration_roles_json, default=[]),
                "key_scopes": parse_json_field(req.key_scopes_json, default={}),
                "dependencies": parse_json_field(req.dependencies_json, default=[]),
                "artifact_data_names": parse_json_field(req.artifact_data_names_json, default={}),
                "scope_key_data_names": parse_json_field(req.scope_key_data_names_json, default={}),
                "role_scopes": parse_json_field(req.role_scopes_json, default={}),
            }
        except Exception as exc:
            self.queue.put(("response", f"Collaboration request failed: {exc}\n"))
            return
        threading.Thread(
            target=self._send_collaboration_thread,
            args=(req, collaboration_args),
            daemon=True,
        ).start()

    def _send_collaboration_thread(self,
                                   req: UserRequestConfig,
                                   collaboration_args: dict[str, Any]) -> None:
        started = time.time()
        runtime = self.controller.runtime
        if runtime is None or not hasattr(runtime, "request_collaboration"):
            self.queue.put(("response",
                            "Collaboration request failed: user runtime is not running or lacks request_collaboration.\n"))
            return
        try:
            from .deployment import request_collaboration_with_deployment
            response = request_collaboration_with_deployment(
                runtime,
                req.service_name,
                payload_from_request(req),
                **collaboration_args,
                ack_timeout_ms=req.ack_timeout_ms,
                timeout_ms=req.timeout_ms,
                deployment_id=req.deployment_id,
            )
            elapsed_ms = (time.time() - started) * 1000.0
            payload = getattr(response, "payload", b"")
            payload_text = payload.decode("utf-8", errors="replace") if isinstance(payload, bytes) else str(payload)
            self.queue.put((
                "response",
                "Collaboration response\n"
                f"status: {getattr(response, 'status', '')}\n"
                f"message: {getattr(response, 'message', getattr(response, 'error', ''))}\n"
                f"elapsed_ms: {elapsed_ms:.3f}\n"
                f"payload:\n{payload_text}\n",
            ))
        except Exception as exc:
            self.queue.put(("response", f"Collaboration request failed: {exc}\n"))

    def refresh_permissions(self) -> None:
        runtime = self.controller.runtime
        if runtime is None or not hasattr(runtime, "get_allowed_services"):
            self.log("User runtime is not running.\n")
            return
        self.response_pane.set(json.dumps([asdict(item) if hasattr(item, "__dataclass_fields__") else str(item)
                                           for item in runtime.get_allowed_services()],
                                          indent=2))

    def discover_services(self) -> None:
        runtime = self.controller.runtime
        if runtime is None or not hasattr(runtime, "get_ndnsd_services"):
            self.log("User runtime is not running.\n")
            return
        self.response_pane.set(json.dumps(runtime.get_ndnsd_services(), indent=2, default=str))

    def apply_config(self, config: UserTabConfig, env: NdnsfSvsEnvConfig) -> None:
        data = asdict(config)
        request = data.pop("request", {})
        for key, value in data.items():
            if key in self.fields:
                self.fields[key].set(value)  # type: ignore[union-attr]
        request_field_map = {
            "service_name": "request_service_name",
            "ack_timeout_ms": "request_ack_timeout_ms",
            "timeout_ms": "request_timeout_ms",
        }
        for key, value in request.items():
            field_key = request_field_map.get(key, key)
            if field_key == "payload":
                self.payload_pane.set(str(value))
            elif field_key in self.fields:
                self.fields[field_key].set(value)  # type: ignore[union-attr]
            elif field_key in self.advanced_json:
                self.advanced_json[field_key].set(str(value))
        self.apply_env(env)


class QwenMiniNdnExperimentTab(ttk.Frame):
    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.fields: dict[str, tk.StringVar | tk.BooleanVar] = {}
        self.queue: queue.Queue[str | tuple[str, Any]] = queue.Queue()
        self.process: subprocess.Popen[str] | None = None
        self.thread: threading.Thread | None = None
        self.stop_requested = False
        self._drain_after_id: str | None = None
        self.status_var = tk.StringVar(value="stopped")
        self._build()
        self._drain_after_id = self.after(200, self._drain_queue)

    def _field(self, parent, row: int, label: str, key: str, value: str = "",
               *, browse: str = "") -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=3)
        var = tk.StringVar(value=value)
        self.fields[key] = var
        ttk.Entry(parent, textvariable=var).grid(row=row, column=1, sticky="ew", padx=6, pady=3)
        if browse:
            if browse == "dir":
                command = self._browse_dir
            elif browse == "save":
                command = self._browse_save
            else:
                command = self._browse_open
            ttk.Button(parent, text="Browse", command=lambda: command(key)).grid(
                row=row, column=2, sticky="ew", padx=6, pady=3)

    def _check(self, parent, row: int, label: str, key: str, value: bool = False) -> None:
        var = tk.BooleanVar(value=value)
        self.fields[key] = var
        ttk.Checkbutton(parent, text=label, variable=var).grid(
            row=row, column=0, columnspan=3, sticky="w", padx=6, pady=3)

    def _build(self) -> None:
        self.columnconfigure(0, weight=1)
        self.rowconfigure(3, weight=1)
        config = ttk.LabelFrame(self, text="Qwen NativeTracer MiniNDN experiment")
        config.grid(row=0, column=0, sticky="ew", padx=6, pady=6)
        config.columnconfigure(1, weight=1)
        rows = [
            ("Runtime profile", "runtime_profile", "examples/di-native-tracer.runtime.json", "open"),
            ("Output directory", "out_dir", "/tmp/ndnsf-di-gui-qwen-minindn", "dir"),
            ("Requests", "requests", "1", ""),
            ("Concurrency", "concurrency", "1", ""),
            ("Provider check timeout s", "provider_check_timeout", "60", ""),
            ("Target RPS", "target_rps", "", ""),
            ("Target RPS sweep list", "target_rps_list", "", ""),
            ("Sweep repeats", "sweep_repeats", "1", ""),
            ("Open-loop duration s", "open_loop_duration_s", "", ""),
            ("Open-loop driver mode", "open_loop_driver_mode", "process-pool", ""),
            ("Dependency envelope mode", "dependency_envelope_mode", "raw", ""),
            ("Output JSON", "output_json", "/tmp/ndnsf-di-gui-qwen-minindn/gui-summary.json", "save"),
            ("Extra harness args", "extra_args", "", ""),
        ]
        for row, (label, key, value, browse) in enumerate(rows):
            self._field(config, row, label, key, value, browse=browse)
        row = len(rows)
        self._check(config, row, "Dry run only", "dry_run", False); row += 1
        self._check(config, row, "Wrap with sudo -n env", "use_sudo", True); row += 1
        buttons = ttk.Frame(config)
        buttons.grid(row=row, column=0, columnspan=3, sticky="ew", padx=6, pady=6)
        ttk.Button(buttons, text="Preview Command", command=self.preview_command).pack(side="left")
        self.run_button = ttk.Button(buttons, text="Run Qwen MiniNDN", command=self.run_experiment)
        self.run_button.pack(side="left", padx=4)
        self.sweep_button = ttk.Button(buttons, text="Run Sweep", command=self.run_sweep)
        self.sweep_button.pack(side="left", padx=4)
        ttk.Button(buttons, text="Refresh Summary", command=self.refresh_summary).pack(side="left", padx=4)
        self.stop_button = ttk.Button(buttons, text="Stop", command=self.stop_experiment, state="disabled")
        self.stop_button.pack(side="left", padx=4)
        ttk.Label(config, text="Status").grid(row=row + 1, column=0, sticky="w", padx=6)
        ttk.Label(config, textvariable=self.status_var).grid(row=row + 1, column=1, sticky="ew", padx=6)

        self.command_preview = TextPane(self, height=5)
        self.command_preview.grid(row=1, column=0, sticky="ew", padx=6, pady=6)
        self.core_summary_pane = TextPane(self, height=10)
        self.core_summary_pane.grid(row=2, column=0, sticky="ew", padx=6, pady=6)
        self.core_summary_pane.set("Core envelope summary will appear here after a run.")
        self.log_pane = TextPane(self, height=22)
        self.log_pane.grid(row=3, column=0, sticky="nsew", padx=6, pady=6)
        self.preview_command()

    def _browse_open(self, key: str) -> None:
        path = filedialog.askopenfilename(title=f"Select {key}")
        if path and isinstance(self.fields.get(key), tk.StringVar):
            self.fields[key].set(path)  # type: ignore[union-attr]

    def _browse_dir(self, key: str) -> None:
        path = filedialog.askdirectory(title=f"Select {key}")
        if path and isinstance(self.fields.get(key), tk.StringVar):
            self.fields[key].set(path)  # type: ignore[union-attr]

    def _browse_save(self, key: str) -> None:
        path = filedialog.asksaveasfilename(title=f"Select {key}")
        if path and isinstance(self.fields.get(key), tk.StringVar):
            self.fields[key].set(path)  # type: ignore[union-attr]

    def value(self, key: str) -> str:
        return str(self.fields[key].get())

    def bool_value(self, key: str) -> bool:
        return bool(self.fields[key].get())

    def _float_value(self, key: str, default: float) -> float:
        value = self.value(key).strip()
        return default if not value else float(value)

    def _int_value(self, key: str, default: int) -> int:
        value = self.value(key).strip()
        return default if not value else int(value)

    def _args_namespace(self,
                        *,
                        out_dir: str | None = None,
                        target_rps: float | None = None) -> SimpleNamespace:
        return SimpleNamespace(
            profile="",
            controller_config="",
            provider_config="",
            user_config="",
            experiment_runtime_profile=self.value("runtime_profile"),
            experiment_out=out_dir if out_dir is not None else self.value("out_dir"),
            experiment_requests=self._int_value("requests", 0),
            experiment_concurrency=self._int_value("concurrency", 0),
            experiment_provider_check_timeout=self._int_value("provider_check_timeout", 0),
            experiment_target_rps=(
                target_rps if target_rps is not None
                else self._float_value("target_rps", -1.0)
            ),
            experiment_open_loop_duration_s=self._float_value("open_loop_duration_s", -1.0),
            experiment_open_loop_driver_mode=(
                self.value("open_loop_driver_mode").strip() or "process-pool"
            ),
            experiment_dependency_envelope_mode=self.value("dependency_envelope_mode") or "raw",
            experiment_dry_run=self.bool_value("dry_run"),
            experiment_extra_arg=split_extra_args(self.value("extra_args")),
            output_json=self.value("output_json"),
        )

    def _wrap_command(self, command: list[str]) -> list[str]:
        if not self.bool_value("use_sudo"):
            return command
        return [
            "sudo",
            "-n",
            "env",
            "PYTHONPATH=NDNSF-DistributedInference:pythonWrapper",
            "PYTHONPYCACHEPREFIX=/tmp/ndnsf_pycache",
            *command,
        ]

    def experiment_command(self,
                           *,
                           out_dir: str | None = None,
                           target_rps: float | None = None) -> tuple[list[str], Path]:
        args = self._args_namespace(out_dir=out_dir, target_rps=target_rps)
        command, out_dir = build_qwen_minindn_command(self.app.profile(), args)
        return self._wrap_command(command), out_dir

    def sweep_commands(self) -> list[tuple[str, list[str], Path]]:
        values = [
            item.strip()
            for item in self.value("target_rps_list").split(",")
            if item.strip()
        ]
        if not values:
            values = [self.value("target_rps").strip() or "0"]
        repeats = max(1, self._int_value("sweep_repeats", 1))
        base_out = Path(self.value("out_dir"))
        commands: list[tuple[str, list[str], Path]] = []
        for rps_text in values:
            rps = float(rps_text)
            token = rps_text.replace(".", "_").replace("-", "m")
            for repeat in range(1, repeats + 1):
                out_dir = base_out / f"rps-{token}-run-{repeat}"
                command, resolved_out = self.experiment_command(
                    out_dir=str(out_dir),
                    target_rps=rps,
                )
                commands.append((f"rps={rps_text} run={repeat}", command, resolved_out))
        return commands

    def preview_command(self) -> None:
        try:
            command, _ = self.experiment_command()
            self.command_preview.set(shlex.join(command))
            self.status_var.set("command ready")
        except Exception as exc:
            self.command_preview.set(f"Command error: {exc}")
            self.status_var.set("command error")

    def run_experiment(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.queue.put("Qwen MiniNDN experiment is already running.\n")
            return
        try:
            command, out_dir = self.experiment_command()
        except Exception as exc:
            self.queue.put(f"Qwen MiniNDN command failed: {exc}\n")
            self.status_var.set("command error")
            return
        out_dir.mkdir(parents=True, exist_ok=True)
        self.command_preview.set(shlex.join(command))
        self.log_pane.set("")
        self.stop_requested = False
        self.status_var.set("starting")
        self.app.set_status("Qwen MiniNDN experiment starting")
        self.run_button.configure(state="disabled")
        self.sweep_button.configure(state="disabled")
        self.stop_button.configure(state="normal")
        self.thread = threading.Thread(
            target=self._run_commands_thread,
            args=([("single", command, out_dir)],),
            daemon=True,
        )
        self.thread.start()

    def run_sweep(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.queue.put("Qwen MiniNDN experiment is already running.\n")
            return
        try:
            commands = self.sweep_commands()
        except Exception as exc:
            self.queue.put(f"Qwen MiniNDN sweep command failed: {exc}\n")
            self.status_var.set("command error")
            return
        for _, _, out_dir in commands:
            out_dir.mkdir(parents=True, exist_ok=True)
        self.command_preview.set("\n".join(shlex.join(command) for _, command, _ in commands))
        self.log_pane.set("")
        self.stop_requested = False
        self.status_var.set(f"sweep starting {len(commands)} runs")
        self.app.set_status("Qwen MiniNDN sweep starting")
        self.run_button.configure(state="disabled")
        self.sweep_button.configure(state="disabled")
        self.stop_button.configure(state="normal")
        self.thread = threading.Thread(
            target=self._run_commands_thread,
            args=(commands,),
            daemon=True,
        )
        self.thread.start()

    def _run_commands_thread(self, commands: list[tuple[str, list[str], Path]]) -> None:
        started_at = time.time()
        results: list[dict[str, Any]] = []
        try:
            for label, command, out_dir in commands:
                if self.stop_requested:
                    results.append({"label": label, "out": str(out_dir), "returncode": -15, "stopped": True})
                    break
                self.queue.put(f"\n=== Qwen MiniNDN {label} ===\n")
                self.process = subprocess.Popen(
                    command,
                    cwd=str(repo_root()),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    bufsize=1,
                )
                assert self.process.stdout is not None
                try:
                    for line in self.process.stdout:
                        self.queue.put(line)
                finally:
                    self.process.stdout.close()
                returncode = self.process.wait()
                result = {"label": label, "out": str(out_dir), "returncode": returncode}
                results.append(result)
                if returncode != 0:
                    break
            elapsed_ms = round((time.time() - started_at) * 1000.0, 3)
            final_rc = next((item["returncode"] for item in results if item["returncode"] != 0), 0)
            self.queue.put(("done", {
                "returncode": final_rc,
                "elapsed_ms": elapsed_ms,
                "results": results,
            }))
        except Exception as exc:
            self.queue.put(("done", {"returncode": -1, "elapsed_ms": 0.0, "error": str(exc)}))

    def stop_experiment(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.stop_requested = True
            self.process.terminate()
            self.queue.put("Qwen MiniNDN experiment termination requested.\n")
            self.status_var.set("stopping")

    def refresh_summary(self) -> None:
        summary_path = Path(self.value("out_dir")) / "summary.json"
        if not summary_path.exists():
            self.core_summary_pane.set(f"No summary found at {summary_path}")
            self.app.set_status("Qwen MiniNDN summary not found")
            return
        try:
            data = json.loads(summary_path.read_text(encoding="utf-8"))
        except Exception as exc:
            self.core_summary_pane.set(f"Summary read failed: {exc}")
            self.app.set_status("Qwen MiniNDN summary read failed")
            return
        self._display_core_summary(data)
        self.app.set_status(f"Qwen MiniNDN summary loaded: {summary_path}")

    def _display_core_summary(self, data: dict[str, Any]) -> None:
        self.core_summary_pane.set(format_core_envelope_summary(
            data.get("coreEnvelopeSummary", {}),
            data.get("providerAckRuntimeHints", {}),
        ))

    def _drain_queue(self) -> None:
        while True:
            try:
                item = self.queue.get_nowait()
            except queue.Empty:
                break
            if isinstance(item, tuple) and item[0] == "done":
                result = item[1]
                returncode = result.get("returncode", -1)
                if returncode == 0:
                    self.status_var.set(f"completed rc=0 elapsed_ms={result.get('elapsed_ms')}")
                    self.app.set_status("Qwen MiniNDN experiment completed")
                    self._append_summary(result.get("results", []))
                else:
                    self.status_var.set(f"failed rc={returncode}")
                    self.app.set_status("Qwen MiniNDN experiment failed")
                    error = result.get("error")
                    if error:
                        self.log_pane.text.insert("end", f"\nERROR: {error}\n")
                self.run_button.configure(state="normal")
                self.sweep_button.configure(state="normal")
                self.stop_button.configure(state="disabled")
                self.process = None
            else:
                self.log_pane.text.insert("end", str(item))
                self.log_pane.text.see("end")
        self._drain_after_id = self.after(200, self._drain_queue)

    def _append_summary(self, results: list[dict[str, Any]]) -> None:
        output_json_value = self.value("output_json").strip()
        compact_runs: list[dict[str, Any]] = []
        csv_rows: list[dict[str, Any]] = []
        if not results:
            results = [{"label": "single", "out": self.value("out_dir"), "returncode": 0}]
        for result in results:
            summary_path = Path(str(result.get("out", self.value("out_dir")))) / "summary.json"
            if not summary_path.exists():
                continue
            try:
                data = json.loads(summary_path.read_text(encoding="utf-8"))
                compact_runs.append({
                    "label": result.get("label", ""),
                    "returncode": result.get("returncode"),
                    "ok": data.get("status") == "SUCCESS",
                    "status": data.get("status"),
                    "runnerMode": data.get("runnerMode"),
                    "userExecution": data.get("userExecution", {}),
                    "dependencyExecution": data.get("dependencyExecution", {}),
                    "dependencyEnvelopeMode": data.get(
                        "dependencyEnvelopeMode",
                        data.get("dependencyPayloadMode", ""),
                    ),
                    "dependencyPayloadMode": data.get(
                        "dependencyPayloadMode",
                        data.get("dependencyEnvelopeMode", ""),
                    ),
                    "coreEnvelopeSummary": data.get("coreEnvelopeSummary", {}),
                    "providerAckRuntimeHints": data.get("providerAckRuntimeHints", {}),
                    "streamChunkDependencyCounters": data.get("streamChunkDependencyCounters", {}),
                    "summary_json": str(summary_path),
                    "out": str(summary_path.parent),
                })
                csv_rows.append(self._summary_csv_row(result, data, summary_path))
                self._display_core_summary(data)
            except Exception as exc:
                self.log_pane.text.insert("end", f"\nSummary read failed: {exc}\n")
        if compact_runs:
            payload: dict[str, Any]
            if len(compact_runs) == 1:
                payload = compact_runs[0]
            else:
                payload = {"ok": all(item.get("ok") for item in compact_runs), "runs": compact_runs}
            if output_json_value:
                output_json = Path(output_json_value)
                output_json.parent.mkdir(parents=True, exist_ok=True)
                output_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
                if csv_rows:
                    csv_path = output_json.with_suffix(".csv")
                    report_path = output_json.with_suffix(".md")
                    plot_path = output_json.with_suffix(".svg")
                    self._write_summary_csv(csv_path, csv_rows)
                    self._write_summary_svg_plot(plot_path, csv_rows)
                    self._write_summary_markdown_report(report_path, csv_rows, plot_path)
                    payload["csv"] = str(csv_path)
                    payload["report"] = str(report_path)
                    payload["plot"] = str(plot_path)
                    output_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
            self.log_pane.text.insert(
                "end",
                "\nQwen MiniNDN summary\n" + json.dumps(payload, indent=2) + "\n",
            )

    def _summary_csv_row(self,
                         result: dict[str, Any],
                         data: dict[str, Any],
                         summary_path: Path) -> dict[str, Any]:
        user_execution = data.get("userExecution", {}) or {}
        dependency_execution = data.get("dependencyExecution", {}) or {}
        provider_utilization = data.get("providerUtilization", {}) or {}
        provider_count = len(provider_utilization) if isinstance(provider_utilization, dict) else 0
        provider_mean_utilization = ""
        provider_busy_ms = ""
        if isinstance(provider_utilization, dict) and provider_utilization:
            utilizations = [
                float(item.get("estimatedUtilization", 0.0) or 0.0)
                for item in provider_utilization.values()
                if isinstance(item, dict)
            ]
            busy_values = [
                float(item.get("busyHandlerMs", 0.0) or 0.0)
                for item in provider_utilization.values()
                if isinstance(item, dict)
            ]
            if utilizations:
                provider_mean_utilization = round(sum(utilizations) / len(utilizations), 6)
            if busy_values:
                provider_busy_ms = round(sum(busy_values), 3)
        request_count = int(user_execution.get("requestCount", 0) or 0)
        success_count = int(user_execution.get("successCount", 0) or 0)
        failure_count = int(user_execution.get("failureCount", 0) or 0)
        success_rate = round(success_count / request_count, 6) if request_count else ""
        return {
            "label": result.get("label", ""),
            "out": str(summary_path.parent),
            "summary_json": str(summary_path),
            "returncode": result.get("returncode", ""),
            "status": data.get("status", ""),
            "runnerMode": data.get("runnerMode", ""),
            "miniNDNRun": data.get("miniNDNRun", ""),
            "targetRps": user_execution.get("targetRps", data.get("targetRps", "")),
            "requestCount": request_count,
            "successCount": success_count,
            "failureCount": failure_count,
            "successRate": success_rate,
            "p50Ms": user_execution.get("p50Ms", ""),
            "p95Ms": user_execution.get("p95Ms", ""),
            "meanMs": user_execution.get("meanMs", ""),
            "makespanMs": user_execution.get("makespanMs", ""),
            "throughputRps": user_execution.get("throughputRps", ""),
            "localBackpressureCount": user_execution.get("localBackpressureCount", ""),
            "localBackpressureWaitCount": user_execution.get("localBackpressureWaitCount", ""),
            "maxScheduleSlipMs": user_execution.get("maxScheduleSlipMs", ""),
            "openLoopDriverMode": user_execution.get("openLoopDriverMode", ""),
            "dependencyStatus": dependency_execution.get("status", ""),
            "dependencyEnvelopeMode": data.get(
                "dependencyEnvelopeMode",
                data.get("dependencyPayloadMode", ""),
            ),
            "dependencyPayloadMode": data.get(
                "dependencyPayloadMode",
                data.get("dependencyEnvelopeMode", ""),
            ),
            "dependencyEventCount": (
                data.get("streamChunkDependencyCounters", {}) or {}
            ).get("eventCount", ""),
            "dependencyDecodeErrorCount": (
                data.get("streamChunkDependencyCounters", {}) or {}
            ).get("decodeErrorCount", ""),
            "dependencyRoles": ",".join(dependency_execution.get("roles", []) or []),
            "providerCount": provider_count,
            "providerMeanUtilization": provider_mean_utilization,
            "providerBusyHandlerMs": provider_busy_ms,
        }

    def _write_summary_csv(self, path: Path, rows: list[dict[str, Any]]) -> None:
        fieldnames = [
            "label",
            "out",
            "summary_json",
            "returncode",
            "status",
            "runnerMode",
            "miniNDNRun",
            "targetRps",
            "requestCount",
            "successCount",
            "failureCount",
            "successRate",
            "p50Ms",
            "p95Ms",
            "meanMs",
            "makespanMs",
            "throughputRps",
            "localBackpressureCount",
            "localBackpressureWaitCount",
            "maxScheduleSlipMs",
            "openLoopDriverMode",
            "dependencyStatus",
            "dependencyEnvelopeMode",
            "dependencyPayloadMode",
            "dependencyEventCount",
            "dependencyDecodeErrorCount",
            "dependencyRoles",
            "providerCount",
            "providerMeanUtilization",
            "providerBusyHandlerMs",
        ]
        with path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fieldnames)
            writer.writeheader()
            for row in rows:
                writer.writerow(row)

    def _write_summary_markdown_report(self,
                                       path: Path,
                                       rows: list[dict[str, Any]],
                                       plot_path: Path | None = None) -> None:
        def number(row: dict[str, Any], key: str) -> float | None:
            value = row.get(key, "")
            if value == "":
                return None
            try:
                return float(value)
            except (TypeError, ValueError):
                return None

        def fmt(value: float | None) -> str:
            if value is None:
                return "n/a"
            text = f"{value:.3f}".rstrip("0").rstrip(".")
            return text if text else "0"

        def cell(value: Any) -> str:
            return str(value if value != "" else "n/a").replace("|", "\\|")

        def integer(value: Any) -> int:
            try:
                return int(value or 0)
            except (TypeError, ValueError):
                return 0

        failed_rows = [
            row for row in rows
            if row.get("status") != "SUCCESS"
            or integer(row.get("failureCount")) > 0
            or integer(row.get("returncode")) != 0
        ]
        successful_rows = [row for row in rows if row not in failed_rows]
        p50_rows = [
            (number(row, "p50Ms"), row)
            for row in successful_rows
            if number(row, "p50Ms") is not None
        ]
        throughput_rows = [
            (number(row, "throughputRps"), row)
            for row in successful_rows
            if number(row, "throughputRps") is not None
        ]
        utilization_values = [
            value for value in (number(row, "providerMeanUtilization") for row in rows)
            if value is not None
        ]
        busy_values = [
            value for value in (number(row, "providerBusyHandlerMs") for row in rows)
            if value is not None
        ]
        best_p50 = min(p50_rows, key=lambda item: item[0]) if p50_rows else (None, {})
        best_throughput = max(throughput_rows, key=lambda item: item[0]) if throughput_rows else (None, {})
        mean_utilization = (
            sum(utilization_values) / len(utilization_values)
            if utilization_values else None
        )
        total_busy_ms = sum(busy_values) if busy_values else None

        lines = [
            "# Qwen MiniNDN Sweep Report",
            "",
            f"- Total runs: {len(rows)}",
            f"- Successful runs: {len(successful_rows)}",
            f"- Failed runs: {len(failed_rows)}",
            f"- Best p50: {fmt(best_p50[0])} ms ({cell(best_p50[1].get('label', ''))})",
            (
                f"- Best throughput: {fmt(best_throughput[0])} RPS "
                f"({cell(best_throughput[1].get('label', ''))})"
            ),
            f"- Mean provider utilization across runs: {fmt(mean_utilization)}",
            f"- Total provider busy handler time: {fmt(total_busy_ms)} ms",
            "",
        ]
        if plot_path is not None:
            lines.extend(["## Plot", "", f"![Qwen MiniNDN sweep plot]({plot_path.name})", ""])
        lines.extend([
            "## Runs",
            "",
            (
                "| label | targetRps | status | successRate | p50Ms | p95Ms | "
                "throughputRps | providerMeanUtilization | providerBusyHandlerMs |"
            ),
            "|---|---:|---|---:|---:|---:|---:|---:|---:|",
        ])
        for row in rows:
            lines.append(
                "| "
                + " | ".join([
                    cell(row.get("label", "")),
                    cell(row.get("targetRps", "")),
                    cell(row.get("status", "")),
                    cell(row.get("successRate", "")),
                    cell(row.get("p50Ms", "")),
                    cell(row.get("p95Ms", "")),
                    cell(row.get("throughputRps", "")),
                    cell(row.get("providerMeanUtilization", "")),
                    cell(row.get("providerBusyHandlerMs", "")),
                ])
                + " |"
            )
        lines.extend(["", "## Failures", ""])
        if failed_rows:
            for row in failed_rows:
                lines.append(
                    f"- {cell(row.get('label', ''))}: status={cell(row.get('status', ''))}, "
                    f"returncode={cell(row.get('returncode', ''))}, "
                    f"failureCount={cell(row.get('failureCount', ''))}, "
                    f"summary={cell(row.get('summary_json', ''))}"
                )
        else:
            lines.append("- No failed runs.")
        lines.extend(["", "## Output Paths", ""])
        for row in rows:
            lines.append(f"- {cell(row.get('label', ''))}: {cell(row.get('summary_json', ''))}")
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def _write_summary_svg_plot(self, path: Path, rows: list[dict[str, Any]]) -> None:
        def number(row: dict[str, Any], key: str) -> float | None:
            value = row.get(key, "")
            if value == "":
                return None
            try:
                return float(value)
            except (TypeError, ValueError):
                return None

        def fmt(value: float | None) -> str:
            if value is None:
                return "n/a"
            return f"{value:.2f}".rstrip("0").rstrip(".")

        def safe_text(value: Any) -> str:
            return html.escape(str(value if value != "" else "n/a"))

        width = 960
        height = 560
        margin_left = 84
        margin_right = 34
        panel_top = 78
        panel_gap = 42
        panel_height = 118
        plot_width = width - margin_left - margin_right
        labels = [str(row.get("label", f"run-{index + 1}") or f"run-{index + 1}")
                  for index, row in enumerate(rows)]
        count = max(1, len(rows))
        slot = plot_width / count

        series = [
            {
                "title": "Latency (ms)",
                "keys": [("p50Ms", "#1f4e8c", "p50"), ("p95Ms", "#c26d2d", "p95")],
                "top": panel_top,
            },
            {
                "title": "Throughput (RPS)",
                "keys": [("throughputRps", "#3d8b5b", "throughput")],
                "top": panel_top + panel_height + panel_gap,
            },
            {
                "title": "Provider utilization",
                "keys": [("providerMeanUtilization", "#6a5acd", "mean util")],
                "top": panel_top + 2 * (panel_height + panel_gap),
            },
        ]

        def panel_max(keys: list[tuple[str, str, str]]) -> float:
            values = [
                value
                for row in rows
                for key, _color, _label in keys
                for value in [number(row, key)]
                if value is not None
            ]
            if not values:
                return 1.0
            highest = max(values)
            return highest * 1.12 if highest > 0 else 1.0

        parts = [
            '<?xml version="1.0" encoding="UTF-8"?>',
            (
                f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
                f'height="{height}" viewBox="0 0 {width} {height}" role="img" '
                'aria-labelledby="title desc">'
            ),
            "<title id=\"title\">Qwen MiniNDN Sweep Plot</title>",
            (
                "<desc id=\"desc\">Latency, throughput, and provider utilization "
                "for each Qwen MiniNDN sweep run.</desc>"
            ),
            '<rect width="100%" height="100%" fill="#ffffff"/>',
            (
                '<text x="34" y="38" font-family="Arial, sans-serif" '
                'font-size="24" font-weight="700" fill="#163f7a">'
                "Qwen MiniNDN Sweep Metrics</text>"
            ),
        ]

        for panel in series:
            top = int(panel["top"])
            keys = panel["keys"]
            max_value = panel_max(keys)
            parts.extend([
                (
                    f'<text x="34" y="{top - 14}" font-family="Arial, sans-serif" '
                    f'font-size="16" font-weight="700" fill="#222">{safe_text(panel["title"])}</text>'
                ),
                (
                    f'<line x1="{margin_left}" y1="{top + panel_height}" '
                    f'x2="{width - margin_right}" y2="{top + panel_height}" '
                    'stroke="#333" stroke-width="1"/>'
                ),
                (
                    f'<line x1="{margin_left}" y1="{top}" x2="{margin_left}" '
                    f'y2="{top + panel_height}" stroke="#333" stroke-width="1"/>'
                ),
                (
                    f'<text x="{margin_left - 8}" y="{top + 5}" text-anchor="end" '
                    'font-family="Arial, sans-serif" font-size="11" fill="#555">'
                    f'{fmt(max_value)}</text>'
                ),
                (
                    f'<text x="{margin_left - 8}" y="{top + panel_height}" text-anchor="end" '
                    'font-family="Arial, sans-serif" font-size="11" fill="#555">0</text>'
                ),
            ])
            bar_group_width = min(74.0, slot * 0.72)
            bar_width = max(10.0, bar_group_width / max(1, len(keys)))
            for index, row in enumerate(rows):
                base_x = margin_left + index * slot + (slot - bar_group_width) / 2
                for key_index, (key, color, legend) in enumerate(keys):
                    value = number(row, key)
                    bar_height = 0.0 if value is None else max(0.0, min(panel_height, panel_height * value / max_value))
                    x = base_x + key_index * bar_width
                    y = top + panel_height - bar_height
                    parts.extend([
                        (
                            f'<rect x="{x:.2f}" y="{y:.2f}" width="{bar_width - 3:.2f}" '
                            f'height="{bar_height:.2f}" fill="{color}" rx="2">'
                            f'<title>{safe_text(labels[index])} {safe_text(legend)}: {fmt(value)}</title>'
                            "</rect>"
                        ),
                        (
                            f'<text x="{x + (bar_width - 3) / 2:.2f}" y="{y - 4:.2f}" '
                            'text-anchor="middle" font-family="Arial, sans-serif" '
                            'font-size="10" fill="#333">'
                            f'{fmt(value)}</text>'
                        ),
                    ])
                if panel is series[-1]:
                    label = labels[index]
                    if len(label) > 18:
                        label = label[:15] + "..."
                    parts.append(
                        f'<text x="{margin_left + index * slot + slot / 2:.2f}" '
                        f'y="{top + panel_height + 26}" text-anchor="middle" '
                        'font-family="Arial, sans-serif" font-size="11" fill="#333">'
                        f'{safe_text(label)}</text>'
                    )
            legend_x = width - margin_right - 230
            for legend_index, (_key, color, legend) in enumerate(keys):
                lx = legend_x + legend_index * 88
                parts.extend([
                    f'<rect x="{lx}" y="{top - 28}" width="12" height="12" fill="{color}"/>',
                    (
                        f'<text x="{lx + 18}" y="{top - 18}" font-family="Arial, sans-serif" '
                        f'font-size="12" fill="#333">{safe_text(legend)}</text>'
                    ),
                ])

        parts.append("</svg>")
        path.write_text("\n".join(parts) + "\n", encoding="utf-8")

    def cancel_periodic_callbacks(self) -> None:
        if self._drain_after_id is None:
            return
        try:
            self.after_cancel(self._drain_after_id)
        except Exception:
            pass
        self._drain_after_id = None

    def destroy(self) -> None:
        self.cancel_periodic_callbacks()
        self.stop_experiment()
        super().destroy()


class DistributedInferenceGui(tk.Tk):
    def __init__(self, factory: RuntimeFactory | None = None):
        super().__init__()
        self.title("NDNSF Distributed Inference")
        self.geometry("1280x820")
        self.status = tk.StringVar(value="Ready")
        self.runtime_factory = factory
        self.profile_path_var = tk.StringVar(
            value="examples/python/NDNSF-DistributedInference/gui_three_role_profile.json")
        toolbar = ttk.Frame(self)
        toolbar.pack(fill="x")
        ttk.Label(toolbar, text="Profile").pack(side="left", padx=4)
        ttk.Entry(toolbar, textvariable=self.profile_path_var, width=70).pack(
            side="left", fill="x", expand=True, padx=4)
        ttk.Button(toolbar, text="Load", command=self.load_three_role_profile).pack(side="left")
        ttk.Button(toolbar, text="Save", command=self.save_three_role_profile).pack(side="left", padx=2)
        ttk.Button(toolbar, text="Run All", command=self.run_all_roles).pack(side="left", padx=2)
        ttk.Button(toolbar, text="Stop All", command=self.stop_all_roles).pack(side="left", padx=2)
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="both", expand=True)
        self.user_tab = UserDirectTab(self.notebook, self, "user", factory=factory)
        self.provider_tab = ProviderDirectTab(self.notebook, self, "provider", factory=factory)
        self.controller_tab = ControllerDirectTab(self.notebook, self, "controller", factory=factory)
        self.wizard = WizardTab(self.notebook, self)
        self.policy_editor = PolicyEditorTab(self.notebook, self)
        self.model_split = ModelSplitTab(self.notebook, self)
        self.certificates = CertificateTab(self.notebook, self)
        self.qwen_minindn = QwenMiniNdnExperimentTab(self.notebook, self)
        self.runner = DeploymentRunnerTab(self.notebook, self)
        self.controller_runtime = RoleRuntimeTab(self.notebook, self, "controller")
        self.user_runtime = RoleRuntimeTab(self.notebook, self, "user")
        self.provider_runtime = RoleRuntimeTab(self.notebook, self, "provider")
        self.notebook.add(self.user_tab, text="User")
        self.notebook.add(self.provider_tab, text="Provider")
        self.notebook.add(self.controller_tab, text="Controller")
        self.notebook.add(self.wizard, text="Project Wizard")
        self.notebook.add(self.policy_editor, text="Policy Editor")
        self.notebook.add(self.model_split, text="Model Split")
        self.notebook.add(self.certificates, text="Certificates")
        self.notebook.add(self.qwen_minindn, text="Qwen MiniNDN")
        self.notebook.add(self.runner, text="Script Runner")
        self.notebook.add(self.controller_runtime, text="Script Controller")
        self.notebook.add(self.user_runtime, text="Script User")
        self.notebook.add(self.provider_runtime, text="Script Provider")
        ttk.Label(self, textvariable=self.status, anchor="w").pack(fill="x")
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def select_tab(self, name: str) -> None:
        for index in range(self.notebook.index("end")):
            if self.notebook.tab(index, "text") == name:
                self.notebook.select(index)
                return

    def set_status(self, value: str) -> None:
        status = getattr(self, "status", None)
        if status is not None:
            status.set(value)

    def profile(self) -> ThreeRoleGuiProfile:
        profile = ThreeRoleGuiProfile(
            env=self.user_tab.env_from_fields(),
            controller=self.controller_tab.config(),
            provider=self.provider_tab.config(),
            user=self.user_tab.config(),
            persist_tokens=self.controller_tab.bool_value("persist_tokens"),
        )
        return profile

    def apply_profile(self, profile: ThreeRoleGuiProfile) -> None:
        self.controller_tab.apply_config(profile.controller, profile.env)
        self.provider_tab.apply_config(profile.provider, profile.env)
        self.user_tab.apply_config(profile.user, profile.env)
        self.set_status("Three-role GUI profile loaded")

    def load_three_role_profile(self) -> None:
        path = self.profile_path_var.get().strip()
        if not path or not Path(path).exists():
            path = filedialog.askopenfilename(
                title="Load three-role profile",
                filetypes=[("JSON", "*.json"), ("All files", "*")],
            )
            if not path:
                return
            self.profile_path_var.set(path)
        try:
            self.apply_profile(load_three_role_profile(path))
        except Exception as exc:
            messagebox.showerror("Load profile failed", str(exc))

    def save_three_role_profile(self) -> None:
        path = self.profile_path_var.get().strip()
        if not path:
            path = filedialog.asksaveasfilename(
                title="Save three-role profile",
                defaultextension=".json",
                filetypes=[("JSON", "*.json"), ("All files", "*")],
            )
            if not path:
                return
            self.profile_path_var.set(path)
        try:
            write_three_role_profile(path, self.profile())
            self.set_status(f"Saved three-role profile: {path}")
        except Exception as exc:
            messagebox.showerror("Save profile failed", str(exc))

    def run_all_roles(self) -> None:
        self.controller_tab.run_role()
        self.provider_tab.run_role()
        self.user_tab.run_role()

    def stop_all_roles(self) -> None:
        self.controller_tab.stop_role()
        self.provider_tab.stop_role()
        self.user_tab.stop_role()
        self.qwen_minindn.stop_experiment()
        self.runner.stop_processes()

    def _cancel_role_callbacks(self) -> None:
        for tab in (self.controller_tab, self.provider_tab, self.user_tab):
            tab.cancel_periodic_callbacks()
        self.qwen_minindn.cancel_periodic_callbacks()

    def _cancel_pending_after_callbacks(self) -> None:
        try:
            pending = self.tk.call("after", "info")
        except Exception:
            return
        if isinstance(pending, str):
            after_ids = pending.split()
        else:
            after_ids = list(pending)
        for after_id in after_ids:
            try:
                self.after_cancel(after_id)
            except Exception:
                pass

    def _on_close(self) -> None:
        self.stop_all_roles()
        self.runner.stop_processes()
        self._cancel_role_callbacks()
        self.destroy()

    def destroy(self) -> None:
        self._cancel_role_callbacks()
        self._cancel_pending_after_callbacks()
        super().destroy()


def main(argv: Sequence[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    if args.headless:
        summary = run_headless(args)
        print(json.dumps(summary, indent=2))
        return 0 if summary.get("ok") else 1
    app = DistributedInferenceGui()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
