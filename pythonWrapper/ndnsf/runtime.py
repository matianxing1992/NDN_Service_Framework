"""Process-level Python wrapper for NDNSF C++ example binaries."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import signal
import shutil
import subprocess
import time
from typing import Iterable, Mapping, Optional, Sequence


@dataclass
class ProcessResult:
    """Captured result of a completed NDNSF process."""

    name: str
    returncode: int
    stdout: str
    stderr: str


class NdnProcess:
    """Small lifecycle wrapper around a C++ NDNSF process."""

    def __init__(
        self,
        name: str,
        command: Sequence[str],
        *,
        cwd: Path,
        env: Optional[Mapping[str, str]] = None,
        stdout_path: Optional[Path] = None,
        stderr_path: Optional[Path] = None,
    ) -> None:
        self.name = name
        self.command = [str(part) for part in command]
        self.cwd = cwd
        self.env = dict(env or {})
        self.stdout_path = stdout_path
        self.stderr_path = stderr_path
        self._process: Optional[subprocess.Popen[str]] = None
        self._stdout_file = None
        self._stderr_file = None

    @property
    def pid(self) -> Optional[int]:
        return None if self._process is None else self._process.pid

    def start(self) -> "NdnProcess":
        if self._process is not None:
            raise RuntimeError(f"{self.name} is already running")
        merged_env = os.environ.copy()
        merged_env.update(self.env)
        stdout = subprocess.PIPE
        stderr = subprocess.PIPE
        if self.stdout_path is not None:
            self.stdout_path.parent.mkdir(parents=True, exist_ok=True)
            self._stdout_file = self.stdout_path.open("w", encoding="utf-8")
            stdout = self._stdout_file
        if self.stderr_path is not None:
            self.stderr_path.parent.mkdir(parents=True, exist_ok=True)
            self._stderr_file = self.stderr_path.open("w", encoding="utf-8")
            stderr = self._stderr_file
        self._process = subprocess.Popen(
            self.command,
            cwd=str(self.cwd),
            env=merged_env,
            stdout=stdout,
            stderr=stderr,
            text=True,
            start_new_session=True,
        )
        return self

    def wait(self, timeout: Optional[float] = None) -> ProcessResult:
        if self._process is None:
            raise RuntimeError(f"{self.name} has not been started")
        stdout, stderr = self._process.communicate(timeout=timeout)
        self._close_log_files()
        return ProcessResult(self.name, self._process.returncode, stdout or "", stderr or "")

    def terminate(self, timeout: float = 5.0) -> ProcessResult:
        if self._process is None:
            return ProcessResult(self.name, 0, "", "")
        if self._process.poll() is None:
            os.killpg(self._process.pid, signal.SIGTERM)
            try:
                return self.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(self._process.pid, signal.SIGKILL)
        return self.wait(timeout=timeout)

    def _close_log_files(self) -> None:
        if self._stdout_file is not None:
            self._stdout_file.close()
            self._stdout_file = None
        if self._stderr_file is not None:
            self._stderr_file.close()
            self._stderr_file = None


class NdnRuntime:
    """Factory and lifecycle manager for NDNSF example binaries."""

    def __init__(
        self,
        repo_root: Optional[Path] = None,
        log_dir: Optional[Path] = None,
        *,
        binary_dir: Optional[Path] = None,
        cwd: Optional[Path] = None,
        library_dirs: Optional[Iterable[Path]] = None,
    ) -> None:
        self.repo_root = Path(repo_root).resolve() if repo_root else None
        self.binary_dir = self._resolve_optional_path(
            binary_dir or os.environ.get("NDNSF_BINARY_DIR"))
        self.cwd = Path(cwd).resolve() if cwd else Path.cwd()
        self.log_dir = Path(log_dir) if log_dir else None
        self.library_dirs = self._resolve_library_dirs(library_dirs)
        self.processes: list[NdnProcess] = []

    def binary(self, name: str) -> Path:
        candidate = Path(name)
        if candidate.is_absolute() or len(candidate.parts) > 1:
            if candidate.exists():
                return candidate.resolve()
            raise FileNotFoundError(f"Missing NDNSF binary: {candidate}")

        search_paths = []
        if self.binary_dir is not None:
            search_paths.append(self.binary_dir / name)
        if self.repo_root is not None:
            search_paths.append(self.repo_root / "build" / "examples" / name)
        path_candidate = shutil.which(name)
        if path_candidate:
            search_paths.append(Path(path_candidate))

        for path in search_paths:
            if path.exists():
                return path.resolve()

        hint = (
            "Install NDNSF C++ binaries into PATH, set NDNSF_BINARY_DIR, or pass "
            "binary_dir=... to NDNSFSession."
        )
        if self.repo_root is not None:
            hint += " For a source tree, './waf build' should create build/examples/."
        raise FileNotFoundError(f"Cannot find NDNSF binary '{name}'. {hint}")

    @staticmethod
    def _resolve_optional_path(value: Optional[object]) -> Optional[Path]:
        if value is None or str(value) == "":
            return None
        return Path(value).expanduser().resolve()

    def _resolve_library_dirs(
        self,
        library_dirs: Optional[Iterable[Path]],
    ) -> list[Path]:
        dirs: list[Path] = []
        env_dirs = os.environ.get("NDNSF_LIBRARY_DIR", "")
        for value in env_dirs.split(os.pathsep):
            if value:
                dirs.append(Path(value).expanduser().resolve())
        if library_dirs:
            dirs.extend(Path(value).expanduser().resolve() for value in library_dirs)
        if self.repo_root is not None:
            build_dir = self.repo_root / "build"
            if build_dir.exists():
                dirs.append(build_dir.resolve())
        return dirs

    def make_process(
        self,
        name: str,
        binary: str,
        args: Iterable[str] = (),
        *,
        env: Optional[Mapping[str, str]] = None,
    ) -> NdnProcess:
        command = [str(self.binary(binary)), *[str(arg) for arg in args]]
        process_env = self._runtime_env(env)
        stdout_path = self.log_dir / f"{name}.out" if self.log_dir else None
        stderr_path = self.log_dir / f"{name}.err" if self.log_dir else None
        return NdnProcess(
            name,
            command,
            cwd=self.cwd,
            env=process_env,
            stdout_path=stdout_path,
            stderr_path=stderr_path,
        )

    def _runtime_env(self, env: Optional[Mapping[str, str]]) -> dict[str, str]:
        process_env = os.environ.copy()
        process_env.update(env or {})
        if not self.library_dirs:
            return process_env
        library_path = os.pathsep.join(str(path) for path in self.library_dirs)
        existing = process_env.get("LD_LIBRARY_PATH", os.environ.get("LD_LIBRARY_PATH", ""))
        process_env["LD_LIBRARY_PATH"] = (
            library_path if not existing else library_path + os.pathsep + existing
        )
        return process_env

    def start_process(
        self,
        name: str,
        binary: str,
        args: Iterable[str] = (),
        *,
        env: Optional[Mapping[str, str]] = None,
    ) -> NdnProcess:
        proc = self.make_process(name, binary, args, env=env).start()
        self.processes.append(proc)
        return proc

    def start_controller(
        self,
        *,
        policy_file: str,
        extra_args: Iterable[str] = (),
        env: Optional[Mapping[str, str]] = None,
    ) -> NdnProcess:
        args = ["--policy-file", policy_file, *extra_args]
        return self.start_process("controller", "App_ServiceController", args, env=env)

    def configure_local_strategy(
        self,
        prefix: str,
        strategy: str = "/localhost/nfd/strategy/multicast",
    ) -> None:
        subprocess.run(
            ["nfdc", "strategy", "set", prefix, strategy],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def stop_all(self) -> list[ProcessResult]:
        results: list[ProcessResult] = []
        for proc in reversed(self.processes):
            results.append(proc.terminate())
        self.processes.clear()
        return results

    def __enter__(self) -> "NdnRuntime":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop_all()


def wait_for_startup(seconds: float) -> None:
    """Wait for controller/provider permission discovery and SVS subscription."""

    if seconds > 0:
        time.sleep(seconds)
