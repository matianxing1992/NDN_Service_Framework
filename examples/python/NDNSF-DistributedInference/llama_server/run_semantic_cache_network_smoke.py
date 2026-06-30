#!/usr/bin/env python3
"""Run a single-host NDNSF llama-server semantic-cache smoke test.

This harness starts:

1. a fake OpenAI-compatible HTTP backend,
2. the NDNSF-DI llama-server controller,
3. the semantic-cache-enabled provider,
4. several real APPClient user requests.

The model backend is fake, but the NDNSF user/controller/provider path is real.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import contextmanager
from dataclasses import asdict, dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


DEFAULT_PROMPTS = [
    "Will it rain in Memphis tomorrow?",
    "Give me tomorrow's Memphis weather forecast.",
    "Explain NDNSF semantic cache.",
    "How does NDNSF cache similar LLM answers?",
]


@dataclass(frozen=True)
class NetworkSmokeSummary:
    tmpdir: str
    requests: int
    user_successes: int
    provider_cache_hits: int
    provider_misses: int
    backend_calls: int
    passed: bool


class _FakeOpenAiState:
    def __init__(self, delay_ms: float):
        self.delay_ms = float(delay_ms)
        self.lock = threading.Lock()
        self.calls: list[dict] = []

    def record(self, payload: bytes) -> bytes:
        if self.delay_ms > 0:
            time.sleep(self.delay_ms / 1000.0)
        try:
            request = json.loads(payload.decode("utf-8"))
        except Exception:
            request = {}
        messages = request.get("messages", [])
        prompt = " ".join(
            str(item.get("content", ""))
            for item in messages
            if isinstance(item, dict) and item.get("role") == "user"
        )
        with self.lock:
            self.calls.append(request)
            call_index = len(self.calls)
        response = {
            "id": f"fake-openai-{call_index}",
            "object": "chat.completion",
            "model": request.get("model", "qwen2.5-0.5b"),
            "choices": [{
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": f"fake llama-server response {call_index}: {prompt[:40]}",
                },
                "finish_reason": "stop",
            }],
            "timings": {
                "prompt_ms": self.delay_ms,
                "predicted_ms": self.delay_ms,
                "prompt_n": max(1, len(prompt.split())),
                "predicted_n": int(request.get("max_tokens", 64) or 64),
            },
        }
        return json.dumps(response, sort_keys=True).encode("utf-8")


def _make_handler(state: _FakeOpenAiState):
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self) -> None:  # noqa: N802
            length = int(self.headers.get("content-length", "0") or "0")
            payload = self.rfile.read(length)
            response = state.record(payload)
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

        def do_GET(self) -> None:  # noqa: N802
            body = b'{"status":"ok"}'
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args) -> None:
            return None

    return Handler


@contextmanager
def fake_openai_server(delay_ms: float):
    state = _FakeOpenAiState(delay_ms)
    server = ThreadingHTTPServer(("127.0.0.1", 0), _make_handler(state))
    thread = threading.Thread(target=server.serve_forever, name="fake-openai", daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}", state
    finally:
        server.shutdown()
        thread.join(timeout=2.0)
        server.server_close()


@contextmanager
def optional_local_nfd(enabled: bool):
    started_here = False
    if enabled:
        if shutil.which("nfd-start") is None or shutil.which("nfd-stop") is None:
            raise RuntimeError("nfd-start/nfd-stop are required for --start-local-nfd")
        running = subprocess.run(
            ["pgrep", "-x", "nfd"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode == 0
        if not running:
            subprocess.run(["nfd-start"], check=True)
            started_here = True
            time.sleep(1.0)
    try:
        yield
    finally:
        if started_here:
            subprocess.run(
                ["nfd-stop"],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )


def _terminate(proc: subprocess.Popen | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3.0)


def _wait_log(path: Path, needle: str, timeout_s: float) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if path.exists() and needle in path.read_text(encoding="utf-8", errors="replace"):
            return True
        time.sleep(0.2)
    return False


def _python_env(repo_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    paths = [
        str(repo_root / "pythonWrapper"),
        str(repo_root / "NDNSF-DistributedInference"),
    ]
    existing = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = ":".join(paths + ([existing] if existing else []))
    env["PYTHONPYCACHEPREFIX"] = env.get("PYTHONPYCACHEPREFIX", "/tmp/ndnsf_pycache")
    env["LD_LIBRARY_PATH"] = ":".join([
        str(repo_root / "build"),
        env.get("LD_LIBRARY_PATH", ""),
    ]).rstrip(":")
    env["NDN_LOG"] = env.get("NDN_LOG", "ndn_service_framework.*=INFO")
    env["NDNSF_SESSION_BASE"] = str(int(time.time()) + os.getpid())
    env["NDNSF_CONFIG"] = env.get("NDNSF_CONFIG", str(Path(tempfile.gettempdir()) / "ndnsf-di-llama-smoke.conf"))
    return env


def run_network_smoke(args: argparse.Namespace) -> NetworkSmokeSummary:
    repo_root = Path(__file__).resolve().parents[4]
    example_dir = Path(__file__).resolve().parent
    tmpdir = Path(tempfile.mkdtemp(prefix="ndnsf-di-llama-semantic-network."))
    policy = tmpdir / "policy.yaml"
    generated = tmpdir / "generated"
    env = _python_env(repo_root)
    env["NDNSF_CONFIG"] = str(tmpdir / "ndnsf.conf")

    controller_proc: subprocess.Popen | None = None
    provider_proc: subprocess.Popen | None = None

    def run_checked(command: list[str], log_name: str, timeout_s: float = 30.0) -> subprocess.CompletedProcess:
        log_path = tmpdir / log_name
        with log_path.open("w", encoding="utf-8") as log:
            result = subprocess.run(
                command,
                cwd=repo_root,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=timeout_s,
                check=False,
            )
        return result

    try:
        with optional_local_nfd(args.start_local_nfd), fake_openai_server(args.backend_delay_ms) as (base_url, state):
            plan = run_checked([
                sys.executable,
                str(example_dir / "plan_llama_server.py"),
                "--policy", str(policy),
                "--providers", "1",
                "--predeployed-only",
            ], "plan.log")
            if plan.returncode != 0:
                raise RuntimeError(f"policy generation failed; see {tmpdir / 'plan.log'}")

            with (tmpdir / "controller.log").open("w", encoding="utf-8") as controller_log:
                controller_proc = subprocess.Popen(
                    [
                        sys.executable,
                        str(example_dir / "controller.py"),
                        "--config", str(policy),
                        "--generated-policy-dir", str(generated),
                    ],
                    cwd=repo_root,
                    env=env,
                    stdout=controller_log,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
            time.sleep(args.controller_wait_s)

            with (tmpdir / "provider.log").open("w", encoding="utf-8") as provider_log:
                provider_proc = subprocess.Popen(
                    [
                        sys.executable,
                        str(example_dir / "provider.py"),
                        "--config", str(policy),
                        "--generated-policy-dir", str(generated),
                        "--provider-id", "",
                        "--llama-url", base_url,
                        "--enable-semantic-cache",
                        "--semantic-cache-budget-mb", "8",
                        "--handler-workers", "1",
                    ],
                    cwd=repo_root,
                    env=env,
                    stdout=provider_log,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
            if not _wait_log(tmpdir / "provider.log", "LLAMA_SERVER_PROVIDER", args.provider_wait_s):
                time.sleep(args.provider_wait_s)

            user_successes = 0
            prompts = [item.strip() for item in args.prompts.split("|") if item.strip()]
            for index, prompt in enumerate(prompts):
                result = run_checked([
                    sys.executable,
                    str(example_dir / "user.py"),
                    "--config", str(policy),
                    "--generated-policy-dir", str(generated),
                    "--prompt", prompt,
                    "--max-tokens", str(args.max_tokens),
                    "--ack-timeout-ms", str(args.ack_timeout_ms),
                    "--timeout-ms", str(args.timeout_ms),
                    "--quiet-per-request",
                ], f"user-{index}.log", timeout_s=args.user_timeout_s)
                if result.returncode == 0:
                    user_successes += 1
                elif not args.continue_after_user_failure:
                    raise RuntimeError(f"user request {index} failed; see {tmpdir / f'user-{index}.log'}")

            provider_text = (tmpdir / "provider.log").read_text(
                encoding="utf-8",
                errors="replace",
            )
            provider_cache_hits = provider_text.count("LLAMA_SERVER_PROVIDER_SEMANTIC_CACHE status=hit")
            provider_misses = provider_text.count("LLAMA_SERVER_PROVIDER_RESPONSE")
            backend_calls = len(state.calls)
            summary = NetworkSmokeSummary(
                tmpdir=str(tmpdir),
                requests=len(prompts),
                user_successes=user_successes,
                provider_cache_hits=provider_cache_hits,
                provider_misses=provider_misses,
                backend_calls=backend_calls,
                passed=(
                    user_successes == len(prompts) and
                    provider_cache_hits >= args.min_cache_hits and
                    backend_calls < len(prompts)
                ),
            )
            (tmpdir / "summary.json").write_text(
                json.dumps(asdict(summary), indent=2) + "\n",
                encoding="utf-8",
            )
            return summary
    finally:
        _terminate(provider_proc)
        _terminate(controller_proc)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prompts", default="|".join(DEFAULT_PROMPTS))
    parser.add_argument("--backend-delay-ms", type=float, default=5.0)
    parser.add_argument("--max-tokens", type=int, default=64)
    parser.add_argument("--ack-timeout-ms", type=int, default=1200)
    parser.add_argument("--timeout-ms", type=int, default=60000)
    parser.add_argument("--controller-wait-s", type=float, default=2.0)
    parser.add_argument("--provider-wait-s", type=float, default=6.0)
    parser.add_argument("--user-timeout-s", type=float, default=80.0)
    parser.add_argument("--min-cache-hits", type=int, default=1)
    parser.add_argument("--start-local-nfd", action="store_true")
    parser.add_argument("--continue-after-user-failure", action="store_true")
    args = parser.parse_args()

    summary = run_network_smoke(args)
    print(
        "LLAMA_SERVER_SEMANTIC_CACHE_NETWORK_SMOKE_SUMMARY",
        f"tmpdir={summary.tmpdir}",
        f"requests={summary.requests}",
        f"user_successes={summary.user_successes}",
        f"provider_cache_hits={summary.provider_cache_hits}",
        f"provider_misses={summary.provider_misses}",
        f"backend_calls={summary.backend_calls}",
        f"passed={int(summary.passed)}",
        flush=True,
    )
    return 0 if summary.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
