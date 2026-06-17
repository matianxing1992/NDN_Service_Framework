#!/usr/bin/env python3
"""MiniNDN smoke for repo-backed Qwen GGUF + llama-server NDNSF-DI.

This uses tiny fake artifacts by default. The point is not LLM quality; it is
the deployment shape: repo node stores runtime/model artifacts, the provider
materializes them inside MiniNDN, starts a local llama-server-compatible
adapter, and the user invokes the LLM inference service through NDNSF.
"""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import io
import json
import os
import pwd
import re
import signal
import subprocess
import sys
import tarfile
import time
from pathlib import Path
from urllib import request

import yaml  # type: ignore

REPO = Path(__file__).resolve().parents[1]
MININDN_ROOT = Path("/tmp/minindn")
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from mininet.log import info, setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.apps.nlsr import Nlsr  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402
from minindn.util import getPopen  # noqa: E402

TOPO = REPO / "Experiments/Topology/AI_Lab.conf"
OUT = REPO / "results/llama_server_minindn_smoke"
LLAMA_DIR = REPO / "examples/python/NDNSF-DistributedInference/llama_server"
REPO_DIR = REPO / "examples/python/NDNSF-DistributedRepo/generic_object_store"
CONFIG = OUT / "llama_server_policy.yaml"
GEN_POLICY = "/tmp/ndnsf-di-llama-server-minindn-policy"
ARTIFACT_REFS = OUT / "llama-server-artifacts.json"
APP_ROOT = "/example/llama"
CONTROLLER_IDENTITY = APP_ROOT + "/controller"
GROUP_IDENTITY = APP_ROOT + "/group"
USER_IDENTITY = APP_ROOT + "/user"
PROVIDER_PREFIX = APP_ROOT + "/provider"
LLM_PROVIDER_ID = "llmA"
REPO_PROVIDER_ID = "repoA"
LLM_PROVIDER_IDENTITY = PROVIDER_PREFIX + "/" + LLM_PROVIDER_ID
REPO_PROVIDER_IDENTITY = PROVIDER_PREFIX + "/" + REPO_PROVIDER_ID
REPO_SERVICE = "/NDNSF/DistributedRepo"
LLM_SERVICE = "/AI/LLM/Qwen2.5-0.5B/LlamaServer"
CONTROLLER_NODE = "memphis"
USER_NODE = "memphis"
REPO_NODE = "neu"
LLM_PROVIDER_NODE = "ucla"


class CleanNlsr(Nlsr):
    def createConfigFile(self):
        super().createConfigFile()
        conf = Path(self.confFile)
        text = conf.read_text(encoding="utf-8")
        clean_block = (
            "advertising\n"
            "{\n"
            f"    /ndn/{self.node.name}-site/{self.node.name} 0\n"
            "}\n"
        )
        text = re.sub(r"advertising\s*\{.*?\}\n", clean_block, text,
                      count=1, flags=re.S)
        conf.write_text(text, encoding="utf-8")


def log(message: str) -> None:
    info(message + "\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="MiniNDN smoke for repo-backed llama-server LLM inference service")
    parser.add_argument("--topology-file", default=str(TOPO))
    parser.add_argument("--output-dir", default=str(OUT))
    parser.add_argument("--nlsr-wait-s", type=float, default=8.0)
    parser.add_argument("--controller-wait-s", type=float, default=8.0)
    parser.add_argument("--repo-start-wait-s", type=float, default=12.0)
    parser.add_argument("--provider-start-timeout-s", type=float, default=60.0)
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=60000)
    parser.add_argument("--prompt", default="Say hello from NDNSF-DI MiniNDN.")
    parser.add_argument("--real-artifacts", action="store_true",
                        help="Use a real llama-server bundle and GGUF model instead of tiny fake artifacts")
    parser.add_argument("--artifact-source", choices=["repo", "local-reference"],
                        default="repo",
                        help="Where provider materialization fetches artifacts from. "
                             "'repo' stores artifacts in DistributedRepo; "
                             "'local-reference' uses localPayloadPath for large real-model smoke.")
    parser.add_argument("--model-path",
                        default=str(REPO / "third_party/qwen/qwen2.5-0.5b-instruct-q4_k_m.gguf"))
    parser.add_argument("--llama-runtime-dir",
                        default=str(REPO / "third_party/llama.cpp-local/bin"))
    parser.add_argument("--llama-server-extra-arg", action="append", default=[],
                        help="Extra arg passed to managed llama-server on provider")
    parser.add_argument("--max-tokens", type=int, default=16)
    parser.add_argument("--local-baseline-port", type=int, default=18182)
    parser.add_argument("--measured-duration-s", type=float, default=0.0,
                        help="Measured distributed duration. 0 keeps single-request smoke behavior")
    parser.add_argument("--measured-requests", type=int, default=1,
                        help="Measured distributed request count when --measured-duration-s is 0")
    parser.add_argument("--warmup-requests", type=int, default=1,
                        help="Local baseline warmup requests before measured local loop")
    parser.add_argument("--local-measured-requests", type=int, default=5,
                        help="Local direct llama-server measured request count for real-artifact benchmark")
    parser.add_argument("--request-interval-ms", type=float, default=1000.0,
                        help="Minimum interval between distributed request starts in measured mode")
    parser.add_argument("--ndn-log", default="ndn_service_framework.*=WARN",
                        help="NDN_LOG value for MiniNDN app processes")
    return parser


