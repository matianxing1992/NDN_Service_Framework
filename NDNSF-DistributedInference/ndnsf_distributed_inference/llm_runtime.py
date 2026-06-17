"""LLM runtime adapters for NDNSF-DistributedInference.

The module keeps LLM-specific runtime glue separate from generic DI planning,
repo-backed artifact materialization, and provider readiness.  A concrete
example such as Qwen GGUF + llama-server can import these helpers while still
using the same artifact deployment and ACK readiness lifecycle as ONNX/YOLO.
"""

from __future__ import annotations

import json
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib import error, request
from urllib.parse import urlparse

from .artifact_deployment import (
    materialize_role_artifacts,
    materialized_path,
)


@dataclass(frozen=True)
class OpenAIChatRequest:
    prompt: str
    model: str = "qwen2.5-0.5b"
    system: str = ""
    max_tokens: int = 64
    temperature: float = 0.2

    def to_payload(self) -> bytes:
        messages: list[dict[str, str]] = []
        if self.system:
            messages.append({"role": "system", "content": self.system})
        messages.append({"role": "user", "content": self.prompt})
        return json.dumps({
            "model": self.model,
            "messages": messages,
            "max_tokens": int(self.max_tokens),
            "temperature": float(self.temperature),
            "stream": False,
        }, sort_keys=True).encode("utf-8")


def encode_openai_chat_request(
    prompt: str,
    *,
    model: str = "qwen2.5-0.5b",
    system: str = "",
    max_tokens: int = 64,
    temperature: float = 0.2,
) -> bytes:
    return OpenAIChatRequest(
        prompt=prompt,
        model=model,
        system=system,
        max_tokens=max_tokens,
        temperature=temperature,
    ).to_payload()


def decode_openai_chat_response(payload: bytes) -> dict[str, Any]:
    return json.loads(payload.decode("utf-8"))


class OpenAICompatibleChatRuntime:
    """Small adapter for llama-server/vLLM style chat-completion endpoints."""

    def __init__(self, base_url: str, *, timeout_s: float = 60.0):
        self.base_url = base_url.rstrip("/")
        self.timeout_s = float(timeout_s)

    def complete(self, payload: bytes) -> bytes:
        body = json.loads(payload.decode("utf-8"))
        if body.get("stream"):
            raise ValueError("streaming chat responses are not supported by this adapter")
        url = self.base_url + "/v1/chat/completions"
        req = request.Request(
            url,
            data=json.dumps(body).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with request.urlopen(req, timeout=self.timeout_s) as response:
                return response.read()
        except error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"LLM runtime HTTP {exc.code}: {detail}") from exc
        except error.URLError as exc:
            raise RuntimeError(
                f"failed to reach LLM runtime at {self.base_url}: {exc}"
            ) from exc


def call_openai_chat_runtime(
    payload: bytes,
    *,
    base_url: str = "http://127.0.0.1:8080",
    timeout_s: float = 60.0,
) -> bytes:
    return OpenAICompatibleChatRuntime(base_url, timeout_s=timeout_s).complete(payload)


class ManagedLlamaServerRuntime:
    """Process manager for a repo-materialized llama-server executable."""

    def __init__(
        self,
        executable: Path,
        model: Path,
        base_url: str,
        extra_args: list[str] | None = None,
    ):
        self.executable = Path(executable)
        self.model = Path(model)
        self.base_url = base_url.rstrip("/")
        self.extra_args = list(extra_args or [])
        self.process: subprocess.Popen | None = None

    def start(self) -> None:
        parsed = urlparse(self.base_url)
        host = parsed.hostname or "127.0.0.1"
        port = parsed.port or 8080
        command = [
            str(self.executable),
            "-m", str(self.model),
            "--host", host,
            "--port", str(port),
            *self.extra_args,
        ]
        self.process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self._wait_ready()
        print(
            "LLM_RUNTIME_MANAGED_STARTED",
            "backend=llama.cpp",
            f"pid={self.process.pid}",
            f"url={self.base_url}",
            f"model={self.model}",
            flush=True,
        )

    def stop(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.process = None

    def _wait_ready(self, timeout_s: float = 15.0) -> None:
        import urllib.request

        deadline = time.time() + timeout_s
        last_error: Exception | None = None
        while time.time() < deadline:
            if self.process is not None and self.process.poll() is not None:
                output = ""
                if self.process.stdout is not None:
                    output = self.process.stdout.read() or ""
                raise RuntimeError(
                    f"llama-server exited before becoming ready: {output}"
                )
            try:
                with urllib.request.urlopen(self.base_url + "/health", timeout=0.5) as response:
                    if response.status < 500:
                        return
            except Exception as exc:  # noqa: BLE001
                last_error = exc
            time.sleep(0.1)
        raise RuntimeError(f"llama-server did not become ready: {last_error}")


def materialize_llm_runtime_artifacts(
    *,
    artifact_references: str | Path | dict,
    role: str,
    cache_dir: str | Path,
    repo_client=None,
    model_slots: tuple[str, ...] = ("model",),
    runtime_slots: tuple[str, ...] = ("runner", "runtime"),
) -> tuple[Path, Path]:
    artifacts = materialize_role_artifacts(
        artifact_references,
        role,
        cache_dir,
        repo_client=repo_client,
    )
    model = materialized_path(artifacts, *model_slots)
    runtime = materialized_path(artifacts, *runtime_slots)
    print(
        "LLM_RUNTIME_ARTIFACTS_MATERIALIZED",
        f"role={role}",
        f"model={model}",
        f"runtime={runtime}",
        flush=True,
    )
    return model, runtime
