#!/usr/bin/env python3
"""Local smoke for the Qwen GGUF + llama-server NDNSF-DI example."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import tempfile
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
EXAMPLE = REPO / "examples/python/NDNSF-DistributedInference/llama_server"
if str(EXAMPLE) not in sys.path:
    sys.path.insert(0, str(EXAMPLE))


def env() -> dict[str, str]:
    current = os.environ.copy()
    current["PYTHONPATH"] = (
        f"{REPO / 'NDNSF-DistributedInference'}:"
        f"{REPO / 'pythonWrapper'}:"
        f"{EXAMPLE}:"
        f"{current.get('PYTHONPATH', '')}"
    )
    return current


def run(command: list[str]) -> str:
    print("$ " + " ".join(command))
    proc = subprocess.run(
        command,
        cwd=str(REPO),
        env=env(),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(proc.stdout, end="")
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)
    return proc.stdout


class FakeLlamaHandler(BaseHTTPRequestHandler):
    def do_POST(self):  # noqa: N802 - stdlib callback name
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length).decode("utf-8"))
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return
        prompt = body.get("messages", [{}])[-1].get("content", "")
        payload = json.dumps({
            "id": "chatcmpl-ndnsf-smoke",
            "object": "chat.completion",
            "model": body.get("model", "qwen2.5-0.5b"),
            "choices": [{
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": "NDNSF-DI llama-server smoke: " + prompt[:24],
                },
                "finish_reason": "stop",
            }],
        }).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args):  # noqa: A002 - stdlib signature
        return


class FakeCtx:
    def __init__(self, request_payload: bytes):
        self.request = request_payload
        self.role = "/LLM/LlamaServer"
        self.payload = b""
        self.failed = ""
        self.ndnsf = self

    def publish_final_response(self, payload: bytes) -> None:
        self.payload = payload

    def fail(self, message: str) -> None:
        self.failed = message


def run_provider_adapter_smoke() -> None:
    from llama_server_lib import decode_chat_response, encode_chat_request
    import provider as provider_module

    server = HTTPServer(("127.0.0.1", 0), FakeLlamaHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        os.environ["NDNSF_DI_LLAMA_SERVER_URL"] = (
            f"http://127.0.0.1:{server.server_port}")
        ctx = FakeCtx(encode_chat_request("hello from qwen smoke"))
        provider_module.handle_llama_server(ctx)
        if ctx.failed:
            raise SystemExit(ctx.failed)
        decoded = decode_chat_response(ctx.payload)
        content = decoded["choices"][0]["message"]["content"]
        if "NDNSF-DI llama-server smoke" not in content:
            raise SystemExit(f"unexpected fake llama-server response: {decoded}")
        print("LLAMA_SERVER_PROVIDER_ADAPTER_SMOKE_OK")
    finally:
        server.shutdown()
        thread.join(timeout=2)


def run_artifact_provisioning_state_smoke() -> None:
    from ndnsf_distributed_inference import ArtifactProvisioningState

    state = ArtifactProvisioningState(component="smoke runtime")
    release_install = threading.Event()
    state.start_install(
        lambda: (release_install.wait(2.0) and "ready-resource"),
        installing_message="installing smoke runtime",
        ready_message="smoke runtime ready",
        start_marker="LLAMA_SERVER_GENERIC_INSTALL_STARTED",
        fail_marker="LLAMA_SERVER_GENERIC_INSTALL_FAILED",
    )
    installing_ack = state.ack()
    if installing_ack.status:
        raise SystemExit("installing artifact runtime should not be selectable")
    release_install.set()
    if not state.wait_ready(2.0):
        raise SystemExit("artifact provisioning did not become ready")
    ready_ack = state.ack()
    if not ready_ack.status or b"runtimeStatus=ready" not in ready_ack.payload:
        raise SystemExit(f"unexpected ready ACK: {ready_ack}")
    state.require_ready()

    failed = ArtifactProvisioningState(component="failed smoke runtime")
    failed.start_install(
        lambda: (_ for _ in ()).throw(RuntimeError("planned install failure")),
        start_marker="LLAMA_SERVER_GENERIC_INSTALL_STARTED",
        fail_marker="LLAMA_SERVER_GENERIC_INSTALL_FAILED",
    )
    if failed.wait_ready(0.5):
        raise SystemExit("failed artifact runtime should not become ready")
    failed_ack = failed.ack()
    if failed_ack.status or b"runtimeStatus=failed" not in failed_ack.payload:
        raise SystemExit(f"unexpected failed ACK: {failed_ack}")
    print("NDNSF_DI_ARTIFACT_PROVISIONING_STATE_SMOKE_OK")


def write_fake_llama_server(path: Path) -> None:
    path.write_text(r'''#!/usr/bin/env python3
import argparse
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

parser = argparse.ArgumentParser()
parser.add_argument("-m", "--model", default="")
parser.add_argument("--host", default="127.0.0.1")
parser.add_argument("--port", type=int, default=8080)
args, _ = parser.parse_known_args()

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/health":
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"OK")
            return
        self.send_error(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length).decode("utf-8"))
        prompt = body.get("messages", [{}])[-1].get("content", "")
        payload = json.dumps({
            "id": "chatcmpl-fake-managed",
            "object": "chat.completion",
            "model": body.get("model", "qwen2.5-0.5b"),
            "choices": [{
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": "managed llama-server: " + prompt[:24],
                },
                "finish_reason": "stop",
            }],
        }).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args):
        return

HTTPServer((args.host, args.port), Handler).serve_forever()
''', encoding="utf-8")
    path.chmod(0o755)


def run_repo_backed_materialize_smoke() -> None:
    from ndnsf_distributed_inference import LocalDistributedRepo, StorageCapability
    from llama_server_lib import (
        build_llama_server_artifact_references,
        decode_chat_response,
        encode_chat_request,
    )
    import provider as provider_module

    with tempfile.TemporaryDirectory(prefix="ndnsf-di-llama-artifact-smoke-") as tmp:
        root = Path(tmp)
        fake_model = root / "Qwen2.5-0.5B-Instruct-Q4_K_M.gguf"
        fake_runtime = root / "llama-server"
        fake_model.write_bytes(b"fake qwen gguf model for artifact smoke")
        write_fake_llama_server(fake_runtime)
        repo = LocalDistributedRepo([
            StorageCapability(repo_node="/local/repo", free_bytes=1_000_000_000)
        ])
        references = build_llama_server_artifact_references(
            repo,
            model_path=fake_model,
            llama_server_path=fake_runtime,
            include_local_payload_paths=False,
        )
        materialized_model, materialized_runtime = (
            provider_module.materialize_llama_server_artifacts(
                artifact_references=references,
                cache_dir=str(root / "cache"),
                repo_client=repo,
            )
        )
        managed = provider_module.ManagedLlamaServer(
            materialized_runtime,
            materialized_model,
            "http://127.0.0.1:18081",
        )
        managed.start()
        try:
            os.environ["NDNSF_DI_LLAMA_SERVER_URL"] = "http://127.0.0.1:18081"
            ctx = FakeCtx(encode_chat_request("repo artifact deployment"))
            provider_module.handle_llama_server(ctx)
            if ctx.failed:
                raise SystemExit(ctx.failed)
            decoded = decode_chat_response(ctx.payload)
            content = decoded["choices"][0]["message"]["content"]
            if "managed llama-server" not in content:
                raise SystemExit(f"unexpected managed response: {decoded}")
            print("LLAMA_SERVER_REPO_BACKED_MATERIALIZE_SMOKE_OK")
        finally:
            managed.stop()


def main() -> int:
    out = Path("/tmp/ndnsf-di-llama-server-smoke")
    out.mkdir(parents=True, exist_ok=True)
    policy = out / "llama_policy.yaml"
    generated = out / "generated-policy"
    native_plan = generated / "native-execution-plan.json"
    run([
        sys.executable,
        "examples/python/NDNSF-DistributedInference/llama_server/plan_llama_server.py",
        "--policy", str(policy),
        "--model", "Qwen2.5-0.5B-Instruct-Q4_K_M.gguf",
        "--llama-server", "llama-server",
        "--providers", "2",
    ])
    run([
        sys.executable,
        "examples/python/NDNSF-DistributedInference/llama_server/download_qwen_gguf.py",
        "--dry-run",
        "--dest", str(out / "models"),
    ])
    run([
        sys.executable,
        "-c",
        "from ndnsf_distributed_inference.policy import main; raise SystemExit(main())",
        "--config", str(policy),
        "--out-dir", str(generated),
        "--print-summary",
    ])
    run([
        "build/examples/di-native-plan-schema-smoke",
        str(native_plan),
        "/AI/LLM/Qwen2.5-0.5B/LlamaServer",
        "llm",
        "gguf",
        "llm-pipeline",
    ])
    run_provider_adapter_smoke()
    run_artifact_provisioning_state_smoke()
    run_repo_backed_materialize_smoke()
    print(f"NDNSF_DI_LLAMA_SERVER_SMOKE_OK policy={policy} native_plan={native_plan}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