def normalize_nlsr_link_costs(ndn) -> None:
    for host in ndn.net.hosts:
        for intf in host.intfList():
            delay = intf.params.get("delay")
            if not delay or not str(delay).endswith("ms"):
                continue
            try:
                value = str(delay)[:-2]
                intf.params["delay"] = f"{max(1, int(round(float(value))))}ms"
            except ValueError:
                pass


def key_name_from_certificate_name(cert_name: str) -> str:
    return cert_name.rsplit("/", 2)[0]


def command_env(homes: dict[str, Path], host_name: str, base_env: dict[str, str]) -> dict[str, str]:
    return {
        **base_env,
        "HOME": str(homes[host_name]),
        "NDN_CLIENT_CONF": str(homes[host_name] / ".ndn/client.conf"),
        "NDN_CLIENT_TRANSPORT": f"unix:///run/nfd/{host_name}.sock",
    }


def start_process(ndn, host_name: str, label: str, cmd: str,
                  env: dict[str, str], processes: list[tuple[object, object, Path]]):
    log_path = OUT / f"{label}.log"
    log(f"start {label} on {host_name}: {cmd}")
    out = log_path.open("wb")
    proc = getPopen(ndn.net[host_name], cmd, envDict=env, shell=True,
                    stdout=out, stderr=subprocess.STDOUT)
    processes.append((proc, out, log_path))
    return proc, log_path


def stop_processes(processes: list[tuple[object, object, Path]]) -> None:
    for proc, file, _ in reversed(processes):
        if proc.poll() is None:
            try:
                proc.send_signal(signal.SIGINT)
                proc.wait(timeout=3)
            except Exception:
                proc.kill()
        try:
            file.close()
        except Exception:
            pass


