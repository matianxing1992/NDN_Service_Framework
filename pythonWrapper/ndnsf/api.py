"""Reusable Python API for orchestrating NDNSF applications.

This module is intentionally application-neutral. It does not know about HELLO,
AI, payment, model shards, or any service-specific payload. Python applications
describe which NDNSF C++ applications to run, and this layer handles process
construction, logging, local runtime library selection, and lifecycle cleanup.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import subprocess
from typing import Iterable, Mapping, Optional

from .runtime import NdnProcess, NdnRuntime, ProcessResult


@dataclass(frozen=True)
class ApplicationConfig:
    """Generic NDNSF application process description."""

    name: str
    binary: str
    args: tuple[str, ...] = ()
    env: Mapping[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class ControllerConfig:
    """ServiceController process configuration."""

    policy_file: str
    name: str = "controller"
    binary: str = "App_ServiceController"
    extra_args: tuple[str, ...] = ()
    env: Mapping[str, str] = field(default_factory=dict)

    def as_application(self) -> ApplicationConfig:
        return ApplicationConfig(
            name=self.name,
            binary=self.binary,
            args=("--policy-file", self.policy_file, *self.extra_args),
            env=self.env,
        )


@dataclass(frozen=True)
class ProviderConfig:
    """Provider process configuration.

    The optional service and role fields are metadata for Python orchestration
    and logs. Concrete command-line arguments remain application-defined.
    """

    name: str
    binary: str
    args: tuple[str, ...] = ()
    service: str = ""
    role: str = ""
    env: Mapping[str, str] = field(default_factory=dict)

    def as_application(self) -> ApplicationConfig:
        return ApplicationConfig(self.name, self.binary, self.args, self.env)


@dataclass(frozen=True)
class UserConfig:
    """User/client process configuration."""

    name: str
    binary: str
    args: tuple[str, ...] = ()
    service: str = ""
    env: Mapping[str, str] = field(default_factory=dict)

    def as_application(self) -> ApplicationConfig:
        return ApplicationConfig(self.name, self.binary, self.args, self.env)


class NDNSFSession:
    """High-level reusable Python orchestration API for NDNSF applications."""

    def __init__(
        self,
        repo_root: Optional[Path] = None,
        log_dir: Optional[Path] = None,
        *,
        binary_dir: Optional[Path] = None,
        cwd: Optional[Path] = None,
        library_dirs: Optional[Iterable[Path]] = None,
    ) -> None:
        self.runtime = NdnRuntime(
            repo_root,
            log_dir=log_dir,
            binary_dir=binary_dir,
            cwd=cwd,
            library_dirs=library_dirs,
        )

    @property
    def repo_root(self) -> Optional[Path]:
        return self.runtime.repo_root

    def command_for(self, app: ApplicationConfig) -> list[str]:
        return [
            str(self.runtime.binary(app.binary)),
            *[str(arg) for arg in app.args],
        ]

    def commands_for(self, apps: Iterable[ApplicationConfig]) -> list[list[str]]:
        return [self.command_for(app) for app in apps]

    def configure_svs_group(
        self,
        sync_prefix: str,
        strategy: str = "/localhost/nfd/strategy/multicast",
    ) -> None:
        self.runtime.configure_local_strategy(sync_prefix, strategy)

    def start_application(self, app: ApplicationConfig) -> NdnProcess:
        return self.runtime.start_process(
            app.name,
            app.binary,
            app.args,
            env=app.env,
        )

    def start_controller(self, config: ControllerConfig) -> NdnProcess:
        return self.start_application(config.as_application())

    def start_provider(self, config: ProviderConfig) -> NdnProcess:
        return self.start_application(config.as_application())

    def start_user(self, config: UserConfig) -> NdnProcess:
        return self.start_application(config.as_application())

    def run_application(
        self,
        app: ApplicationConfig,
        timeout: Optional[float] = None,
    ) -> ProcessResult:
        proc = self.runtime.make_process(
            app.name,
            app.binary,
            app.args,
            env=app.env,
        ).start()
        try:
            return proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return proc.terminate()

    def run_user(self, config: UserConfig, timeout: Optional[float] = None) -> ProcessResult:
        return self.run_application(config.as_application(), timeout=timeout)

    def stop_all(self) -> list[ProcessResult]:
        return self.runtime.stop_all()

    def __enter__(self) -> "NDNSFSession":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop_all()