def wait_log(path: Path, needle: str, timeout_s: float, proc=None) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.25)
    return False


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
            "id": "chatcmpl-minindn-managed",
            "object": "chat.completion",
            "model": body.get("model", "qwen2.5-0.5b"),
            "choices": [{
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": "MiniNDN managed llama-server: " + prompt[:32],
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


def write_llama_server_bundle(runtime_dir: Path, output: Path) -> Path:
    runtime_dir = runtime_dir.resolve()
    server = runtime_dir / "llama-server"
    if not server.exists():
        raise FileNotFoundError(f"llama-server not found under {runtime_dir}")
    files = [
        path for path in runtime_dir.iterdir()
        if path.name == "llama-server" or path.name.startswith("lib")
    ]
    if not files:
        raise RuntimeError(f"no llama-server runtime files found under {runtime_dir}")
    tar_bytes = io.BytesIO()
    with tarfile.open(fileobj=tar_bytes, mode="w:gz") as tf:
        for path in files:
            tf.add(path, arcname=path.name, recursive=False)
    encoded = base64.b64encode(tar_bytes.getvalue()).decode("ascii")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        """#!/usr/bin/env bash
set -euo pipefail
self="$0"
bundle_dir="$(dirname "$self")/.llama-server-unpacked-$(sha256sum "$self" | awk '{print $1}' | cut -c1-16)"
server="$bundle_dir/llama-server"
if [ ! -x "$server" ]; then
  rm -rf "$bundle_dir"
  mkdir -p "$bundle_dir"
  awk '/^__NDNSF_LLAMA_BUNDLE_BELOW__$/ { found=1; next } found { print }' "$self" | base64 -d | tar -xzf - -C "$bundle_dir"
  chmod +x "$server" || true
fi
export LD_LIBRARY_PATH="$bundle_dir:${LD_LIBRARY_PATH:-}"
exec "$server" "$@"
exit 0
__NDNSF_LLAMA_BUNDLE_BELOW__
""" + encoded + "\n",
        encoding="utf-8",
    )
    output.chmod(0o755)
    return output


def prepare_policy_and_artifacts(args) -> tuple[Path, Path]:
    OUT.mkdir(parents=True, exist_ok=True)
    if args.real_artifacts:
        model_path = Path(args.model_path).expanduser().resolve()
        if not model_path.exists():
            raise FileNotFoundError(
                f"real Qwen GGUF model not found: {model_path}. "
                "Run examples/python/NDNSF-DistributedInference/llama_server/download_qwen_gguf.py first.")
        runtime = write_llama_server_bundle(
            Path(args.llama_runtime_dir).expanduser().resolve(),
            OUT / "llama-server-bundle",
        )
    else:
        model_path = OUT / "Qwen2.5-0.5B-Instruct-Q4_K_M.gguf"
        runtime = OUT / "llama-server"
        model_path.write_bytes(b"fake qwen gguf model for MiniNDN repo-backed smoke\n")
        write_fake_llama_server(runtime)
    subprocess.run([
        sys.executable,
        str(LLAMA_DIR / "plan_llama_server.py"),
        "--policy", str(CONFIG),
        "--model", str(model_path),
        "--llama-server", str(runtime),
        "--providers", "1",
    ], cwd=str(REPO), check=True)
    config = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
    config["controller"] = CONTROLLER_IDENTITY
    config["group"] = GROUP_IDENTITY
    config.setdefault("runtime", {})["user_identity"] = USER_IDENTITY
    config.setdefault("runtime", {})["provider_prefix"] = PROVIDER_PREFIX
    config.setdefault("trust", {})["app_roots"] = [APP_ROOT]
    for service in config.get("services", []):
        if service.get("name") == LLM_SERVICE:
            service["users"] = [USER_IDENTITY]
            service["providers"] = [{
                "identity": LLM_PROVIDER_IDENTITY,
                "roles": "all",
            }]
    config.setdefault("services", []).append({
        "name": REPO_SERVICE,
        "model": REPO_SERVICE,
        "roles": [],
        "dependencies": [],
        "users": [
            CONTROLLER_IDENTITY,
            LLM_PROVIDER_IDENTITY,
        ],
        "providers": [{
            "identity": REPO_PROVIDER_IDENTITY,
            "roles": [],
        }],
    })
    CONFIG.write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")
    return model_path, runtime


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * (pct / 100.0)
    lower = int(rank)
    upper = min(lower + 1, len(ordered) - 1)
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize_values(values: list[float]) -> dict[str, float | int]:
    return {
        "count": len(values),
        "avg_ms": sum(values) / len(values) if values else 0.0,
        "min_ms": min(values) if values else 0.0,
        "p50_ms": percentile(values, 50),
        "p95_ms": percentile(values, 95),
        "max_ms": max(values) if values else 0.0,
    }


def parse_user_csv(path: Path) -> dict:
    rows = []
    if path.exists():
        with path.open(newline="", encoding="utf-8") as file:
            rows = list(csv.DictReader(file))
    elapsed = [float(row["elapsed_ms"]) for row in rows if row.get("elapsed_ms")]
    provider = [
        float(row["provider_total_ms"])
        for row in rows
        if row.get("provider_total_ms") not in (None, "")
    ]
    overhead = [
        max(0.0, float(row["elapsed_ms"]) - float(row["provider_total_ms"]))
        for row in rows
        if row.get("elapsed_ms") and row.get("provider_total_ms") not in (None, "")
    ]
    return {
        "rows": rows,
        "elapsed": summarize_values(elapsed),
        "provider": summarize_values(provider),
        "estimated_ndnsf_overhead": summarize_values(overhead),
    }


def run_local_real_baseline(runtime: Path, model: Path, *, prompt: str,
                            max_tokens: int, port: int,
                            extra_args: list[str],
                            warmup_requests: int = 1,
                            measured_requests: int = 5) -> tuple[float, float, dict]:
    log_path = OUT / "llama-local-real.log"
    proc = subprocess.Popen(
        [
            str(runtime),
            "-m", str(model),
            "--host", "127.0.0.1",
            "--port", str(port),
            "--ctx-size", "512",
            "--threads", "2",
            "--parallel", "1",
            "--no-webui",
            *extra_args,
        ],
        cwd=str(REPO),
        stdout=log_path.open("wb"),
        stderr=subprocess.STDOUT,
    )
    base_url = f"http://127.0.0.1:{port}"
    try:
        deadline = time.time() + 180
        while time.time() < deadline:
            try:
                with request.urlopen(base_url + "/health", timeout=1) as response:
                    if response.status < 500:
                        break
            except Exception:
                if proc.poll() is not None:
                    raise RuntimeError(
                        f"local llama-server exited early; log={log_path}")
                time.sleep(0.5)
        else:
            raise RuntimeError(f"local llama-server did not become ready; log={log_path}")

        def infer() -> float:
            body = {
                "model": "qwen2.5-0.5b",
                "messages": [{"role": "user", "content": prompt}],
                "max_tokens": int(max_tokens),
                "temperature": 0.0,
                "stream": False,
            }
            req = request.Request(
                base_url + "/v1/chat/completions",
                data=json.dumps(body).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            start = time.perf_counter()
            with request.urlopen(req, timeout=180) as response:
                response.read()
            return (time.perf_counter() - start) * 1000.0

        cold = infer()
        warm_values = []
        for _ in range(max(0, int(warmup_requests))):
            warm_values.append(infer())
        measured_values = [infer() for _ in range(max(1, int(measured_requests)))]
        warm = measured_values[0]
        summary = {
            "cold_ms": cold,
            "warmup_ms": warm_values,
            "measured": summarize_values(measured_values),
            "measured_values_ms": measured_values,
        }
        (OUT / "llama-local-real-baseline.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        print(
            "LLAMA_SERVER_LOCAL_REAL_BASELINE",
            f"cold_ms={cold:.2f}",
            f"warm_ms={warm:.2f}",
            f"measured_avg_ms={summary['measured']['avg_ms']:.2f}",
            f"measured_p50_ms={summary['measured']['p50_ms']:.2f}",
            f"measured_p95_ms={summary['measured']['p95_ms']:.2f}",
            f"log={log_path}",
        )
        return cold, warm, summary
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)


def _local_artifact_entry(path: Path, *, object_name: str, object_type: str,
                          object_id: str, kind: str, executable: bool,
                          metadata: dict) -> dict:
    payload = path.read_bytes()
    manifest = {
        "objectName": object_name,
        "objectType": object_type,
        "objectClass": object_type,
        "sha256": hashlib.sha256(payload).hexdigest(),
        "size": len(payload),
        "segmentCount": 1,
        "replicationFactor": 1,
        "minReplicationFactor": 1,
        "maxReplicationFactor": 1,
        "policyEpoch": "/Policy/llama-server-qwen/v1",
        "metadata": {
            **metadata,
            "localPayloadPath": str(path),
        },
    }
    return {
        "source": "local-reference",
        "localPayloadPath": str(path),
        "repoManifest": manifest,
        "objectType": object_type,
        "objectId": object_id,
        "filename": path.name,
        "kind": kind,
        "executable": executable,
        "metadata": {
            **metadata,
            "localPayloadPath": str(path),
        },
    }


def write_local_artifact_references(model: Path, runtime: Path, out: Path) -> Path:
    refs = {
        "schemaVersion": 1,
        "roles": {
            "/LLM/LlamaServer": {
                "model": _local_artifact_entry(
                    model,
                    object_name="/local/NDNSF-DI/ARTIFACT/AI/LLM/Qwen2.5-0.5B/model",
                    object_type="model-artifact",
                    object_id="/Model/Qwen2.5-0.5B-Instruct-Q4_K_M/GGUF",
                    kind="model",
                    executable=False,
                    metadata={
                        "modelFamily": "llm",
                        "modelFormat": "gguf",
                        "runtimeBackend": "llama.cpp",
                        "filename": model.name,
                    },
                ),
                "runner": _local_artifact_entry(
                    runtime,
                    object_name="/local/NDNSF-DI/ARTIFACT/AI/LLM/Qwen2.5-0.5B/runtime/llama-server",
                    object_type="runtime-executable",
                    object_id="/Runtime/llama.cpp/llama-server",
                    kind="runtime",
                    executable=True,
                    metadata={
                        "runtimeBackend": "llama.cpp",
                        "entrypoint": "llama-server",
                        "filename": runtime.name,
                    },
                ),
            },
        },
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(refs, indent=2, sort_keys=True), encoding="utf-8")
    print(f"LLAMA_SERVER_LOCAL_ARTIFACT_REFERENCES_OK out={out}")
    return out


def generate_policy_bundle(env: dict[str, str]) -> None:
    subprocess.run([
        sys.executable,
        "-c",
        "from ndnsf_distributed_inference.policy import main; raise SystemExit(main())",
        "--config", str(CONFIG),
        "--out-dir", str(GEN_POLICY),
        "--print-summary",
    ], cwd=str(REPO), env=env, check=True)


def main() -> int:
    global OUT, CONFIG, ARTIFACT_REFS
    global GEN_POLICY
    args = build_parser().parse_args()
    sys.argv = [sys.argv[0]]
    setLogLevel("info")
    OUT = Path(args.output_dir).expanduser().resolve()
    CONFIG = OUT / "llama_server_policy.yaml"
    ARTIFACT_REFS = OUT / "llama-server-artifacts.json"
    GEN_POLICY = str(OUT / "generated-policy")
    model_path, runtime_path = prepare_policy_and_artifacts(args)
    local_cold_ms = None
    local_warm_ms = None
    local_summary = {}
    if args.real_artifacts:
        local_cold_ms, local_warm_ms, local_summary = run_local_real_baseline(
            runtime_path,
            model_path,
            prompt=args.prompt,
            max_tokens=args.max_tokens,
            port=args.local_baseline_port,
            extra_args=args.llama_server_extra_arg,
            warmup_requests=args.warmup_requests,
            measured_requests=args.local_measured_requests,
        )
    base_env = {
        **os.environ,
        "PYTHONFAULTHANDLER": "1",
        "PYTHONUNBUFFERED": "1",
        "PYTHONPATH": ":".join([
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "pythonWrapper"),
            str(LLAMA_DIR),
            str(REPO / "Experiments"),
            os.environ.get("PYTHONPATH", ""),
        ]),
        "NDN_LOG": args.ndn_log,
        "NDNSF_DI_CLIENT_TIMING": "0",
        "NDNSF_DI_PROVIDER_TIMING": "0",
        "NDNSF_RESPONSE_LARGE_DATA_THRESHOLD": "1024",
    }
    base_env.pop("NDN_CLIENT_TRANSPORT", None)
    generate_policy_bundle(base_env)

    subprocess.run(["pkill", "-f", "llama_server/(provider|user|deploy_artifacts)\\.py"],
                   check=False)
    subprocess.run(["pkill", "-f", "generic_object_store/(repo_node|controller)\\.py"],
                   check=False)
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=args.topology_file)
    processes: list[tuple[object, object, Path]] = []
    try:
        ndn.start()
        normalize_nlsr_link_costs(ndn)
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        AppManager(ndn, ndn.net.hosts, CleanNlsr, sync="psync", security=False,
                   faceType="udp", nFaces=3, routingType="link-state",
                   logLevel="INFO")
        perf.wait_for_nfd_sockets(ndn, OUT)

        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin(
            [ndn.net[CONTROLLER_NODE]],
            [CONTROLLER_IDENTITY, CONTROLLER_IDENTITY + "/DKEY",
             CONTROLLER_IDENTITY + "/KEY", APP_ROOT, APP_ROOT + "/KEY"],
        )
        rh.addOrigin([ndn.net[USER_NODE]], [USER_IDENTITY, USER_IDENTITY + "/KEY"])
        rh.addOrigin(
            [ndn.net[LLM_PROVIDER_NODE]],
            [LLM_PROVIDER_IDENTITY, LLM_PROVIDER_IDENTITY + "/KEY"],
        )
        rh.addOrigin(
            [ndn.net[REPO_NODE]],
            [REPO_PROVIDER_IDENTITY, REPO_PROVIDER_IDENTITY + "/KEY",
             "/NDNSF/DistributedRepo/Object"],
        )
        rh.addOrigin(ndn.net.hosts, [GROUP_IDENTITY])
        rh.calculateRoutes()
        log(f"Waiting {args.nlsr_wait_s:.1f}s for NLSR convergence")
        time.sleep(args.nlsr_wait_s)
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, APP_ROOT, Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, GROUP_IDENTITY, Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF/DistributedRepo/Object",
                             Nfdc.STRATEGY_MULTICAST)

        node_identities = [
            (CONTROLLER_NODE, CONTROLLER_IDENTITY),
            (USER_NODE, USER_IDENTITY),
            (LLM_PROVIDER_NODE, LLM_PROVIDER_IDENTITY),
            (REPO_NODE, REPO_PROVIDER_IDENTITY),
        ]
        identities_by_node: dict[str, str] = {}
        for host_name, identity in node_identities:
            identities_by_node.setdefault(host_name, identity)
        homes: dict[str, Path] = {}
        for host_name in sorted(set(identities_by_node)):
            home = MININDN_ROOT / host_name
            ndn_dir = home / ".ndn"
            subprocess.run(["rm", "-rf", str(ndn_dir)], check=False)
            ndn_dir.mkdir(parents=True, exist_ok=True)
            (ndn_dir / "client.conf").write_text(
                f"transport=unix:///run/nfd/{host_name}.sock\n",
                encoding="utf-8",
            )
            homes[host_name] = home

        passphrase = "ndnsf-minindn"
        root_cert = OUT / "root.cert"
        controller_node = ndn.net[CONTROLLER_NODE]
        for node in ndn.net.hosts:
            for identity in [APP_ROOT, *[identity for _, identity in node_identities]]:
                perf.node_cmd(node, "ndnsec delete {} >/dev/null 2>&1 || true".format(
                    perf.shell_quote(identity)))
        perf.node_cmd(controller_node, "ndnsec key-gen -t r {} > {}".format(
            perf.shell_quote(APP_ROOT), perf.shell_quote(root_cert)))
        perf.node_cmd(controller_node,
                      "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                          perf.shell_quote(root_cert)))

        exported_keys = []
        cert_names = {}
        for index, (host_name, identity) in enumerate(node_identities):
            req = OUT / f"{host_name}-{index}.req"
            cert = OUT / f"{host_name}-{index}.cert"
            key = OUT / f"{host_name}-{index}.ndnkey"
            perf.node_cmd(controller_node, "ndnsec key-gen -t r {} > {}".format(
                perf.shell_quote(identity), perf.shell_quote(req)))
            perf.node_cmd(controller_node,
                          "ndnsec cert-gen -s {} -i ROOT {} > {}".format(
                              perf.shell_quote(APP_ROOT), perf.shell_quote(req),
                              perf.shell_quote(cert)))
            perf.node_cmd(controller_node,
                          "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                              perf.shell_quote(cert)))
            cert_name = perf.certificate_name_from_file(cert)
            cert_names[host_name] = cert_name
            key_name = key_name_from_certificate_name(cert_name)
            perf.node_cmd(controller_node,
                          "ndnsec set-default -k -n {} >/dev/null 2>&1 || true".format(
                              perf.shell_quote(key_name)))
            perf.node_cmd(controller_node, "ndnsec-export -P {} -o {} -k {}".format(
                perf.shell_quote(passphrase), perf.shell_quote(key),
                perf.shell_quote(key_name)))
            exported_keys.append((key, key_name))

        for host_name in sorted(set(identities_by_node)):
            perf.node_cmd(ndn.net[host_name],
                          "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                              perf.shell_quote(root_cert)))
            for key, _ in exported_keys:
                perf.node_cmd(ndn.net[host_name],
                              "ndnsec import -P {} {} >/dev/null 2>&1 || true".format(
                                  perf.shell_quote(passphrase), perf.shell_quote(key)))
            perf.node_cmd(ndn.net[host_name],
                          "ndnsec set-default -n {} >/dev/null 2>&1 || true".format(
                              perf.shell_quote(identities_by_node[host_name])))
            perf.node_cmd(ndn.net[host_name],
                          "ndnsec set-default -c -n {} >/dev/null 2>&1 || true".format(
                              perf.shell_quote(cert_names[host_name])))

        config_obj = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
        config_obj.setdefault("trust", {})["anchor_file"] = str(root_cert)
        CONFIG.write_text(yaml.safe_dump(config_obj, sort_keys=False),
                          encoding="utf-8")
        generate_policy_bundle(base_env)

        node_env = {
            name: command_env(homes, name, base_env)
            for name in sorted(set(identities_by_node))
        }
        base = f"cd {perf.shell_quote(REPO)} && exec python3 "
        common = " --config {} --generated-policy-dir {}".format(
            perf.shell_quote(CONFIG), perf.shell_quote(GEN_POLICY))
        start_process(
            ndn, CONTROLLER_NODE, "controller",
            base + "-c " + perf.shell_quote(
                "from ndnsf_distributed_inference import APPController; "
                "import sys; "
                "c=APPController.from_config(sys.argv[1], generated_policy_dir=sys.argv[2]); "
                "print('controller ready', flush=True); c.run()"
            ) + " " + perf.shell_quote(CONFIG) + " " + perf.shell_quote(GEN_POLICY),
            node_env[CONTROLLER_NODE], processes,
        )
        time.sleep(args.controller_wait_s)
        start_process(
            ndn, REPO_NODE, "repoA",
            base + perf.shell_quote(REPO_DIR / "repo_node.py") + common +
            f" --provider-id {REPO_PROVIDER_ID} "
            f"--repo-node {REPO_PROVIDER_IDENTITY} "
            f"--storage-dir {MININDN_ROOT}/{REPO_NODE}/llama-repo-store "
            "--failure-domain lab-rack --advertise-stored-prefixes",
            node_env[REPO_NODE], processes,
        )
        log(f"Waiting {args.repo_start_wait_s:.1f}s for repo provider")
        time.sleep(args.repo_start_wait_s)

        if args.artifact_source == "local-reference":
            write_local_artifact_references(model_path, runtime_path, ARTIFACT_REFS)
        else:
            deploy_log = OUT / "deploy-artifacts.log"
            deploy_out = deploy_log.open("wb")
            deploy_proc = getPopen(
                ndn.net[CONTROLLER_NODE],
                base + perf.shell_quote(LLAMA_DIR / "deploy_artifacts.py") +
                common +
                " --model {} --llama-server {} --out {} --repo-service {}".format(
                    perf.shell_quote(model_path),
                    perf.shell_quote(runtime_path),
                    perf.shell_quote(ARTIFACT_REFS),
                    perf.shell_quote(REPO_SERVICE),
                ),
                envDict=node_env[CONTROLLER_NODE],
                shell=True,
                stdout=deploy_out,
                stderr=subprocess.STDOUT,
            )
            processes.append((deploy_proc, deploy_out, deploy_log))
            deploy_proc.wait(timeout=180)
            deploy_text = deploy_log.read_text(errors="replace")
            print(deploy_text)
            if deploy_proc.returncode != 0 or "LLAMA_SERVER_ARTIFACT_DEPLOY_OK" not in deploy_text:
                raise RuntimeError(f"llama-server artifact deploy failed; log={deploy_log}")

        extra_arg_flags = "".join(
            " --llama-server-extra-arg=" + perf.shell_quote(arg)
            for arg in args.llama_server_extra_arg
        )
        provider_proc, provider_log = start_process(
            ndn, LLM_PROVIDER_NODE, "llama-provider",
            base + perf.shell_quote(LLAMA_DIR / "provider.py") + common +
            f" --provider-id {LLM_PROVIDER_ID} "
            f"--artifact-references {perf.shell_quote(ARTIFACT_REFS)} "
            f"--artifact-cache-dir {perf.shell_quote(OUT / 'llama-provider-cache')} "
            "--llama-url http://127.0.0.1:18081 "
            f"{extra_arg_flags} "
            "--handler-workers 2",
            node_env[LLM_PROVIDER_NODE], processes,
        )
        if not wait_log(provider_log, "LLAMA_SERVER_MANAGED_STARTED",
                        args.provider_start_timeout_s, provider_proc):
            raise RuntimeError(f"llama provider did not start managed server; log={provider_log}")

        user_log = OUT / "llama-user.log"
        user_csv = OUT / "llama-user-measured.csv"
        user_out = user_log.open("wb")
        measured_flags = (
            " --requests {} --duration-s {} --interval-ms {} --csv {} --quiet-per-request".format(
                max(1, int(args.measured_requests)),
                float(args.measured_duration_s),
                float(args.request_interval_ms),
                perf.shell_quote(user_csv),
            )
        )
        user_proc = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(LLAMA_DIR / "user.py") + common +
            " --prompt {} --max-tokens {} --temperature 0.0 --ack-timeout-ms {} --timeout-ms {}".format(
                perf.shell_quote(args.prompt),
                args.max_tokens,
                args.ack_timeout_ms,
                args.timeout_ms,
            ) + measured_flags,
            envDict=node_env[USER_NODE],
            shell=True,
            stdout=user_out,
            stderr=subprocess.STDOUT,
        )
        processes.append((user_proc, user_out, user_log))
        expected_user_timeout = max(
            180,
            int(args.measured_duration_s + args.timeout_ms / 1000.0 + 120),
            int(max(1, args.measured_requests) * (args.timeout_ms / 1000.0 + 5) + 120),
        )
        user_proc.wait(timeout=expected_user_timeout)
        user_text = user_log.read_text(errors="replace")
        print(user_text)
        provider_text = provider_log.read_text(errors="replace")
        if user_proc.returncode != 0 or "LLAMA_SERVER_USER_RESPONSE" not in user_text:
            raise RuntimeError(f"llama user failed; log={user_log}")
        if not args.real_artifacts and "MiniNDN managed llama-server" not in user_text:
            raise RuntimeError(f"unexpected llama response; log={user_log}")
        if "LLAMA_SERVER_ARTIFACTS_MATERIALIZED" not in provider_text:
            raise RuntimeError(f"provider did not materialize artifacts; log={provider_log}")
        if "LLAMA_SERVER_PROVIDER_RESPONSE" not in provider_text:
            raise RuntimeError(f"provider did not handle LLM inference request; log={provider_log}")
        timing_match = re.search(r"LLAMA_SERVER_USER_TIMING .*elapsed_ms=([0-9.]+)", user_text)
        distributed_ms = float(timing_match.group(1)) if timing_match else -1.0
        distributed_summary = parse_user_csv(user_csv)
        if distributed_ms < 0 and distributed_summary["rows"]:
            distributed_ms = float(distributed_summary["rows"][0]["elapsed_ms"])
        benchmark_summary = {
            "artifact_source": args.artifact_source,
            "prompt": args.prompt,
            "max_tokens": args.max_tokens,
            "ack_timeout_ms": args.ack_timeout_ms,
            "timeout_ms": args.timeout_ms,
            "local": local_summary,
            "distributed": distributed_summary,
            "logs": {
                "user": str(user_log),
                "provider": str(provider_log),
                "csv": str(user_csv),
            },
        }
        summary_path = OUT / "llama-real-benchmark-summary.json"
        summary_path.write_text(
            json.dumps(benchmark_summary, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        if distributed_summary["elapsed"]["count"]:
            print(
                "LLAMA_SERVER_BENCHMARK_SUMMARY",
                f"distributed_count={distributed_summary['elapsed']['count']}",
                f"distributed_p50_ms={distributed_summary['elapsed']['p50_ms']:.2f}",
                f"distributed_p95_ms={distributed_summary['elapsed']['p95_ms']:.2f}",
                f"provider_p50_ms={distributed_summary['provider']['p50_ms']:.2f}",
                f"overhead_p50_ms={distributed_summary['estimated_ndnsf_overhead']['p50_ms']:.2f}",
                f"summary={summary_path}",
            )
        mode_marker = (
            "LLAMA_SERVER_REAL_MININDN_REPO_BACKED_OK"
            if args.real_artifacts and args.artifact_source == "repo" else
            "LLAMA_SERVER_REAL_MININDN_OK"
            if args.real_artifacts else
            "LLAMA_SERVER_MININDN_REPO_BACKED_OK"
        )
        print(
            f"{mode_marker} "
            f"artifact_source={args.artifact_source} "
            f"local_cold_ms={local_cold_ms if local_cold_ms is not None else -1:.2f} "
            f"local_warm_ms={local_warm_ms if local_warm_ms is not None else -1:.2f} "
            f"distributed_ms={distributed_ms:.2f} "
            f"policy={CONFIG} artifacts={ARTIFACT_REFS} user_log={user_log} "
            f"provider_log={provider_log}"
        )
        return 0
    finally:
        stop_processes(processes)
        try:
            ndn.stop()
        finally:
            Minindn.cleanUp()


if __name__ == "__main__":
    raise SystemExit(main())
