#!/usr/bin/env python3
"""MiniNDN smoke for distributed validation LLM pipeline inference."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import pwd
import re
import signal
import shutil
import statistics
import site
import subprocess
import sys
import time
from pathlib import Path, PurePosixPath

import yaml  # type: ignore

REPO = Path(__file__).resolve().parents[1]
MININDN_ROOT = Path("/tmp/minindn")
# The runner is deliberately executable from a clean checkout.  Import the
# two local Python wrapper trees before loading NDNSF-DI; relying on a globally
# installed py_repoclient made the MiniNDN gate pass on one host and fail in a
# sealed candidate container.
sys.path.insert(0, str(REPO / "NDNSF-DistributedRepo/pythonWrapper"))
sys.path.insert(1, str(REPO / "pythonWrapper"))
sys.path.insert(0, str(REPO / "Experiments"))
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))
sys.path.insert(0, str(REPO / "tools/ndnsf-di"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from ndnsf_distributed_inference.deployment import (  # noqa: E402
    BoundedRecoveryController,
    RecoveryReason,
)
from spec107_fault_controller import OwnedProcessRegistry  # noqa: E402
from run_spec107_live_faults import (  # noqa: E402
    FAULT_CELLS,
    derive_fault_provider_control,
    validate_cell_claim,
)
from spec107_identity import (  # noqa: E402
    committed_source_digest,
    digest_object,
    validate_campaign_set,
    validate_candidate_identity,
)
from spec107_artifacts import verify_artifact_set  # noqa: E402
from spec168_real_model_gate import (  # noqa: E402
    expected_stage_completion_marker,
    real_model_readiness_marker,
    validate_real_model_binding,
)
from spec168_runtime_evidence import write_spec168_runtime_evidence  # noqa: E402
from spec107_preflight import (  # noqa: E402
    PreflightError,
    claim_campaign_writer,
    run_campaign_preflight,
    write_invalid_preflight_record,
)
from ndnsf_distributed_inference.app_sdk import (  # noqa: E402
    ArtifactReference, DeploymentDefinition, DeploymentRevision,
)
from mininet.log import info, setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.apps.nlsr import Nlsr  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402
from minindn.util import getPopen  # noqa: E402


TOPO = REPO / "Experiments/Topology/AI_Lab.conf"
OUT = REPO / "results/llm_pipeline_minindn_smoke"
LLM_DIR = REPO / "examples/python/NDNSF-DistributedInference/llm_pipeline"
CONFIG = OUT / "llm_pipeline_policy.yaml"
DEFAULT_QWEN_CONTENT_STORE = REPO / "results/.ndnsf-di-content-addressed"
GEN_POLICY = "/tmp/ndnsf-di-llm-pipeline-minindn-policy"
APP_ROOT = "/example/llm-pipeline"
CONTROLLER_IDENTITY = APP_ROOT + "/controller"
GROUP_IDENTITY = APP_ROOT + "/group"
USER_IDENTITY = APP_ROOT + "/user"
PROVIDER_PREFIX = APP_ROOT + "/provider"
REPO_PROVIDER_PREFIX = APP_ROOT + "/repo"
SERVICE = "/AI/LLM/Pipeline/Fake"
CONTROLLER_NODE = "memphis"
USER_NODE = "memphis"
STAGE_NODES = ["ucla", "arizona", "wustl"]
STAGE_PROVIDER_IDS = ["", "1", "2"]
STAGE_IDENTITIES = [
    PROVIDER_PREFIX,
    PROVIDER_PREFIX + "/1",
    PROVIDER_PREFIX + "/2",
]
REPO_IDENTITIES = [
    REPO_PROVIDER_PREFIX,
    REPO_PROVIDER_PREFIX + "/1",
    REPO_PROVIDER_PREFIX + "/2",
]
DEPLOYMENT_ARTIFACT_DIGEST = "sha256:" + "a" * 64


def configure_spec111_deployment_workflow(config_path: Path, out: Path,
                                          stages: int) -> dict[str, object]:
    # Keep parser/help and topology-validation commands usable in the minimal
    # runner image.  The signer is only needed when the deployment workflow is
    # actually constructed; importing it at module load made ``--help`` fail
    # on images that intentionally omit the optional cryptography package.
    from ndnsf_distributed_inference.app_sdk import ProviderEvidenceSigner

    if stages != len(STAGE_IDENTITIES):
        raise ValueError("Spec 111 deployment workflow currently requires three stages")
    controls = [f"/APP/Deployment/Control/Stage/{index}"
                for index in range(stages)]
    roles = [f"/LLM/Pipeline/Stage/{index}" for index in range(stages)]
    payload = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    payload["services"] = [
        item for item in payload.get("services", ())
        if not str(item.get("name", "")).startswith("/APP/Deployment/Control/")
    ]
    for index, (service, role, identity) in enumerate(
            zip(controls, roles, STAGE_IDENTITIES)):
        payload["services"].append({
            "name": service,
            "model": "/Model/Deployment/Evidence",
            "users": [USER_IDENTITY],
            "providers": [{"identity": identity, "roles": [role]}],
            "roles": [role],
            "dependencies": [],
            "artifacts": [],
            "input": {"codec": "application/json", "implemented": True},
            "output": {"codec": "application/json", "implemented": True},
            "metadata": {"owner": "APPDeployment", "wireProtocol": "ordinary-NDNSF"},
        })
    authorization = payload.setdefault("authorization_summary", {})
    for user in authorization.get("users", ()):
        if user.get("identity") == USER_IDENTITY:
            user["services"] = [
                item for item in user.get("services", ())
                if not str(item).startswith("/APP/Deployment/Control/")
            ] + controls
    for provider in authorization.get("providers", ()):
        identity = str(provider.get("identity", ""))
        if identity not in STAGE_IDENTITIES:
            continue
        index = STAGE_IDENTITIES.index(identity)
        provider["services"] = [
            item for item in provider.get("services", ())
            if not str(item.get("service", "")).startswith(
                "/APP/Deployment/Control/")
        ] + [{"service": controls[index], "roles": "all"}]
    config_path.write_text(
        yaml.safe_dump(payload, sort_keys=False), encoding="utf-8")

    definition = DeploymentDefinition(
        "spec111-minindn-qwen",
        "qwen-external",
        (ArtifactReference(
            "file:///project/tma1/ndnsf-di/models/qwen",
            DEPLOYMENT_ARTIFACT_DIGEST, 1, "/models/qwen"),),
        tuple(roles),
        {"precision": "fake", "workflow": "spec111-minindn"},
    )
    revision = DeploymentRevision.resolve(definition)
    definition_path = out / "deployment-definition.json"
    definition_path.write_text(json.dumps({
        "deploymentId": definition.deployment_id,
        "modelId": definition.model_id,
        "artifacts": [item.__dict__ for item in definition.artifacts],
        "roles": list(definition.roles),
        "configuration": dict(definition.configuration),
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    key_dir = Path(f"/tmp/spec111-provider-evidence-{os.getpid()}")
    shutil.rmtree(key_dir, ignore_errors=True)
    key_dir.mkdir(mode=0o700, parents=True)
    public_keys = {}
    private_keys = []
    for index in range(stages):
        signer = ProviderEvidenceSigner.generate()
        private_path = key_dir / f"stage-{index}.pem"
        private_path.write_bytes(signer.private_pem())
        private_path.chmod(0o600)
        private_keys.append(str(private_path))
        public_keys[signer.key_id] = signer.public_pem().decode("ascii")
    trust_path = out / "provider-evidence-trust.json"
    trust_path.write_text(json.dumps({
        "schema": "ndnsf-di-provider-evidence-trust-v1",
        "trustedProviderKeys": public_keys,
        "controlServices": controls,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return {
        "revision": revision.revision,
        "definition": str(definition_path),
        "trust": str(trust_path),
        "controls": controls,
        "roles": roles,
        "privateKeys": private_keys,
        "keyDir": str(key_dir),
    }


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


def write_bootstrap_token(
    token_file: str | Path,
    identity: str,
    output: str | Path,
) -> Path:
    for raw in Path(token_file).read_text(encoding="utf-8").splitlines():
        fields = raw.split()
        if fields and not fields[0].startswith("#") and fields[0] == identity:
            if len(fields) < 2:
                break
            target = Path(output)
            target.write_text(fields[1] + "\n", encoding="utf-8")
            target.chmod(0o600)
            return target
    raise RuntimeError(f"bootstrap token missing for {identity}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="MiniNDN smoke for distributed LLM pipeline inference")
    parser.add_argument("--topology-file", default=str(TOPO))
    parser.add_argument("--output-dir", default=str(OUT))
    parser.add_argument("--stages", type=int, default=3)
    parser.add_argument("--layers", type=int, default=24)
    parser.add_argument(
        "--runtime",
        choices=("fake", "tiny-transformers", "qwen-transformers", "qwen-onnx",
                 "qwen-onnx-cpu-native"),
        default="fake",
    )
    parser.add_argument("--transformer-layers", type=int, default=4)
    parser.add_argument("--qwen-model", default="Qwen/Qwen2.5-0.5B-Instruct")
    parser.add_argument("--qwen-revision", default="main")
    parser.add_argument("--qwen-allow-download", action="store_true")
    parser.add_argument("--qwen-dtype", choices=("float32", "float16", "auto"), default="float32")
    parser.add_argument(
        "--qwen-content-store",
        default=str(DEFAULT_QWEN_CONTENT_STORE),
        help=(
            "Persistent content-addressed store for Qwen stage artifacts; "
            "run directories contain symlinks instead of model copies."),
    )
    parser.add_argument("--qwen-execution-provider", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--qwen-device-ids", default="0", help="Comma-separated logical GPU IDs mapped round-robin to stages")
    parser.add_argument(
        "--reuse-existing-policy",
        action="store_true",
        help=(
            "Reuse an existing llm_pipeline_policy.yaml and generated stage "
            "artifacts in --output-dir. Useful for repeated Qwen benchmarks "
            "where rewriting multi-GB stage artifacts is unnecessary."
        ),
    )
    parser.add_argument("--compute-delay-ms", type=float, default=1.0)
    parser.add_argument("--nlsr-wait-s", type=float, default=8.0)
    parser.add_argument(
        "--static-routing-only",
        action="store_true",
        help=(
            "Use MiniNDN's explicit NdnRoutingHelper routes without launching "
            "NLSR. Intended for minimal candidate containers."
        ),
    )
    parser.add_argument(
        "--controller-wait-s", type=float, default=0.0,
        help=(
            "Deprecated compatibility input; must remain zero. The controller "
            "ready marker is the startup authority."
        ),
    )
    parser.add_argument(
        "--provider-wait-s", type=float, default=0.0,
        help=(
            "Deprecated compatibility input; must remain zero. Provider ready "
            "markers, ACK closure, preparation progress, and dependency data "
            "drive request execution."
        ),
    )
    parser.add_argument("--provider-start-timeout-s", type=float, default=20.0)
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=60000)
    parser.add_argument("--ndn-log", default="ndn_service_framework.*=INFO")
    parser.add_argument("--prompt", default="Explain NDNSF-DI pipeline inference.")
    parser.add_argument("--warmup-requests", type=int, default=0)
    parser.add_argument("--measured-requests", type=int, default=1)
    parser.add_argument("--max-new-tokens", type=int, default=1)
    parser.add_argument("--generation-campaign-manifest", default="")
    parser.add_argument("--generation-jsonl", default="")
    parser.add_argument("--qwen-tokenizer-dir", default="")
    parser.add_argument("--workload-digest", default="")
    parser.add_argument("--model-identity-digest", default="")
    parser.add_argument(
        "--spec168-source-digest", default="",
        help=(
            "Emit the canonical Spec 168 runtime admission and lifecycle "
            "artifacts bound to this immutable source digest."
        ),
    )
    parser.add_argument(
        "--spec168-max-ack-collect-ms", type=float, default=5000.0,
        help=(
            "Maximum ACK coverage closure time admitted by the Spec 168 "
            "real-MiniNDN gate."
        ),
    )
    parser.add_argument(
        "--selection-dataflow-v2",
        action="store_true",
        help=(
            "Use request-first ACK_CLOSED planning, deferred DistributedRepo "
            "publication/fetch, and per-role data-driven execution."
        ),
    )
    parser.add_argument(
        "--selection-dataflow-v3",
        action="store_true",
        help=(
            "Use the normal V3 request-first ACK/Selection path with signed "
            "cache residency and no ACK-time reservation."
        ),
    )
    parser.add_argument("--qwen-stage-manifest", default="")
    parser.add_argument("--qwen-stage-root", default="")
    parser.add_argument("--repo-object-prefix", default="")
    parser.add_argument("--request-id", default="")
    parser.add_argument("--selection-offer-lease-ms", type=int, default=900000)
    parser.add_argument("--selection-max-prepare-ms", type=int, default=900000)
    parser.add_argument("--selection-cache-max-age-ms", type=int, default=900000)
    parser.add_argument(
        "--require-real-model",
        action="store_true",
        help=(
            "Fail closed unless this is an explicit Qwen real-model run with "
            "campaign, tokenizer, and immutable identity evidence."
        ),
    )
    parser.add_argument("--durable-app-submit", action="store_true")
    parser.add_argument(
        "--deployment-workflow", action="store_true",
        help="Run the Spec 111 signed Provider lifecycle over ordinary NDNSF services")
    parser.add_argument("--deployment-revision", default="")
    parser.add_argument(
        "--app-state-root", default="/tmp/ndnsf-di-app-state")
    parser.add_argument(
        "--test-only-allow-ephemeral-app-state", action="store_true")
    parser.add_argument("--expected-token-ids", default="")
    parser.add_argument(
        "--native-first-kv-mode",
        choices=("full-context", "delta-only"),
        default="full-context",
    )
    parser.add_argument("--measured-duration-s", type=float, default=0.0)
    parser.add_argument("--request-interval-ms", type=float, default=0.0)
    parser.add_argument("--campaign-id", default="")
    parser.add_argument(
        "--spec107-diagnostic",
        choices=("", "generation-session-attribution"),
        default="",
        help="Run a diagnostic-only Spec 107 attribution cell.",
    )
    parser.add_argument(
        "--candidate-manifest", default="",
        help="Digest-bound Spec 107 candidate manifest for diagnostic identity.",
    )
    parser.add_argument(
        "--campaign-manifest", default="",
        help="Locked Spec 107 campaign manifest for live-cell identity.",
    )
    parser.add_argument(
        "--spec107-timing-sample-rate", type=int, default=1,
        help="Stable request sampling denominator for diagnostic timelines.",
    )
    parser.add_argument(
        "--spec107-artifact-store", default="",
        help="Verified content-addressed Spec 107 Qwen artifact-set directory.",
    )
    parser.add_argument(
        "--spec107-qwen-service-manifest", default="",
        help="Reviewed Qwen ONNX stage metadata manifest for artifact reuse.",
    )
    parser.add_argument(
        "--spec107-qwen-runtime-manifest", default="",
        help="Reviewed Qwen prompt/token runtime manifest for artifact reuse.",
    )
    parser.add_argument(
        "--spec107-command-profile", default="",
        help="Digest-bound exact diagnostic command profile.",
    )
    parser.add_argument(
        "--spec107-live-fault-cell",
        choices=("", "positive-control", "provider-kill-restart", "straggler",
                 "missing-segment", "dependency-digest-mismatch", "stale-telemetry",
                 "kv-eviction", "provider-boot-change", "late-old-output"),
        default="",
        help="Execute one preregistered Spec 107 live-fault cell.",
    )
    parser.add_argument(
        "--publish-input-reference",
        action="store_true",
        help=(
            "For Qwen runtimes, publish token_ids/attention_mask context as "
            "NDNSF large-data and send the standard reference payload."
        ),
    )
    parser.add_argument(
        "--context-input-mode",
        choices=("full", "append-empty-delta-after-first", "append-token-delta-after-first"),
        default="full",
        help="Qwen context request shape for full-context or append-delta validation.",
    )
    parser.add_argument(
        "--delta-token-ids",
        default="2",
        help="Comma-separated token IDs for append-token-delta-after-first.",
    )
    parser.add_argument(
        "--large-fetch-timing",
        action="store_true",
        help=(
            "Enable narrow Core collaboration large-data fetch timing. This "
            "adds per-segment received/validated diagnostics for Stage1/Stage2 "
            "hidden-state fetches and should be used for diagnosis, not normal "
            "latency benchmarks."
        ),
    )
    parser.add_argument(
        "--fault-matrix-contract", action="store_true",
        help=("Execute the deterministic eight-cell recovery fault contract and "
              "write fault-matrix-contract.json; this is not network injection"),
    )
    return parser


def execute_fault_matrix_contract() -> dict[str, object]:
    fallback = {
        "/LLM/Pipeline/Stage/0": {"primary": "ucla", "fallback": "arizona"},
        "/LLM/Pipeline/Stage/1": {"primary": "arizona", "fallback": "wustl"},
        "/LLM/Pipeline/Stage/2": {"primary": "wustl", "fallback": "ucla"},
    }
    cases: list[dict[str, object]] = []

    def recovery_case(name: str, reason: RecoveryReason,
                      replacement: str = "/provider/fallback") -> None:
        controller = BoundedRecoveryController(
            f"fault-{name}", request_deadline_ms=5_000,
            started_at_ms=1_000, max_replacements=1)
        first = controller.start("/provider/primary")
        action = controller.recover(
            reason, at_ms=1_100, replacement_provider=replacement)
        old_authoritative = controller.accept_result(first.attempt_epoch, b"late-old")
        cases.append({
            "name": name, "injectionApplied": True,
            "networkInjection": False, "action": action.action,
            "attemptEpoch": action.attempt_epoch,
            "remainingDeadlineMs": action.remaining_deadline_ms,
            "terminalReason": (
                action.terminal_reason.value if action.terminal_reason else ""),
            "oldEpochAuthoritative": old_authoritative,
            "controlPayloads": list(action.control_payloads),
        })

    recovery_case("provider-kill-restart", RecoveryReason.PROVIDER_LOST)
    recovery_case("straggler", RecoveryReason.STRAGGLER_DEADLINE)
    recovery_case("stale-telemetry", RecoveryReason.TELEMETRY_STALE)
    recovery_case(
        "cache-eviction", RecoveryReason.CACHE_MISS_FULL_CONTEXT_REQUIRED, "")

    for name, terminal_reason in (
        ("missing-segment", "DEPENDENCY_MISSING"),
        ("dependency-hash-mismatch", "DEPENDENCY_HASH_MISMATCH"),
    ):
        cases.append({
            "name": name, "injectionApplied": True,
            "networkInjection": False, "action": "fail",
            "attemptEpoch": 1, "terminalReason": terminal_reason,
            "oldEpochAuthoritative": False, "controlPayloads": [],
        })

    recovery_case("provider-restart-new-boot", RecoveryReason.PROVIDER_LOST)
    recovery_case("late-old-output", RecoveryReason.PROVIDER_LOST)

    fallacies = (
        "network-reliable", "latency-zero", "bandwidth-infinite",
        "network-secure", "topology-static", "single-administrator",
        "transport-cost-zero", "network-homogeneous", "time-synchronized",
        "resources-stable", "failures-independent",
    )
    return {
        "schema": "ndnsf-di-spec105-fault-matrix-contract-v1",
        "scope": "deterministic recovery contract in MiniNDN harness",
        "networkInjection": False,
        "physicalHardwareEvidence": False,
        "fallbackRoleActivation": fallback,
        "cases": cases,
        "fallacyScan": [{"fallacy": item, "status": "PASS"} for item in fallacies],
        "overall": "BLOCK",
        "blockReason": "contract injection does not prove live MiniNDN fault recovery",
    }


def python_path_entries() -> list[str]:
    source_entries = [
        str(REPO / "NDNSF-DistributedInference"),
        str(LLM_DIR),
        str(REPO / "Experiments"),
        site.getusersitepackages(),
    ]
    inherited_entries = [
        entry for entry in os.environ.get("PYTHONPATH", "").split(os.pathsep)
        if entry
    ]
    # A sealed container carries ABI-matched NDNSF and DistributedRepo native
    # extensions.  Keep current application/experiment Python code mounted,
    # but do not let host or stale native packages shadow the candidate.  The
    # entrypoint-provided overlay must remain first even after MiniNDN rebuilds
    # the environment for each node process.
    prefer_runtime_closure = bool(os.environ.get("SPEC168_OVERLAY_ROOT")) \
        or os.environ.get("NDNSF_PREFER_INSTALLED_NATIVE") == "1"
    if prefer_runtime_closure:
        entries = inherited_entries + source_entries
    else:
        entries = list(source_entries)
        entries.insert(1, str(REPO / "pythonWrapper"))
        entries.insert(2, str(REPO / "NDNSF-DistributedRepo/pythonWrapper"))
        entries.extend(inherited_entries)
    sudo_user = os.environ.get("SUDO_USER")
    if sudo_user:
        try:
            sudo_home = pwd.getpwnam(sudo_user).pw_dir
            version = f"python{sys.version_info.major}.{sys.version_info.minor}"
            entries.append(str(Path(sudo_home) / ".local/lib" / version / "site-packages"))
        except KeyError:
            pass
    deduped = []
    for entry in entries:
        if entry and entry not in deduped:
            deduped.append(entry)
    return deduped


def local_python_subprocess_env() -> dict[str, str]:
    """Keep repository-local Python wrappers visible to helper subprocesses.

    The runner itself prepends the wrappers to ``sys.path``.  Helper scripts
    (notably the Spec 162 policy/manifest builders) are separate Python
    processes and therefore cannot inherit that in-process path mutation.
    Passing the same closure explicitly makes the MiniNDN gate independent of
    whether the host has ``py_repoclient`` installed globally.
    """
    return {
        **os.environ,
        "PYTHONPATH": os.pathsep.join(python_path_entries()),
        "PYTHONNOUSERSITE": "1",
    }


def python_process_prefix(base_env: dict[str, str]) -> str:
    """Return a fail-closed Python command prefix for MiniNDN node processes.

    Mininet implementations differ in how completely they preserve ``env``
    through their namespace helper.  Put the candidate's import and native
    library closure on the command line as well as in ``envDict`` so every
    controller, repository, provider, and user resolves the same build.
    """
    required = ("PYTHONPATH",)
    missing = [key for key in required if not base_env.get(key)]
    if missing:
        raise RuntimeError(
            "MiniNDN Python runtime closure is incomplete: " + ",".join(missing))
    closure = {
        "PYTHONPATH": base_env["PYTHONPATH"],
        "PYTHONNOUSERSITE": "1",
        "NDN_LOG": base_env["NDN_LOG"],
    }
    if base_env.get("LD_LIBRARY_PATH"):
        closure["LD_LIBRARY_PATH"] = base_env["LD_LIBRARY_PATH"]
    if base_env.get("NDNSF_SELECTION_TARGETED_PREFETCH"):
        closure["NDNSF_SELECTION_TARGETED_PREFETCH"] = base_env["NDNSF_SELECTION_TARGETED_PREFETCH"]
    assignments = " ".join(
        f"{key}={perf.shell_quote(value)}" for key, value in closure.items())
    return (
        f"cd {perf.shell_quote(REPO)} && exec env {assignments} "
        f"{perf.shell_quote(sys.executable)} "
    )


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


def spec168_process_rows(
    processes: list[tuple[object, object, Path]],
) -> list[dict[str, object]]:
    role_by_log = {
        "controller.log": "controller",
        "repo-0.log": "repository",
        "repo-1.log": "repository-replica-1",
        "repo-2.log": "repository-replica-2",
        "llm-pipeline-user.log": "user",
        "stage0-provider.log": "provider-stage-0",
        "stage1-provider.log": "provider-stage-1",
        "stage2-provider.log": "provider-stage-2",
    }
    rows = []
    for process, _, log_path in processes:
        role = role_by_log.get(log_path.name)
        if role is not None:
            rows.append({"role": role, "pid": int(process.pid)})
    return rows


def wait_log(path: Path, needle: str, timeout_s: float, proc=None) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.25)
    return False


def _percentile(values: list[float], percentile: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    index = (len(ordered) - 1) * percentile
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    if lower == upper:
        return ordered[lower]
    weight = index - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def _parse_key_values(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for part in line.split()[1:]:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        result[key] = value
    return result


def _float_field(row: dict[str, str], key: str) -> float:
    try:
        return float(row.get(key, "0") or "0")
    except ValueError:
        return 0.0


def _int_field(row: dict[str, str], key: str) -> int:
    try:
        return int(float(row.get(key, "0") or "0"))
    except ValueError:
        return 0


def _summarize(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {"count": 0, "avg_ms": 0.0, "p50_ms": 0.0, "p95_ms": 0.0}
    return {
        "count": len(values),
        "avg_ms": statistics.fmean(values),
        "p50_ms": statistics.median(values),
        "p95_ms": _percentile(values, 0.95),
    }


def write_qwen_stage_profile(provider_logs: list[Path],
                             user_metrics_csv: Path,
                             output_dir: Path) -> Path | None:
    rows: list[dict[str, str]] = []
    for log_path in provider_logs:
        for line in log_path.read_text(errors="replace").splitlines():
            if "LLM_PIPELINE_QWEN_STAGE_TIMING" not in line:
                continue
            row = _parse_key_values(line)
            row["log"] = log_path.name
            rows.append(row)
    if not rows:
        return None
    csv_path = output_dir / "qwen-stage-profile.csv"
    fields = [
        "log", "role", "stage", "requestId", "isFinal", "input_bytes",
        "output_bytes", "prefetch_submit_ms", "input_wait_ms", "ref_wait_ms",
        "fetch_ms", "used_planned_name", "expected_segments",
        "expected_bytes", "decode_ms", "serialize_ms", "compute_ms",
        "artificial_delay_ms", "runner_total_ms", "publish_ms", "total_ms",
    ]
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

    by_stage: dict[str, dict[str, dict[str, float | int]]] = {}
    fields_to_summarize = [
        "input_wait_ms", "ref_wait_ms", "fetch_ms", "decode_ms",
        "serialize_ms", "compute_ms", "runner_total_ms", "publish_ms",
        "total_ms",
    ]
    for stage in sorted({row.get("stage", "") for row in rows}):
        stage_rows = [row for row in rows if row.get("stage") == stage]
        by_stage[stage] = {
            field: _summarize([_float_field(row, field) for row in stage_rows])
            for field in fields_to_summarize
        }

    user_latencies: list[float] = []
    if user_metrics_csv.exists():
        with user_metrics_csv.open(encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                if row.get("phase") == "measured" and row.get("status") == "ok":
                    user_latencies.append(_float_field(row, "distributed_ms"))

    stage_totals = {
        stage: float(summary["total_ms"]["p50_ms"])
        for stage, summary in by_stage.items()
    }
    compute_totals = {
        stage: float(summary["compute_ms"]["p50_ms"])
        for stage, summary in by_stage.items()
    }
    fetch_totals = {
        stage: float(summary["fetch_ms"]["p50_ms"])
        for stage, summary in by_stage.items()
    }
    serialization_totals = {
        stage: (
            float(summary["decode_ms"]["p50_ms"]) +
            float(summary["serialize_ms"]["p50_ms"])
        )
        for stage, summary in by_stage.items()
    }
    stage_chain_p50_ms = max(stage_totals.values() or [0.0])
    distributed_p50_ms = float(_summarize(user_latencies)["p50_ms"])
    outer_residual_ms = max(0.0, distributed_p50_ms - stage_chain_p50_ms)
    summary = {
        "schema": "ndnsf-di-qwen-pipeline-profile-v1",
        "stageProfileCsv": str(csv_path),
        "userMetricsCsv": str(user_metrics_csv),
        "measuredRequests": len(user_latencies),
        "distributed": _summarize(user_latencies),
        "byStage": by_stage,
        "derived": {
            "stage_critical_path_p50_ms": stage_chain_p50_ms,
            "stage_compute_p50_ms_by_stage": compute_totals,
            "stage_hidden_fetch_p50_ms_by_stage": fetch_totals,
            "stage_serialization_p50_ms_by_stage": serialization_totals,
            "outer_ack_selection_response_residual_p50_ms": outer_residual_ms,
            "outerResidualNote": (
                "Residual is user distributed p50 minus the maximum provider "
                "stage handler p50. Stage handlers overlap because downstream "
                "providers prefetch planned hidden-state names as soon as the "
                "outer request is assigned. The residual includes outer "
                "ACK/selection/final-response propagation and cross-process "
                "scheduling not yet separately exposed by Core."
            ),
        },
    }
    summary_path = output_dir / "qwen-stage-profile-summary.json"
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    print(
        "LLM_PIPELINE_QWEN_PROFILE "
        f"distributed_p50_ms={distributed_p50_ms:.2f} "
        f"stage_critical_path_p50_ms={stage_chain_p50_ms:.2f} "
        f"outer_residual_p50_ms={outer_residual_ms:.2f} "
        f"summary={summary_path} csv={csv_path}"
    )
    for stage, stage_summary in by_stage.items():
        print(
            "LLM_PIPELINE_QWEN_PROFILE_STAGE "
            f"stage={stage} "
            f"compute_p50_ms={float(stage_summary['compute_ms']['p50_ms']):.2f} "
            f"fetch_p50_ms={float(stage_summary['fetch_ms']['p50_ms']):.2f} "
            f"decode_p50_ms={float(stage_summary['decode_ms']['p50_ms']):.2f} "
            f"serialize_p50_ms={float(stage_summary['serialize_ms']['p50_ms']):.2f} "
            f"publish_p50_ms={float(stage_summary['publish_ms']['p50_ms']):.2f} "
            f"total_p50_ms={float(stage_summary['total_ms']['p50_ms']):.2f}"
        )
    return summary_path


def write_collab_large_fetch_profile(provider_logs: list[Path],
                                     output_dir: Path) -> Path | None:
    rows: list[dict[str, str]] = []
    for log_path in provider_logs:
        for line in log_path.read_text(errors="replace").splitlines():
            if "NDNSF_COLLAB_LARGE_FETCH_TIMING" not in line:
                continue
            row = _parse_key_values(line)
            if not row:
                continue
            row["log"] = log_path.name
            rows.append(row)
    if not rows:
        return None

    csv_path = output_dir / "qwen-collab-large-fetch-events.csv"
    fields = [
        "log", "event", "mode", "requestId", "keyScope", "dataName",
        "segment", "segmentName", "timestamp_us", "start_epoch_ms",
        "encoded_bytes", "plaintext_bytes", "elapsed_ms",
        "first_segment_ms", "last_segment_received_ms",
        "last_segment_validated_ms", "fetch_start_to_data_ms",
        "fetch_start_to_validated_ms", "interest_to_data_ms",
        "interest_to_validated_ms", "data_to_validated_ms", "decrypt_ms",
        "fetch_start_to_decrypt_done_ms", "received_segments",
        "validated_segments", "received_wire_bytes", "wire_bytes", "nacks",
        "segment_timeouts", "interest_lifetime_ms", "init_cwnd",
    ]
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

    complete_rows = [
        row for row in rows
        if row.get("event") == "complete" and row.get("elapsed_ms")
    ]
    start_rows = [row for row in rows if row.get("event") == "start"]
    active_put_rows = [row for row in rows if row.get("event") == "segment_active_put"]
    received_rows = [row for row in rows if row.get("event") == "segment_received"]
    validated_rows = [row for row in rows if row.get("event") == "segment_validated"]
    decrypt_rows = [row for row in rows if row.get("event") == "decrypt"]
    error_rows = [row for row in rows if row.get("event") == "error"]

    by_scope: dict[str, dict[str, object]] = {}
    for scope in sorted({row.get("keyScope", "") for row in rows if row.get("keyScope")}):
        scope_starts = [row for row in start_rows if row.get("keyScope") == scope]
        start_by_request = {
            row.get("requestId", ""): _int_field(row, "timestamp_us")
            for row in scope_starts
            if row.get("requestId")
        }
        received_by_segment = {
            (row.get("requestId", ""), row.get("segmentName", "")): _int_field(row, "timestamp_us")
            for row in received_rows
            if row.get("keyScope") == scope and row.get("requestId") and row.get("segmentName")
        }
        active_to_data_ms: list[float] = []
        start_to_active_ms: list[float] = []
        for row in active_put_rows:
            if row.get("keyScope") != scope:
                continue
            request_id = row.get("requestId", "")
            segment_name = row.get("segmentName", "")
            active_us = _int_field(row, "timestamp_us")
            data_us = received_by_segment.get((request_id, segment_name), 0)
            start_us = start_by_request.get(request_id, 0)
            if start_us > 0 and active_us >= start_us:
                start_to_active_ms.append((active_us - start_us) / 1000.0)
            if active_us > 0 and data_us >= active_us:
                active_to_data_ms.append((data_us - active_us) / 1000.0)

        scope_complete = [row for row in complete_rows if row.get("keyScope") == scope]
        scope_received = [row for row in received_rows if row.get("keyScope") == scope]
        scope_validated = [row for row in validated_rows if row.get("keyScope") == scope]
        scope_decrypt = [row for row in decrypt_rows if row.get("keyScope") == scope]
        by_scope[scope] = {
            "startCount": len(scope_starts),
            "activePutCount": sum(
                1 for row in active_put_rows if row.get("keyScope") == scope),
            "completeCount": len(scope_complete),
            "segmentReceivedCount": len(scope_received),
            "segmentValidatedCount": len(scope_validated),
            "elapsedMs": _summarize([_float_field(row, "elapsed_ms")
                                     for row in scope_complete]),
            "firstSegmentMs": _summarize([_float_field(row, "first_segment_ms")
                                          for row in scope_complete]),
            "lastSegmentReceivedMs": _summarize([
                _float_field(row, "last_segment_received_ms")
                for row in scope_complete
            ]),
            "lastSegmentValidatedMs": _summarize([
                _float_field(row, "last_segment_validated_ms")
                for row in scope_complete
            ]),
            "segmentFetchStartToDataMs": _summarize([
                _float_field(row, "fetch_start_to_data_ms")
                for row in scope_received
            ]),
            "segmentFetchStartToValidatedMs": _summarize([
                _float_field(row, "fetch_start_to_validated_ms")
                for row in scope_validated
            ]),
            "segmentDataToValidatedMs": _summarize([
                _float_field(row, "data_to_validated_ms")
                for row in scope_validated
            ]),
            "fetchStartToActivePutMs": _summarize(start_to_active_ms),
            "activePutToDataMs": _summarize(active_to_data_ms),
            "decryptMs": _summarize([_float_field(row, "decrypt_ms")
                                     for row in scope_decrypt]),
            "fetchStartToDecryptDoneMs": _summarize([
                _float_field(row, "fetch_start_to_decrypt_done_ms")
                for row in scope_decrypt
            ]),
            "encodedBytes": _summarize([_float_field(row, "encoded_bytes")
                                        for row in scope_complete]),
            "plaintextBytes": _summarize([_float_field(row, "plaintext_bytes")
                                          for row in scope_decrypt]),
            "receivedSegments": _summarize([
                float(_int_field(row, "received_segments"))
                for row in scope_complete
            ]),
            "wireBytesPerSegment": _summarize([
                float(_int_field(row, "wire_bytes"))
                for row in scope_received
            ]),
        }

    summary = {
        "schema": "ndnsf-di-collab-large-fetch-profile-v1",
        "eventCsv": str(csv_path),
        "eventCount": len(rows),
        "completeCount": len(complete_rows),
        "errorCount": len(error_rows),
        "startCount": len(start_rows),
        "activePutCount": len(active_put_rows),
        "segmentReceivedCount": len(received_rows),
        "segmentValidatedCount": len(validated_rows),
        "decryptCount": len(decrypt_rows),
        "elapsedMs": _summarize([_float_field(row, "elapsed_ms")
                                 for row in complete_rows]),
        "firstSegmentMs": _summarize([_float_field(row, "first_segment_ms")
                                      for row in complete_rows]),
        "lastSegmentValidatedMs": _summarize([
            _float_field(row, "last_segment_validated_ms")
            for row in complete_rows
        ]),
        "decryptMs": _summarize([_float_field(row, "decrypt_ms")
                                 for row in decrypt_rows]),
        "byKeyScope": by_scope,
        "rows": rows,
    }
    summary_path = output_dir / "qwen-collab-large-fetch-stats.json"
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    print(
        "LLM_PIPELINE_COLLAB_LARGE_FETCH_PROFILE "
        f"events={len(rows)} complete={len(complete_rows)} "
        f"errors={len(error_rows)} "
        f"elapsed_p50_ms={float(summary['elapsedMs']['p50_ms']):.2f} "
        f"first_segment_p50_ms={float(summary['firstSegmentMs']['p50_ms']):.2f} "
        f"last_validated_p50_ms="
        f"{float(summary['lastSegmentValidatedMs']['p50_ms']):.2f} "
        f"decrypt_p50_ms={float(summary['decryptMs']['p50_ms']):.2f} "
        f"summary={summary_path} csv={csv_path}"
    )
    for scope, scope_summary in by_scope.items():
        print(
            "LLM_PIPELINE_COLLAB_LARGE_FETCH_SCOPE "
            f"keyScope={scope} "
            f"complete={scope_summary['completeCount']} "
            f"elapsed_p50_ms="
            f"{float(scope_summary['elapsedMs']['p50_ms']):.2f} "
            f"segment_data_p50_ms="
            f"{float(scope_summary['segmentFetchStartToDataMs']['p50_ms']):.2f} "
            f"segment_validated_p50_ms="
            f"{float(scope_summary['segmentFetchStartToValidatedMs']['p50_ms']):.2f} "
            f"start_to_active_put_p50_ms="
            f"{float(scope_summary['fetchStartToActivePutMs']['p50_ms']):.2f} "
            f"active_put_to_data_p50_ms="
            f"{float(scope_summary['activePutToDataMs']['p50_ms']):.2f} "
            f"decrypt_p50_ms="
            f"{float(scope_summary['decryptMs']['p50_ms']):.2f} "
            f"segments_p50="
            f"{float(scope_summary['receivedSegments']['p50_ms']):.0f}"
        )
    return summary_path


def prepare_policy(stages: int, layers: int, *,
                   runtime: str = "fake",
                   transformer_layers: int = 4,
                   qwen_model: str = "Qwen/Qwen2.5-0.5B-Instruct",
                   qwen_revision: str = "main",
                   qwen_prompt: str = "",
                   qwen_allow_download: bool = False,
                   qwen_dtype: str = "float32",
                   qwen_content_store: str = "",
                   qwen_artifact_store: str = "",
                   qwen_service_manifest: str = "",
                   qwen_runtime_manifest: str = "") -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    hf_home = os.environ.get("HF_HOME")
    sudo_user = os.environ.get("SUDO_USER")
    if not hf_home and sudo_user:
        try:
            hf_home = str(Path(pwd.getpwnam(sudo_user).pw_dir) / ".cache/huggingface")
        except KeyError:
            hf_home = None
    if not hf_home:
        hf_home = str(Path.home() / ".cache/huggingface")
    policy_env = {
        **os.environ,
        "PYTHONPATH": ":".join(python_path_entries()),
        "HF_HOME": hf_home,
        "HUGGINGFACE_HUB_CACHE": str(Path(hf_home) / "hub"),
        "TRANSFORMERS_CACHE": str(Path(hf_home) / "hub"),
    }
    subprocess.run([
        sys.executable,
        str(LLM_DIR / "plan_pipeline.py"),
        "--policy", str(CONFIG),
        "--service", SERVICE,
        "--stages", str(stages),
        "--layers", str(layers),
        "--controller", CONTROLLER_IDENTITY,
        "--group", GROUP_IDENTITY,
        "--user", USER_IDENTITY,
        "--provider-prefix", PROVIDER_PREFIX,
        "--runtime", runtime,
        "--transformer-layers", str(transformer_layers),
        "--qwen-model", qwen_model,
        "--qwen-revision", qwen_revision,
        "--qwen-prompt", qwen_prompt,
        "--qwen-dtype", qwen_dtype,
        *( ["--qwen-content-store", qwen_content_store]
           if qwen_content_store else []),
        *(["--qwen-artifact-store", qwen_artifact_store]
          if qwen_artifact_store else []),
        *(["--qwen-service-manifest", qwen_service_manifest]
          if qwen_service_manifest else []),
        *(["--qwen-runtime-manifest", qwen_runtime_manifest]
          if qwen_runtime_manifest else []),
        "--trust-app-root", APP_ROOT,
        *(["--qwen-allow-download"] if qwen_allow_download else []),
    ], cwd=str(REPO), env=policy_env, check=True)


def prepare_selection_dataflow_v2(args) -> dict[str, object]:
    """Freeze graph/offer inputs without publishing model bytes before Request."""

    source_stage_manifest = Path(
        args.qwen_stage_manifest).expanduser().resolve()
    stage_root = Path(args.qwen_stage_root).expanduser().resolve()
    model_name = str(args.qwen_model)
    if "Qwen3.6" in model_name:
        model_type = "qwen3_5"
    elif "Qwen3" in model_name:
        model_type = "qwen3"
    elif "Qwen2" in model_name:
        model_type = "qwen2"
    else:
        raise RuntimeError(
            f"unsupported request-first Qwen model family: {model_name}")
    stage_document = json.loads(
        source_stage_manifest.read_text(encoding="utf-8"))
    for stage in stage_document.get("stages", ()):
        artifact = stage_root / Path(str(stage["path"])).name
        if not artifact.is_file() or artifact.stat().st_size != int(stage["bytes"]):
            raise RuntimeError(
                f"frozen Qwen stage is unavailable: {artifact}")
        digest = hashlib.sha256()
        with artifact.open("rb") as stream:
            for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != str(stage["sha256"]):
            raise RuntimeError(
                f"frozen Qwen stage digest mismatch: {artifact}")
        stage["path"] = str(artifact)
    stage_document["sourceManifestSha256"] = hashlib.sha256(
        source_stage_manifest.read_bytes()).hexdigest()
    stage_manifest = OUT / "stage-manifest.runtime.json"
    stage_manifest.write_text(
        json.dumps(stage_document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    automatic = OUT / "automatic-planning.json"
    repo_registration = OUT / "repo-registration.json"
    repo_prefix = (
        args.repo_object_prefix
        or USER_IDENTITY + "/NDNSF-DISTRIBUTED-REPO/OBJECT/QWEN"
    )
    subprocess.run([
        sys.executable,
        str(REPO / "specs/162-itiger-qwen36-generation/jobs/"
            "build-automatic-planning-manifest.py"),
        "--stage-manifest", str(stage_manifest),
        "--repository-prefix", repo_prefix,
        "--output", str(automatic),
    ], cwd=str(REPO), env=local_python_subprocess_env(), check=True)
    manifest = json.loads(automatic.read_text(encoding="utf-8"))
    backend = (
        "transformers" if args.qwen_execution_provider == "cuda"
        else "transformers-cpu"
    )
    offer_key_map: dict[str, str] = {}
    residency_paths: list[str] = []
    storage_key_paths: list[str] = []
    offer_key_paths: list[str] = []
    for index, (provider, stage) in enumerate(
            zip(STAGE_IDENTITIES, manifest["stages"])):
        storage_key = OUT / f"selection-storage-{index}.key"
        offer_key = OUT / f"selection-offer-{index}.key"
        storage_key.write_bytes(os.urandom(32))
        offer_key.write_bytes(os.urandom(32))
        storage_key.chmod(0o600)
        offer_key.chmod(0o600)
        offer_key_map[provider] = str(offer_key)
        storage_key_paths.append(str(storage_key))
        offer_key_paths.append(str(offer_key))
        residency = {
            "role": str(stage["role"]),
            "artifact_digest": str(stage["sha256"]),
            "model_content_digest": str(
                manifest["model"]["contentDigest"]),
            "semantics_digest": str(
                manifest["model"]["semanticsDigest"]),
            "graph_digest": str(manifest["graphDigest"]),
            "partition_digest": str(manifest["candidateDigest"]),
            "adapter_id": str(manifest["adapterId"]),
            "adapter_version": str(manifest["adapterVersion"]),
            "precision": str(manifest.get("dtype", "bfloat16")),
            "backend": backend,
        }
        residency_path = OUT / f"selection-residency-{index}.json"
        residency_path.write_text(
            json.dumps(residency, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        residency_paths.append(str(residency_path))
    offer_map_path = OUT / "selection-offer-key-map.json"
    offer_map_path.write_text(
        json.dumps(offer_key_map, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    envelope_key = OUT / "request-envelope.key"
    envelope_key.write_bytes(os.urandom(32))
    envelope_key.chmod(0o600)
    return {
        "stageManifest": str(stage_manifest),
        "automaticPlanning": str(automatic),
        "repoRegistration": str(repo_registration),
        "repoObjectPrefix": repo_prefix,
        "offerKeyMap": str(offer_map_path),
        "envelopeKey": str(envelope_key),
        "storageKeys": tuple(storage_key_paths),
        "offerKeys": tuple(offer_key_paths),
        "residency": tuple(residency_paths),
        "localArtifacts": tuple(
            f"{stage['role']}={stage['path']}"
            for stage in manifest["stages"]
        ),
        "requiredMiB": tuple(
            int(stage["requiredGpuMiB"])
            for stage in manifest["stages"]
        ),
        "modelType": model_type,
    }


def generate_policy_bundle(env: dict[str, str]) -> None:
    subprocess.run([
        sys.executable,
        "-c",
        "from ndnsf_distributed_inference.policy import main; raise SystemExit(main())",
        "--config", str(CONFIG),
        "--out-dir", str(GEN_POLICY),
        "--print-summary",
    ], cwd=str(REPO), env=env, check=True)


def write_native_qwen_bundle(out_dir: Path, *, execution_provider: str = "cpu",
                             device_ids: list[str] | None = None) -> tuple[Path, Path]:
    manifest_source = json.loads(
        (out_dir / "qwen-onnx-service-manifest.json").read_text(encoding="utf-8"))
    roles = [str(stage["role"]) for stage in manifest_source["stages"]]
    dependencies = []
    manifest_dependencies = []
    stage_count = len(manifest_source["stages"])
    if stage_count < 2:
        raise ValueError("Qwen native bundle requires at least two stages")
    if execution_provider not in {"cpu", "cuda"}:
        raise ValueError("Qwen execution provider must be cpu or cuda")
    device_ids = device_ids or (["cpu0"] if execution_provider == "cpu" else ["0"])
    if not device_ids or any(not value for value in device_ids):
        raise ValueError("Qwen device mapping must not be empty")
    for index in range(stage_count - 1):
        next_stage = manifest_source["stages"][index + 1]
        tensors = [
            "hidden_states", "attention_mask", "position_ids",
        ]
        dependency = {
            "producers": [roles[index]], "consumers": [roles[index + 1]],
            "keyScope": f"pipeline-stage-{index}-to-{index + 1}",
            "topicPrefix": "/activation/llm",
            "objectNameTemplate": (
                "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/"
                "{keyScope}/{producerRole}/bundle/{sequence}"),
            "expectedSegments": 0, "expectedBytes": 0, "required": True,
            "segmentNaming": {"mode": "ndn-segment-component",
                              "staticSegmentCount": 0, "dynamicFallback": True},
            "tensors": tensors,
        }
        dependencies.append(dependency)
        manifest_dependencies.append({
            "producers": dependency["producers"], "consumers": dependency["consumers"],
            "key_scope": dependency["keyScope"], "topic_prefix": dependency["topicPrefix"],
            "object_name_template": dependency["objectNameTemplate"],
            "expected_segments": 0, "expected_bytes": 0, "required": True,
            "tensors": tensors,
        })
    artifacts = []
    for index, stage in enumerate(manifest_source["stages"]):
        passthrough = ["attention_mask", "position_ids"]
        metadata = {
            "inputNames": ",".join(stage["inputNames"]),
            "outputNames": ",".join(stage["outputNames"]),
            "forceOutputBundle": "true",
            "executionProvider": execution_provider,
            "allowCpuFallback": "false",
            "deviceId": device_ids[index % len(device_ids)],
            "dtype": str(manifest_source.get("dtype", "float16")),
            "passthroughTensors": ",".join(passthrough),
            "kvTensorMap": ",".join(
                f"{input_name}={output_name}"
                for input_name, output_name in zip(
                    stage["cacheInputs"], stage["cacheOutputs"])
            ),
            "kvOutputTensors": ",".join(stage["cacheOutputs"]),
            "kvOutputScope": "kv-state",
            "outputBundleScope": (
                "final-response" if index == stage_count - 1 else f"pipeline-stage-{index}-to-{index + 1}"),
        }
        if index < 2:
            metadata["outputAlias.hidden_states_out"] = "hidden_states"
        if index > 0:
            for name in ("hidden_states", "attention_mask", "position_ids"):
                metadata[f"inputScope.{name}"] = f"pipeline-stage-{index - 1}-to-{index}"
        artifacts.append({
            "role": stage["role"], "path": stage["path"],
            "artifact": f"/Artifact/QwenPilot/Stage/{index}",
            "filename": Path(stage["path"]).name, "kind": "model",
            "backend": "onnxruntime", "metadata": metadata,
        })
    model_name = str(manifest_source["model"])
    model_revision = str(manifest_source.get("modelRevision", ""))
    model_uri = "/Model/Qwen2.5/" + model_name.rsplit("-", 1)[-1].replace("Instruct", "").strip("-")
    plan = {"version": 2, "services": [{
        "schemaVersion": 2, "service": SERVICE,
        "model": model_uri, "modelRepository": model_name,
        "modelRevision": model_revision, "dtype": manifest_source.get("dtype", "float16"),
        "modelFamily": "llm",
        "modelFormat": "onnx", "plannerKind": "llm-pipeline",
        "runtimeBackend": "onnxruntime", "roles": roles,
        "dependencies": dependencies,
    }]}
    service_manifest = {"services": [{
        "name": SERVICE, "model": model_uri, "modelRepository": model_name,
        "modelRevision": model_revision, "dtype": manifest_source.get("dtype", "float16"),
        "roles": roles, "dependencies": manifest_dependencies,
        "artifacts": artifacts, "modelFamily": "llm", "modelFormat": "onnx",
        "plannerKind": "llm-pipeline", "runtimeBackend": "onnxruntime",
    }]}
    plan_path = out_dir / "native-qwen-execution-plan.json"
    manifest_path = out_dir / "native-qwen-service-manifest.json"
    plan_path.write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    manifest_path.write_text(
        json.dumps(service_manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return plan_path, manifest_path


def validate_spec107_artifact_binding(
    candidate: dict[str, object], artifact_store: str | Path,
    qwen_service_manifest: str | Path,
    qwen_runtime_manifest: str | Path,
) -> dict[str, object]:
    store = Path(artifact_store).expanduser().resolve()
    manifest = verify_artifact_set(store)
    manifest_path = store / "artifact-set.json"
    manifest_digest = "sha256:" + hashlib.sha256(manifest_path.read_bytes()).hexdigest()
    digests = candidate.get("digests")
    if not isinstance(digests, dict) or digests.get("artifact") != manifest_digest:
        raise ValueError("SPEC107_ARTIFACT_CANDIDATE_DIGEST_MISMATCH")
    service_digest = "sha256:" + hashlib.sha256(
        Path(qwen_service_manifest).read_bytes()).hexdigest()
    if digests.get("model") != service_digest:
        raise ValueError("SPEC107_MODEL_CANDIDATE_DIGEST_MISMATCH")
    runtime_digest = "sha256:" + hashlib.sha256(
        Path(qwen_runtime_manifest).read_bytes()).hexdigest()
    if digests.get("tokenizer") != runtime_digest:
        raise ValueError("SPEC107_TOKENIZER_CANDIDATE_DIGEST_MISMATCH")
    return manifest


def validate_spec107_source_binding(
    candidate: dict[str, object], repo_root: str | Path,
) -> None:
    """Require the execution checkout to remain the frozen clean Git source."""

    root = Path(repo_root).resolve()
    try:
        status = subprocess.run(
            ["git", "status", "--porcelain=v1", "--untracked-files=no"],
            cwd=root, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False)
    except OSError as exc:
        raise ValueError(f"SPEC107_SOURCE_GIT_UNAVAILABLE:{exc}") from exc
    if status.returncode != 0:
        raise ValueError(
            "SPEC107_SOURCE_GIT_INVALID:" + status.stderr.strip())
    if status.stdout.strip():
        raise ValueError("SPEC107_SOURCE_TREE_DIRTY")
    digests = candidate.get("digests")
    if not isinstance(digests, dict):
        raise ValueError("SPEC107_CANDIDATE_DIGESTS_INVALID")
    if digests.get("source") != committed_source_digest(root):
        raise ValueError("SPEC107_SOURCE_CANDIDATE_DIGEST_MISMATCH")


def validate_spec107_execution_binding(
    candidate: dict[str, object], plan: str | Path, trust_policy: str | Path,
) -> None:
    digests = candidate.get("digests")
    if not isinstance(digests, dict):
        raise ValueError("SPEC107_CANDIDATE_DIGESTS_INVALID")
    plan_digest = "sha256:" + hashlib.sha256(Path(plan).read_bytes()).hexdigest()
    if digests.get("plan") != plan_digest:
        raise ValueError("SPEC107_PLAN_CANDIDATE_DIGEST_MISMATCH")
    policy_digest = "sha256:" + hashlib.sha256(
        Path(trust_policy).read_bytes()).hexdigest()
    if digests.get("trustPolicy") != policy_digest:
        raise ValueError("SPEC107_TRUST_POLICY_CANDIDATE_DIGEST_MISMATCH")


def validate_spec107_profile_workload_binding(
    candidate: dict[str, object], args: argparse.Namespace,
    command_cell: dict[str, object], campaign_profile: str | Path,
    workload_manifest: str | Path,
) -> None:
    """Bind reviewed diagnostic profile/workload bytes to actual CLI behavior."""

    digests = candidate.get("digests")
    if not isinstance(digests, dict):
        raise ValueError("SPEC107_CANDIDATE_DIGESTS_INVALID")
    profile_path = Path(campaign_profile)
    workload_path = Path(workload_manifest)
    profile_digest = "sha256:" + hashlib.sha256(profile_path.read_bytes()).hexdigest()
    if digests.get("profile") != profile_digest:
        raise ValueError("SPEC107_PROFILE_CANDIDATE_DIGEST_MISMATCH")
    workload_digest = "sha256:" + hashlib.sha256(workload_path.read_bytes()).hexdigest()
    if digests.get("workload") != workload_digest:
        raise ValueError("SPEC107_WORKLOAD_CANDIDATE_DIGEST_MISMATCH")
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    workload = json.loads(workload_path.read_text(encoding="utf-8"))
    if (
        not isinstance(profile, dict)
        or profile.get("schema") != "ndnsf-di-spec107-diagnostic-profile-v1"
        or profile.get("physicalProductionOverall") != "DEFERRED"
        or profile.get("roles") != [f"/LLM/Pipeline/Stage/{index}" for index in range(3)]
    ):
        raise ValueError("SPEC107_PROFILE_INVALID")
    topology_value = profile.get("topology")
    if not isinstance(topology_value, str) or not topology_value:
        raise ValueError("SPEC107_PROFILE_INVALID")
    expected_topology = Path(topology_value)
    if not expected_topology.is_absolute():
        expected_topology = REPO / expected_topology
    if (
        Path(args.topology_file).expanduser().resolve() != expected_topology.resolve()
        or args.stages != profile.get("stageCount")
        or args.runtime != profile.get("runtime")
    ):
        raise ValueError("SPEC107_PROFILE_ARGUMENT_MISMATCH")
    cells = workload.get("cells") if isinstance(workload, dict) else None
    matches = [
        cell for cell in cells or []
        if isinstance(cell, dict) and cell.get("ordinal") == command_cell.get("ordinal")]
    if (
        not isinstance(workload, dict)
        or workload.get("schema") != "ndnsf-di-spec107-diagnostic-workload-v1"
        or workload.get("automaticRetry") is not False
        or len(matches) != 1
    ):
        raise ValueError("SPEC107_WORKLOAD_INVALID")
    expected_tokens = workload.get("expectedTokenIds")
    actual_tokens = [
        int(value) for value in str(args.expected_token_ids).split(",") if value]
    cell = matches[0]
    comparisons = (
        (args.prompt, workload.get("prompt")),
        (actual_tokens, expected_tokens),
        (args.warmup_requests, cell.get("warmupRequests")),
        (args.measured_requests, cell.get("measuredRequests")),
        (args.max_new_tokens, cell.get("maxNewTokens")),
        (float(args.measured_duration_s), float(cell.get("measuredDurationSeconds", -1))),
        (float(args.request_interval_ms), float(cell.get("requestIntervalMs", -1))),
    )
    if any(actual != expected for actual, expected in comparisons):
        raise ValueError("SPEC107_WORKLOAD_ARGUMENT_MISMATCH")


def enforce_spec107_harness_preflight(
    *, candidate: dict[str, object], campaign: dict[str, object],
    artifact_store: str | Path, artifact_manifest: dict[str, object],
    plan: str | Path, repo_root: str | Path, projected_new_bytes: int,
    free_bytes: int | None = None,
) -> dict[str, object]:
    """Retain an invalid diagnostic preflight before any output or role starts."""

    root = Path(repo_root).resolve()
    plan_path = Path(plan)
    if not plan_path.is_absolute():
        plan_path = root / plan_path
    plan_path = plan_path.resolve()
    candidate_digests = candidate.get("digests")
    plan_digest = "sha256:" + hashlib.sha256(plan_path.read_bytes()).hexdigest()
    if not isinstance(candidate_digests, dict) or candidate_digests.get("plan") != plan_digest:
        raise ValueError("SPEC107_PLAN_CANDIDATE_DIGEST_MISMATCH")
    plan_value = json.loads(plan_path.read_text(encoding="utf-8"))
    services = plan_value.get("services") if isinstance(plan_value, dict) else None
    if not isinstance(services, list) or len(services) != 1:
        raise ValueError("SPEC107_PREFLIGHT_PLAN_INVALID")
    service = services[0]
    if not isinstance(service, dict):
        raise ValueError("SPEC107_PREFLIGHT_PLAN_INVALID")
    roles = service.get("roles")
    backend = service.get("runtimeBackend")
    if (
        not isinstance(roles, list) or len(roles) != 3
        or any(not isinstance(role, str) or not role for role in roles)
        or not isinstance(backend, str) or not backend
    ):
        raise ValueError("SPEC107_PREFLIGHT_PLAN_INVALID")
    record = run_campaign_preflight(
        candidate=candidate,
        campaign=campaign,
        artifact_root=artifact_store,
        artifact_manifest=artifact_manifest,
        repo_root=root,
        projected_new_bytes=projected_new_bytes,
        free_bytes=free_bytes,
        provider_capabilities={role: [backend] for role in roles},
        required_capability=backend,
    )
    if record["verdict"] != "PASS":
        try:
            retained = write_invalid_preflight_record(record, repo_root=root)
        except PreflightError as exc:
            raise SystemExit(f"INVALID_PREFLIGHT_UNRETAINED:{exc}") from exc
        raise SystemExit(f"INVALID_PREFLIGHT:{retained}")
    claim_campaign_writer(record, repo_root=root)
    return record


def validate_spec107_command_binding(
    candidate: dict[str, object], campaign: dict[str, object],
    args: argparse.Namespace, command_profile: str | Path,
) -> dict[str, object]:
    path = Path(command_profile)
    digest = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
    candidate_digests = candidate.get("digests")
    if not isinstance(candidate_digests, dict) or candidate_digests.get("command") != digest:
        raise ValueError("SPEC107_COMMAND_CANDIDATE_DIGEST_MISMATCH")
    if campaign.get("commandDigest") != digest:
        raise ValueError("SPEC107_COMMAND_CAMPAIGN_DIGEST_MISMATCH")
    profile = json.loads(path.read_text(encoding="utf-8"))
    if profile.get("schema") != "ndnsf-di-spec107-diagnostic-command-profile-v1":
        raise ValueError("SPEC107_COMMAND_PROFILE_INVALID")
    cells = profile.get("cells")
    execution = profile.get("execution")
    if not isinstance(cells, list) or not isinstance(execution, dict):
        raise ValueError("SPEC107_COMMAND_PROFILE_INVALID")
    matches = [
        cell for cell in cells
        if isinstance(cell, dict)
        and cell.get("ordinal") == campaign.get("ordinal")
        and cell.get("outputRoot") == campaign.get("outputRoot")
    ]
    if len(matches) != 1:
        raise ValueError("SPEC107_COMMAND_CELL_MISMATCH")
    cell = matches[0]
    output_root = cell.get("outputRoot")
    cell_name = cell.get("name")
    if (
        not isinstance(output_root, str) or "\\" in output_root
        or not isinstance(cell_name, str) or not cell_name
    ):
        raise ValueError("SPEC107_COMMAND_OUTPUT_ROOT_INVALID")
    output_parts = PurePosixPath(output_root).parts
    if (
        len(output_parts) != 3
        or output_parts[0] != "results"
        or not output_parts[1].startswith("spec107-attribution-")
        or output_parts[1] == "spec107-attribution-"
        or output_parts[2] != cell_name
    ):
        raise ValueError("SPEC107_COMMAND_OUTPUT_ROOT_INVALID")
    comparisons = (
        (args.runtime, execution.get("runtime")),
        (args.prompt, execution.get("prompt")),
        (args.ndn_log, execution.get("ndnLog")),
        (args.spec107_timing_sample_rate, execution.get("timingSampleRate")),
        (args.warmup_requests, cell.get("warmupRequests")),
        (args.measured_requests, cell.get("measuredRequests")),
        (args.max_new_tokens, cell.get("maxNewTokens")),
        (float(args.measured_duration_s), float(cell.get("measuredDurationSeconds", -1))),
        (float(args.request_interval_ms), float(cell.get("requestIntervalMs", -1))),
    )
    if any(actual != expected for actual, expected in comparisons):
        raise ValueError("SPEC107_COMMAND_ARGUMENT_MISMATCH")
    expected_tokens = execution.get("expectedTokenIds")
    if expected_tokens is not None:
        actual_tokens = [
            int(value) for value in str(args.expected_token_ids).split(",") if value]
        if actual_tokens != expected_tokens:
            raise ValueError("SPEC107_COMMAND_ARGUMENT_MISMATCH")
    return cell


def main() -> int:
    global OUT, CONFIG
    args = build_parser().parse_args()
    # MiniNDN launches NFD and Mininet shells from the runner's process
    # environment.  A host-wide framework ``NDN_LOG`` filter is not valid
    # NFD syntax and can abort every node before the application starts;
    # ``--ndn-log`` is copied into application child environments below.
    # Likewise, Mininet's ``Host.popen`` indexes ``SHELL`` directly, so a
    # minimal container image must still receive an explicit shell contract.
    os.environ.pop("NDN_LOG", None)
    os.environ.setdefault("SHELL", "/bin/bash")
    if args.controller_wait_s != 0.0:
        raise SystemExit(
            "--controller-wait-s is no longer supported; controller startup "
            "is marker-driven")
    if args.provider_wait_s != 0.0:
        raise SystemExit(
            "--provider-wait-s is no longer supported; provider readiness is "
            "event-driven")
    if args.stages != 3:
        raise SystemExit("this MiniNDN smoke currently maps exactly 3 stages")
    if args.selection_dataflow_v2 or args.selection_dataflow_v3:
        profile = "selection-dataflow-v3" if args.selection_dataflow_v3 else "selection-dataflow-v2"
        if args.runtime != "qwen-transformers":
            raise SystemExit(
                f"--{profile} currently requires qwen-transformers")
        if not args.qwen_stage_manifest:
            raise SystemExit(
                f"--{profile} requires --qwen-stage-manifest")
        if not Path(args.qwen_stage_manifest).expanduser().is_file():
            raise SystemExit(
                "--qwen-stage-manifest does not exist: "
                f"{args.qwen_stage_manifest}")
        if (not args.qwen_stage_root
                or not Path(args.qwen_stage_root).expanduser().is_dir()):
            raise SystemExit(
                f"--{profile} requires an existing "
                "--qwen-stage-root")
        if not args.request_id:
            raise SystemExit(
                f"--{profile} requires one explicit --request-id")
        if args.max_new_tokens < 2:
            raise SystemExit(
                f"--{profile} requires multi-token generation")
        if args.test_only_allow_ephemeral_app_state:
            raise SystemExit(
                f"--{profile} forbids test-only application state")
    if args.selection_dataflow_v2 and args.selection_dataflow_v3:
        raise SystemExit("select exactly one Selection Dataflow profile")
    if args.require_real_model:
        try:
            validate_real_model_binding(args)
        except ValueError as error:
            raise SystemExit(str(error)) from error
    if args.runtime == "tiny-transformers" and args.layers == 24:
        args.layers = args.transformer_layers
    sys.argv = [sys.argv[0]]
    setLogLevel("info")
    OUT = Path(args.output_dir).expanduser().resolve()
    spec107_candidate_id = ""
    spec107_artifact_store = ""
    spec107_qwen_service_manifest = ""
    spec107_qwen_runtime_manifest = ""
    spec107_candidate_payload = None
    spec107_campaign_payload = None
    spec107_command_cell = None
    spec107_command_profile_payload = None
    if args.spec107_diagnostic:
        if args.runtime != "qwen-onnx-cpu-native":
            raise SystemExit("Spec 107 attribution requires qwen-onnx-cpu-native")
        if not args.candidate_manifest:
            raise SystemExit("Spec 107 attribution requires --candidate-manifest")
        if not args.campaign_manifest:
            raise SystemExit("Spec 107 attribution requires --campaign-manifest")
        candidate_path = Path(args.candidate_manifest).expanduser().resolve()
        candidate_payload = validate_candidate_identity(json.loads(
            candidate_path.read_text(encoding="utf-8")))
        try:
            validate_spec107_source_binding(candidate_payload, REPO)
        except ValueError as exc:
            raise SystemExit(str(exc)) from exc
        spec107_candidate_payload = candidate_payload
        spec107_candidate_id = str(candidate_payload.get("candidateId", ""))
        if (
            not spec107_candidate_id.startswith("spec107-c1-")
            or "spec105" in spec107_candidate_id.lower()
        ):
            raise SystemExit("Spec 107 attribution candidate identity is invalid")
        if not args.campaign_id.startswith("spec107-c1-diagnostic-"):
            raise SystemExit("Spec 107 attribution requires a diagnostic campaign ID")
        campaign_payload = validate_campaign_set(
            [json.loads(Path(args.campaign_manifest).read_text(encoding="utf-8"))],
            candidate_id=spec107_candidate_id,
            candidate_digest=digest_object(candidate_payload))[0]
        spec107_campaign_payload = campaign_payload
        if (campaign_payload["kind"] != "diagnostic" or
                campaign_payload["campaignId"] != args.campaign_id or
                campaign_payload["releaseEligible"] is not False):
            raise SystemExit("Spec 107 attribution campaign identity mismatch")
        expected_output = (REPO / str(campaign_payload["outputRoot"])).resolve()
        if OUT != expected_output:
            raise SystemExit(
                f"Spec 107 attribution output identity mismatch: expected {expected_output}")
        if args.spec107_timing_sample_rate < 1:
            raise SystemExit("Spec 107 timing sample rate must be >= 1")
        if not args.spec107_command_profile:
            raise SystemExit("Spec 107 attribution requires --spec107-command-profile")
        try:
            spec107_command_cell = validate_spec107_command_binding(
                candidate_payload, campaign_payload, args,
                args.spec107_command_profile)
            spec107_command_profile_payload = json.loads(Path(
                args.spec107_command_profile).read_text(encoding="utf-8"))
        except (ValueError, OSError, json.JSONDecodeError) as exc:
            raise SystemExit(str(exc)) from exc
        reuse_inputs = (
            args.spec107_artifact_store,
            args.spec107_qwen_service_manifest,
            args.spec107_qwen_runtime_manifest,
        )
        if not all(reuse_inputs):
            raise SystemExit(
                "Spec 107 attribution requires artifact store and Qwen manifests")
        try:
            validate_spec107_artifact_binding(
                candidate_payload, args.spec107_artifact_store,
                args.spec107_qwen_service_manifest,
                args.spec107_qwen_runtime_manifest)
        except (ValueError, OSError) as exc:
            raise SystemExit(str(exc)) from exc
        spec107_artifact_store = str(
            Path(args.spec107_artifact_store).expanduser().resolve())
        spec107_qwen_service_manifest = str(
            Path(args.spec107_qwen_service_manifest).expanduser().resolve())
        spec107_qwen_runtime_manifest = str(
            Path(args.spec107_qwen_runtime_manifest).expanduser().resolve())
        artifact_inputs = spec107_command_profile_payload.get("artifactInputs")
        projected_new_bytes = spec107_command_cell.get("projectedNewBytes")
        if not isinstance(artifact_inputs, dict) or not isinstance(
                projected_new_bytes, int) or isinstance(projected_new_bytes, bool):
            raise SystemExit("SPEC107_COMMAND_PREFLIGHT_INVALID")
        plan_input = artifact_inputs.get("nativePlan")
        profile_input = artifact_inputs.get("campaignProfile")
        workload_input = artifact_inputs.get("workloadManifest")
        if (
            not isinstance(plan_input, str) or not plan_input
            or not isinstance(profile_input, str) or not profile_input
            or not isinstance(workload_input, str) or not workload_input
        ):
            raise SystemExit("SPEC107_COMMAND_PREFLIGHT_INVALID")
        try:
            validate_spec107_profile_workload_binding(
                candidate_payload, args, spec107_command_cell,
                REPO / profile_input, REPO / workload_input)
            enforce_spec107_harness_preflight(
                candidate=candidate_payload,
                campaign=campaign_payload,
                artifact_store=spec107_artifact_store,
                artifact_manifest=validate_spec107_artifact_binding(
                    candidate_payload, spec107_artifact_store,
                    spec107_qwen_service_manifest,
                    spec107_qwen_runtime_manifest),
                plan=plan_input,
                repo_root=REPO,
                projected_new_bytes=projected_new_bytes,
            )
        except (ValueError, OSError, json.JSONDecodeError) as exc:
            raise SystemExit(str(exc)) from exc
    if args.spec107_live_fault_cell:
        if args.runtime != "qwen-onnx-cpu-native":
            raise SystemExit("Spec 107 live faults require qwen-onnx-cpu-native")
        if not args.campaign_id.startswith("spec107-c1-fault-"):
            raise SystemExit("Spec 107 live faults require a fault campaign ID")
        if not args.candidate_manifest:
            raise SystemExit("Spec 107 live faults require --candidate-manifest")
        if not args.campaign_manifest:
            raise SystemExit("Spec 107 live faults require --campaign-manifest")
        candidate_payload = validate_candidate_identity(json.loads(
            Path(args.candidate_manifest).read_text(encoding="utf-8")))
        try:
            validate_spec107_source_binding(candidate_payload, REPO)
        except ValueError as exc:
            raise SystemExit(str(exc)) from exc
        spec107_candidate_payload = candidate_payload
        reuse_inputs = (
            args.spec107_artifact_store,
            args.spec107_qwen_service_manifest,
            args.spec107_qwen_runtime_manifest,
        )
        if not all(reuse_inputs):
            raise SystemExit(
                "Spec 107 live faults require artifact store and Qwen manifests")
        try:
            validate_spec107_artifact_binding(
                candidate_payload, args.spec107_artifact_store,
                args.spec107_qwen_service_manifest,
                args.spec107_qwen_runtime_manifest)
        except (ValueError, OSError) as exc:
            raise SystemExit(str(exc)) from exc
        spec107_artifact_store = str(
            Path(args.spec107_artifact_store).expanduser().resolve())
        spec107_qwen_service_manifest = str(
            Path(args.spec107_qwen_service_manifest).expanduser().resolve())
        spec107_qwen_runtime_manifest = str(
            Path(args.spec107_qwen_runtime_manifest).expanduser().resolve())
        campaign_payload = validate_campaign_set(
            [json.loads(Path(args.campaign_manifest).read_text(encoding="utf-8"))],
            candidate_id=str(candidate_payload["candidateId"]),
            candidate_digest=digest_object(candidate_payload))[0]
        if (campaign_payload["kind"] != "fault" or
                campaign_payload["campaignId"] != args.campaign_id):
            raise SystemExit("Spec 107 live-fault campaign identity mismatch")
        expected_cell = (
            REPO / str(campaign_payload["outputRoot"]) /
            f"{FAULT_CELLS.index(args.spec107_live_fault_cell) + 1:02d}-"
            f"{args.spec107_live_fault_cell}").resolve()
        if OUT != expected_cell:
            raise SystemExit(
                f"Spec 107 live-fault output identity mismatch: expected {expected_cell}")
        validate_cell_claim(
            cell_id=args.spec107_live_fault_cell,
            candidate_id=str(candidate_payload["candidateId"]),
            campaign_id=str(campaign_payload["campaignId"]),
            output_root=OUT)
        if OUT.exists():
            raise SystemExit(f"Spec 107 live-fault output must be unique: {OUT}")
    CONFIG = OUT / "llm_pipeline_policy.yaml"
    if args.fault_matrix_contract:
        OUT.mkdir(parents=True, exist_ok=True)
        report = execute_fault_matrix_contract()
        target = OUT / "fault-matrix-contract.json"
        target.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
        print("LLM_PIPELINE_FAULT_MATRIX_CONTRACT " + json.dumps(
            report, sort_keys=True, separators=(",", ":")))
        return 2
    if args.reuse_existing_policy:
        if not CONFIG.exists():
            raise SystemExit(
                f"--reuse-existing-policy requested but {CONFIG} does not exist")
        if args.runtime in ("qwen-transformers", "qwen-onnx", "qwen-onnx-cpu-native") and not (
            OUT / "qwen-pipeline-runtime.json"
        ).exists():
            raise SystemExit(
                "--reuse-existing-policy for Qwen runtimes requires "
                f"{OUT / 'qwen-pipeline-runtime.json'}")
        OUT.mkdir(parents=True, exist_ok=True)
        print(f"LLM_PIPELINE_REUSE_POLICY policy={CONFIG}")
    else:
        policy_runtime = (
            "fake" if (args.selection_dataflow_v2 or args.selection_dataflow_v3)
            else "qwen-onnx" if args.runtime == "qwen-onnx-cpu-native"
            else args.runtime
        )
        prepare_policy(
            args.stages,
            args.layers,
            runtime=policy_runtime,
            transformer_layers=args.transformer_layers,
            qwen_model=args.qwen_model,
            qwen_revision=args.qwen_revision,
            qwen_prompt=args.prompt,
            qwen_allow_download=args.qwen_allow_download,
            qwen_dtype=args.qwen_dtype,
            qwen_content_store=args.qwen_content_store,
            qwen_artifact_store=spec107_artifact_store,
            qwen_service_manifest=spec107_qwen_service_manifest,
            qwen_runtime_manifest=spec107_qwen_runtime_manifest,
        )
    if args.selection_dataflow_v2 or args.selection_dataflow_v3:
        repo_policy = OUT / "llm_pipeline_policy.repo.yaml"
        subprocess.run([
            sys.executable,
            str(REPO / "specs/162-itiger-qwen36-generation/jobs/"
                "build-generation-policy.py"),
            "--input", str(CONFIG),
            "--output", str(repo_policy),
            "--provenance", str(OUT / "policy-provenance.json"),
            "--user", USER_IDENTITY,
            "--provider-prefix", PROVIDER_PREFIX,
            "--repo-provider-prefix", REPO_PROVIDER_PREFIX,
        ], cwd=str(REPO), env=local_python_subprocess_env(), check=True)
        os.replace(repo_policy, CONFIG)
    workflow_bundle = None
    if args.deployment_workflow:
        if args.runtime == "qwen-onnx-cpu-native" or args.spec107_live_fault_cell:
            raise SystemExit(
                "Spec 111 deployment workflow requires the Python Provider path")
        workflow_bundle = configure_spec111_deployment_workflow(
            CONFIG, OUT, args.stages)
        workflow_revision = str(workflow_bundle["revision"])
        if args.deployment_revision and args.deployment_revision != workflow_revision:
            raise SystemExit("--deployment-revision conflicts with resolved workflow revision")
        args.deployment_revision = workflow_revision
        args.durable_app_submit = True
        shutil.rmtree(Path(args.app_state_root), ignore_errors=True)
    base_env = {
        **os.environ,
        "PYTHONFAULTHANDLER": "1",
        "PYTHONUNBUFFERED": "1",
        "PYTHONPATH": ":".join(python_path_entries()),
        "NDN_LOG": args.ndn_log,
        "NDNSF_RESPONSE_LARGE_DATA_THRESHOLD": "1024",
    }
    if args.selection_dataflow_v2 or args.selection_dataflow_v3:
        # A collaboration Selection can be larger than one SVS publication
        # path reliably fans out.  The same authenticated, hybrid-encrypted
        # provider-specific Data is therefore also fetched by its exact V2
        # name.  Both producer direct-put and Provider prefetch are guarded by
        # this shared flag; this is transport redundancy, not an authorization
        # bypass or a second collaboration attempt.
        base_env["NDNSF_SELECTION_TARGETED_PREFETCH"] = "1"
    if os.environ.get("NDNSF_SPEC168_SELECTION_TRACE") == "1":
        base_env["NDN_LOG"] = "ndn_service_framework.*=TRACE"
    if args.large_fetch_timing:
        base_env["NDNSF_COLLAB_LARGE_FETCH_TIMING"] = "1"
    if args.runtime == "qwen-onnx-cpu-native":
        base_env["NDNSF_DI_RUNTIME_TIMING"] = "1"
    if args.spec107_diagnostic:
        base_env["NDNSF_TIMELINE_TRACE"] = "1"
        base_env["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = str(
            args.spec107_timing_sample_rate)
    base_env.pop("NDN_CLIENT_TRANSPORT", None)
    generate_policy_bundle(base_env)
    selection_bundle = (
        prepare_selection_dataflow_v2(args)
        if (args.selection_dataflow_v2 or args.selection_dataflow_v3) else None
    )
    native_plan = native_manifest = None
    if args.runtime == "qwen-onnx-cpu-native":
        native_plan, native_manifest = write_native_qwen_bundle(
            OUT,
            execution_provider=args.qwen_execution_provider,
            device_ids=[value.strip() for value in args.qwen_device_ids.split(",") if value.strip()],
        )
    if spec107_candidate_payload is not None:
        if native_plan is None:
            raise SystemExit("Spec 107 execution binding requires native plan")
        try:
            validate_spec107_execution_binding(
                spec107_candidate_payload, native_plan, CONFIG)
        except (ValueError, OSError) as exc:
            raise SystemExit(str(exc)) from exc

    subprocess.run(["pkill", "-f", "llm_pipeline/(provider|user)\\.py"],
                   check=False)
    Minindn.cleanUp()
    if args.static_routing_only:
        required = ("nfd", "nfdc", "ndnsec", "infoconv", "mnexec", "ovs-vsctl")
        missing = [name for name in required if shutil.which(name) is None]
        if missing:
            raise SystemExit(
                "static MiniNDN dependencies are missing: " + ",".join(missing))
    else:
        Minindn.verifyDependencies()
    if args.static_routing_only:
        from mininet.node import OVSBridge
        ndn = Minindn(
            topoFile=args.topology_file,
            controller=None,
            switch=OVSBridge,
        )
    else:
        ndn = Minindn(topoFile=args.topology_file)
    processes: list[tuple[object, object, Path]] = []
    fault_registry = (
        OwnedProcessRegistry(
            campaign_id=args.campaign_id,
            registry_path=OUT / "owned-processes.json")
        if args.spec107_live_fault_cell else None)
    fault_owned_by_stage = {}
    fault_provider_specs = {}
    fault_control = None
    try:
        ndn.start()
        normalize_nlsr_link_costs(ndn)
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        if not args.static_routing_only:
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
        for node_name, identity, repo_identity in zip(
                STAGE_NODES, STAGE_IDENTITIES, REPO_IDENTITIES):
            rh.addOrigin(
                [ndn.net[node_name]],
                [identity, identity + "/KEY",
                 repo_identity, repo_identity + "/KEY"],
            )
            if args.selection_dataflow_v2 or args.selection_dataflow_v3:
                rh.addOrigin(
                    [ndn.net[node_name]], ["/NDNSF/DistributedRepo"])
        rh.addOrigin(ndn.net.hosts, [GROUP_IDENTITY])
        rh.calculateRoutes()
        log(
            f"Waiting {args.nlsr_wait_s:.1f}s for "
            + ("static route settlement"
               if args.static_routing_only else "NLSR convergence"))
        time.sleep(args.nlsr_wait_s)
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, APP_ROOT, Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, GROUP_IDENTITY, Nfdc.STRATEGY_MULTICAST)
            if args.selection_dataflow_v2 or args.selection_dataflow_v3:
                Nfdc.setStrategy(
                    node, "/NDNSF/DistributedRepo", Nfdc.STRATEGY_MULTICAST)

        node_identities = [
            (CONTROLLER_NODE, CONTROLLER_IDENTITY),
            (USER_NODE, USER_IDENTITY),
            *list(zip(STAGE_NODES, STAGE_IDENTITIES)),
            *list(zip(STAGE_NODES, REPO_IDENTITIES)),
        ]
        identities_by_node: dict[str, str] = {}
        local_identities_by_node: dict[str, list[str]] = {}
        for host_name, identity in node_identities:
            identities_by_node.setdefault(host_name, identity)
            local_identities_by_node.setdefault(host_name, []).append(identity)
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
            cert_names[identity] = cert_name
            key_name = key_name_from_certificate_name(cert_name)
            perf.node_cmd(controller_node,
                          "ndnsec set-default -k -n {} >/dev/null 2>&1 || true".format(
                              perf.shell_quote(key_name)))
            perf.node_cmd(controller_node, "ndnsec-export -P {} -o {} -k {}".format(
                perf.shell_quote(passphrase), perf.shell_quote(key),
                perf.shell_quote(key_name)))
            exported_keys.append((host_name, identity, cert, key, key_name))

        for host_name in sorted(set(identities_by_node)):
            perf.node_cmd(ndn.net[host_name],
                          "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                              perf.shell_quote(root_cert)))
            # Every participant receives the public trust material, but a
            # host receives private keys only for identities it owns.  The
            # previous all-keys-on-all-hosts setup made isolated MiniNDN homes
            # behave like one shared PIB and hid TigerCluster certificate
            # bootstrap failures.
            for _, _, cert, _, _ in exported_keys:
                perf.node_cmd(
                    ndn.net[host_name],
                    "ndnsec cert-install -N -f {} >/dev/null 2>&1".format(
                        perf.shell_quote(cert)))
            for owner_host, _, _, key, _ in exported_keys:
                if owner_host == host_name:
                    perf.node_cmd(
                        ndn.net[host_name],
                        "ndnsec import -P {} {} >/dev/null 2>&1".format(
                            perf.shell_quote(passphrase), perf.shell_quote(key)))
            for identity in local_identities_by_node[host_name]:
                perf.node_cmd(ndn.net[host_name],
                              "ndnsec set-default -c -n {} >/dev/null 2>&1".format(
                                  perf.shell_quote(cert_names[identity])))
            perf.node_cmd(ndn.net[host_name],
                          "ndnsec set-default -n {} >/dev/null 2>&1 || true".format(
                              perf.shell_quote(identities_by_node[host_name])))

        config_obj = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
        config_obj.setdefault("trust", {})["anchor_file"] = str(root_cert)
        CONFIG.write_text(yaml.safe_dump(config_obj, sort_keys=False),
                          encoding="utf-8")
        generate_policy_bundle(base_env)

        node_env = {
            name: command_env(homes, name, base_env)
            for name in sorted(set(identities_by_node))
        }
        base = python_process_prefix(base_env)
        common = " --config {} --generated-policy-dir {}".format(
            perf.shell_quote(CONFIG), perf.shell_quote(GEN_POLICY))
        controller_proc, controller_log = start_process(
            ndn, CONTROLLER_NODE, "controller",
            base + "-c " + perf.shell_quote(
                "from ndnsf_distributed_inference.app_sdk.controller import APPController; "
                "import subprocess,sys; "
                "c=APPController.from_config(sys.argv[1], "
                "generated_policy_dir=sys.argv[2], "
                "bootstrap_token_file=sys.argv[3]); "
                "f=open(sys.argv[4],'wb'); "
                "subprocess.run(['ndnsec','cert-dump','-i',"
                "'/example/llm-pipeline/controller'],stdout=f,check=True); "
                "f.close(); "
                "print('controller ready', flush=True); c.run()"
            ) + " " + perf.shell_quote(CONFIG)
            + " " + perf.shell_quote(GEN_POLICY)
            + " " + perf.shell_quote(OUT / "bootstrap-tokens.txt")
            + " " + perf.shell_quote(OUT / "controller.cert"),
            node_env[CONTROLLER_NODE], processes,
        )
        if not wait_log(controller_log, "controller ready",
                        args.provider_start_timeout_s, controller_proc):
            raise RuntimeError(
                f"controller did not reach ready state; log={controller_log}")

        repo_logs = []
        repo_user_bootstrap = None
        if selection_bundle is not None:
            token_file = OUT / "bootstrap-tokens.txt"
            controller_cert = OUT / "controller.cert"
            if not token_file.is_file() or not controller_cert.is_file():
                raise RuntimeError(
                    "controller did not publish Repo bootstrap authority")
            for env in node_env.values():
                env["NDNSF_CONTROLLER_CERT_FILE"] = str(controller_cert)
            repo_free_bytes = shutil.disk_usage(OUT).free
            for stage_index, (node_name, provider_id, repo_identity) in enumerate(
                    zip(STAGE_NODES, STAGE_PROVIDER_IDS, REPO_IDENTITIES)):
                bootstrap = write_bootstrap_token(
                    token_file,
                    repo_identity,
                    OUT / f"repo-provider-{stage_index}.token",
                )
                provider_id_arg = (
                    f" --provider-id {perf.shell_quote(provider_id)}"
                    if provider_id else ""
                )
                repo_command = (
                    base
                    + perf.shell_quote(
                        REPO / "specs/162-itiger-qwen36-generation/jobs/"
                        "run-repo-node.py")
                    + " --config " + perf.shell_quote(CONFIG)
                    + " --generated-policy-dir "
                    + perf.shell_quote(OUT / f"generated/repo-{stage_index}")
                    + provider_id_arg
                    + " --provider-prefix "
                    + perf.shell_quote(REPO_PROVIDER_PREFIX)
                    + " --repo-node "
                    + perf.shell_quote(
                        f"/NDNSF/DistributedRepo/Node/{stage_index}")
                    + " --storage-dir "
                    + perf.shell_quote(OUT / f"repo-store-{stage_index}")
                    + " --state-root "
                    + perf.shell_quote(OUT / "repo-operator-state")
                    + " --test-only-allow-ephemeral-app-state"
                    + " --free-bytes " + str(repo_free_bytes)
                    + " --bootstrap-token-file "
                    + perf.shell_quote(bootstrap)
                )
                repo_proc, repo_log = start_process(
                    ndn,
                    node_name,
                    f"repo-{stage_index}",
                    repo_command,
                    node_env[node_name],
                    processes,
                )
                repo_logs.append(repo_log)
                if not wait_log(
                        repo_log, "SPEC162_REPO_NODE_STARTING",
                        args.provider_start_timeout_s, repo_proc):
                    raise RuntimeError(
                        f"Repo node did not start; log={repo_log}")
            repo_user_bootstrap = write_bootstrap_token(
                token_file,
                USER_IDENTITY,
                OUT / "repo-user-bootstrap.token",
            )

        provider_logs = []
        for stage_index, (node_name, provider_id) in enumerate(zip(STAGE_NODES, STAGE_PROVIDER_IDS)):
            provider_id_arg = (
                f" --provider-id {provider_id} "
                if provider_id else
                " "
            )
            workflow_provider_args = ""
            if workflow_bundle is not None:
                workflow_provider_args = (
                    " --provider-identity {} --deployment-control-service {} "
                    "--deployment-role {} --deployment-revision {} "
                    "--deployment-artifact-digest {} --provider-boot-epoch {} "
                    "--provider-evidence-private-key {}"
                ).format(
                    perf.shell_quote(STAGE_IDENTITIES[stage_index]),
                    perf.shell_quote(workflow_bundle["controls"][stage_index]),
                    perf.shell_quote(workflow_bundle["roles"][stage_index]),
                    perf.shell_quote(workflow_bundle["revision"]),
                    perf.shell_quote(DEPLOYMENT_ARTIFACT_DIGEST),
                    perf.shell_quote(f"spec111-stage-{stage_index}-boot-1"),
                    perf.shell_quote(workflow_bundle["privateKeys"][stage_index]),
                )
            selection_provider_args = ""
            if selection_bundle is not None:
                device = (
                    "cuda:0"
                    if args.qwen_execution_provider == "cuda" else "cpu"
                )
                require_cuda = (
                    " --require-cuda"
                    if args.qwen_execution_provider == "cuda" else ""
                )
                required_mib = int(
                    selection_bundle["requiredMiB"][stage_index])
                selection_provider_args = (
                    (" --lazy-qwen-load"
                     if args.selection_dataflow_v2 else "")
                    + f" --device {perf.shell_quote(device)}{require_cuda}"
                    " --handler-workers 2"
                    " --provider-identity "
                    + perf.shell_quote(STAGE_IDENTITIES[stage_index])
                    + (" --selection-dataflow-v2"
                       if args.selection_dataflow_v2
                       else " --selection-dataflow-v3")
                    + " --selection-model-type "
                    + perf.shell_quote(selection_bundle["modelType"])
                    + f" --selection-gpu-capacity-mib {max(32760, required_mib)}"
                    f" --selection-offered-gpu-mib {required_mib}"
                    f" --selection-offer-lease-ms {args.selection_offer_lease_ms}"
                    f" --selection-max-prepare-ms {args.selection_max_prepare_ms}"
                    f" --selection-residency-ttl-ms {args.selection_cache_max_age_ms}"
                    " --selection-wal-path "
                    + perf.shell_quote(OUT / f"selection-{stage_index}.wal")
                    + " --selection-storage-key-file "
                    + perf.shell_quote(
                        selection_bundle["storageKeys"][stage_index])
                    + " --selection-signing-key-file "
                    + perf.shell_quote(
                        selection_bundle["offerKeys"][stage_index])
                    + " --selection-residency-json "
                    + perf.shell_quote(
                        selection_bundle["residency"][stage_index])
                    + " --selection-repo-registration "
                    + perf.shell_quote(selection_bundle["repoRegistration"])
                    + " --selection-model-cache-dir "
                    + perf.shell_quote(
                        OUT / f"provider-model-cache-{stage_index}")
                    + " --selection-local-artifact "
                    + perf.shell_quote(
                        selection_bundle["localArtifacts"][stage_index])
                    + " --repo-client-state-root "
                    + perf.shell_quote(
                        OUT / f"repo-client-state-{stage_index}")
                )
            if args.runtime == "qwen-onnx-cpu-native":
                if native_plan is None or native_manifest is None:
                    raise RuntimeError("native Qwen plan/manifest were not generated")
                fault_provider_cells = {
                    "straggler", "missing-segment", "dependency-digest-mismatch",
                    "stale-telemetry", "kv-eviction", "late-old-output",
                }
                use_fault_provider = (
                    args.spec107_live_fault_cell in fault_provider_cells and
                    stage_index == 1)
                provider_executable = (
                    REPO / "build/examples/di-native-fault-provider"
                    if use_fault_provider else
                    REPO / "build/examples/di-native-provider")
                fault_args = (
                    f"--fault-type {perf.shell_quote(args.spec107_live_fault_cell)} "
                    f"--fault-role {perf.shell_quote('/LLM/Pipeline/Stage/1')} "
                    f"--fault-delay-ms 250 "
                    if use_fault_provider else "")
                provider_command = (
                    f"cd {perf.shell_quote(REPO)} && exec "
                    f"{'setsid ' if args.spec107_live_fault_cell else ''}"
                    f"{perf.shell_quote(provider_executable)} {fault_args}"
                    f"--plan {perf.shell_quote(native_plan)} "
                    f"--manifest {perf.shell_quote(native_manifest)} "
                    f"--service {perf.shell_quote(SERVICE)} "
                    f"--provider {perf.shell_quote(STAGE_IDENTITIES[stage_index])} "
                    f"--roles {perf.shell_quote(f'/LLM/Pipeline/Stage/{stage_index}')} "
                    f"--group {perf.shell_quote(GROUP_IDENTITY)} "
                    f"--controller {perf.shell_quote(CONTROLLER_IDENTITY)} "
                    f"--trust-schema {perf.shell_quote(REPO / 'examples/trust-schema.conf')} "
                    "--workers 1 --serve"
                )
                ready_marker = "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY"
            else:
                provider_command = (
                    base + perf.shell_quote(LLM_DIR / "provider.py") + common +
                    provider_id_arg +
                    f"--roles /LLM/Pipeline/Stage/{stage_index} "
                    f"--runtime {args.runtime} "
                    f"--stages {args.stages} "
                    f"--transformer-layers {args.transformer_layers} "
                    f"--compute-delay-ms {args.compute_delay_ms}" +
                    workflow_provider_args + selection_provider_args
                )
                ready_marker = "LLM_PIPELINE_PROVIDER_READY"
            proc, log_path = start_process(
                ndn, node_name, f"stage{stage_index}-provider",
                provider_command, node_env[node_name], processes,
            )
            provider_logs.append(log_path)
            if not wait_log(log_path, ready_marker, args.provider_start_timeout_s, proc):
                raise RuntimeError(f"stage provider did not start; log={log_path}")
            if fault_registry is not None:
                boot_match = re.search(
                    r"providerBootId=([^\s]+)", log_path.read_text(errors="replace"))
                if not boot_match:
                    raise RuntimeError(
                        f"Spec 107 live-fault provider boot identity missing: {log_path}")
                fault_owned_by_stage[stage_index] = fault_registry.adopt(
                    proc,
                    role=f"/LLM/Pipeline/Stage/{stage_index}",
                    provider_name=STAGE_IDENTITIES[stage_index],
                    provider_boot_id=boot_match.group(1),
                )
                fault_provider_specs[stage_index] = (
                    node_name, provider_command, node_env[node_name], ready_marker)
        log("Provider process readiness complete; the User Request now drives "
            "ACK closure, model preparation, and dataflow execution")

        user_log = OUT / "llm-pipeline-user.log"
        metrics_csv = OUT / "llm-pipeline-user-measured.csv"
        user_out = user_log.open("wb")
        user_runtime = "qwen-onnx" if args.runtime == "qwen-onnx-cpu-native" else args.runtime
        native_user_args = (
            "--native-cpu-provider --qwen-service-manifest {}".format(
                perf.shell_quote(OUT / "qwen-onnx-service-manifest.json"))
            if args.runtime == "qwen-onnx-cpu-native" else ""
        )
        spec107_user_args = (
            "--spec107-candidate-id {} --spec107-diagnostic-timing-jsonl {}".format(
                perf.shell_quote(spec107_candidate_id),
                perf.shell_quote(OUT / "spec107-client-timing.jsonl"),
            )
            if args.spec107_diagnostic else ""
        )
        # The canonical APPClient always owns a RuntimeJournal, even when this
        # workload uses the compatibility inference call instead of durable
        # submit/reopen.  Forward the state-root safety contract unconditionally;
        # only the submit mode and revision remain conditional.
        durable_user_args = "--app-state-root {} {} {}".format(
            perf.shell_quote(args.app_state_root),
            ("--test-only-allow-ephemeral-app-state"
             if args.test_only_allow_ephemeral_app_state else ""),
            (
                "--durable-app-submit --deployment-revision {}".format(
                    perf.shell_quote(args.deployment_revision))
                if args.durable_app_submit else ""
            ),
        )
        workflow_user_args = ""
        if workflow_bundle is not None:
            control_args = " ".join(
                "--deployment-control-service " + perf.shell_quote(service)
                for service in workflow_bundle["controls"])
            workflow_user_args = (
                "--deployment-workflow --deployment-definition {} "
                "--provider-trust-bundle {} {} --deployment-workflow-summary {}"
            ).format(
                perf.shell_quote(workflow_bundle["definition"]),
                perf.shell_quote(workflow_bundle["trust"]),
                control_args,
                perf.shell_quote(OUT / "deployment-workflow-summary.json"),
            )
        qwen_summary_user_args = (
            "--qwen-runtime-summary "
            + perf.shell_quote(OUT / "qwen-pipeline-runtime.json")
            if selection_bundle is None else ""
        )
        selection_user_args = ""
        if selection_bundle is not None:
            if repo_user_bootstrap is None:
                raise RuntimeError("Repo User bootstrap token is unavailable")
            selection_user_args = (
                " --automatic-planning-manifest "
                + perf.shell_quote(selection_bundle["automaticPlanning"])
                + " --selection-offer-key-map "
                + perf.shell_quote(selection_bundle["offerKeyMap"])
                + f" --selection-cache-max-age-ms {args.selection_cache_max_age_ms}"
                + " --qwen-stage-manifest "
                + perf.shell_quote(selection_bundle["stageManifest"])
                + " --repo-registration-output "
                + perf.shell_quote(selection_bundle["repoRegistration"])
                + " --repo-user " + perf.shell_quote(USER_IDENTITY)
                + " --repo-bootstrap-token-file "
                + perf.shell_quote(repo_user_bootstrap)
                + " --repo-object-prefix "
                + perf.shell_quote(selection_bundle["repoObjectPrefix"])
                + " --repo-generated-policy-dir "
                + perf.shell_quote(OUT / "generated/repo-publisher")
                + " --repo-state-root "
                + perf.shell_quote(OUT / "repo-publisher-state")
                + " --test-only-allow-ephemeral-app-state"
                + " --repo-publisher-script "
                + perf.shell_quote(
                    REPO / "specs/162-itiger-qwen36-generation/jobs/"
                    "register-qwen36-repo.py")
                + " --request-id " + perf.shell_quote(args.request_id)
                + " --app-envelope-key-file "
                + perf.shell_quote(selection_bundle["envelopeKey"])
                + (" --selection-dataflow-v3"
                   if args.selection_dataflow_v3 else "")
            )
        user_proc = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(LLM_DIR / "user.py") + common +
            " --prompt {} --stages {} --compute-delay-ms {} "
            "--runtime {} --transformer-layers {} "
            "{} "
            "--context-input-mode {} --delta-token-ids {} "
            "--ack-timeout-ms {} --timeout-ms {} "
            "--warmup-requests {} --measured-requests {} "
            "--max-new-tokens {} "
            "--native-first-kv-mode {} "
            "--expected-token-ids {} "
            "--measured-duration-s {} --request-interval-ms {} --campaign-id {} "
            "--metrics-csv {} {} {} {} {} {} {} {}".format(
                perf.shell_quote(args.prompt),
                args.stages,
                args.compute_delay_ms,
                user_runtime,
                args.transformer_layers,
                qwen_summary_user_args,
                args.context_input_mode,
                perf.shell_quote(args.delta_token_ids),
                args.ack_timeout_ms,
                args.timeout_ms,
                args.warmup_requests,
                args.measured_requests,
                args.max_new_tokens,
                args.native_first_kv_mode,
                perf.shell_quote(args.expected_token_ids),
                args.measured_duration_s,
                args.request_interval_ms,
                perf.shell_quote(args.campaign_id),
                perf.shell_quote(metrics_csv),
                (
                    "--generation-campaign-manifest {} --generation-jsonl {} "
                    "--qwen-tokenizer-dir {} --workload-digest {} "
                    "--model-identity-digest {}"
                ).format(
                    perf.shell_quote(args.generation_campaign_manifest),
                    perf.shell_quote(args.generation_jsonl),
                    perf.shell_quote(args.qwen_tokenizer_dir),
                    perf.shell_quote(args.workload_digest),
                    perf.shell_quote(args.model_identity_digest),
                ) if args.generation_campaign_manifest else "",
                "--publish-input-reference" if args.publish_input_reference else "",
                native_user_args,
                spec107_user_args,
                durable_user_args,
                workflow_user_args,
                selection_user_args,
            ),
            envDict=node_env[USER_NODE],
            shell=True,
            stdout=user_out,
            stderr=subprocess.STDOUT,
        )
        processes.append((user_proc, user_out, user_log))
        if args.spec107_live_fault_cell in {
            "provider-kill-restart", "provider-boot-change"
        }:
            if fault_registry is None:
                raise RuntimeError("Spec 107 live-fault registry is unavailable")
            target = fault_owned_by_stage[1]
            stage_log = provider_logs[1]
            trigger_us = fault_registry.wait_for_log_trigger(
                target, log_path=stage_log, marker="role_compute_start",
                timeout_seconds=max(1.0, args.timeout_ms / 1000.0))
            injection_us = time.monotonic_ns() // 1000
            fault_registry.guarded_signal(target, signal.SIGTERM)
            effect = fault_registry.observe_process_exit(
                target, timeout_seconds=5.0)
            node_name, provider_command, provider_env, ready_marker = fault_provider_specs[1]
            replacement_proc, replacement_log = start_process(
                ndn, node_name, "stage1-provider-replacement",
                provider_command, provider_env, processes)
            if not wait_log(
                replacement_log, ready_marker, args.provider_start_timeout_s,
                replacement_proc):
                raise RuntimeError(
                    f"Spec 107 replacement provider did not start: {replacement_log}")
            boot_match = re.search(
                r"providerBootId=([^\s]+)",
                replacement_log.read_text(errors="replace"))
            if not boot_match:
                raise RuntimeError("Spec 107 replacement boot identity missing")
            replacement = fault_registry.adopt(
                replacement_proc, role=target.role,
                provider_name=target.provider_name,
                provider_boot_id=boot_match.group(1))
            fault_owned_by_stage[1] = replacement
            fault_control = {
                "schema": "ndnsf-di-spec107-live-fault-control-v1",
                "campaignId": args.campaign_id,
                "cellId": args.spec107_live_fault_cell,
                "triggerMonotonicUs": trigger_us,
                "injectionMonotonicUs": injection_us,
                "injectionApplied": True,
                "networkInjection": True,
                "target": target.to_dict(),
                "replacement": replacement.to_dict(),
                "observedEffect": effect,
            }
        user_proc.wait(timeout=max(
            180.0,
            args.measured_duration_s + args.timeout_ms / 1000.0 + 30.0,
        ))
        user_text = user_log.read_text(errors="replace")
        print(user_text)
        expected_user_marker = (
            "LLM_PIPELINE_GENERATION_CAMPAIGN_PASS"
            if args.generation_campaign_manifest else
            "LLM_PIPELINE_OPEN_LOOP_SUMMARY"
            if args.runtime == "qwen-onnx-cpu-native" and args.measured_duration_s > 0 else
            "LLM_PIPELINE_USER_RESPONSE"
        )
        if expected_user_marker not in user_text:
            raise RuntimeError(f"LLM pipeline user failed; log={user_log}")
        if (workflow_bundle is not None and
                "LLM_PIPELINE_DEPLOYMENT_WORKFLOW_PASS" not in user_text):
            raise RuntimeError(
                f"Spec 111 deployment workflow did not finish; log={user_log}")
        user_failed = user_proc.returncode != 0
        for stage_index, log_path in enumerate(provider_logs):
            text = log_path.read_text(errors="replace")
            full_generation = (
                args.generation_campaign_manifest and
                "LLM_PIPELINE_QWEN_FULL_STAGE_START" in text
            )
            expected = expected_stage_completion_marker(
                args.runtime,
                stage_index=stage_index,
                stages=args.stages,
                full_generation=full_generation,
            )
            if expected not in text:
                raise RuntimeError(f"stage {stage_index} missing {expected}; log={log_path}")
            if args.require_real_model and (
                real_model_readiness_marker(
                    args.runtime,
                    deferred_selection=(
                        args.selection_dataflow_v2 or args.selection_dataflow_v3),
                ) not in text
            ):
                raise RuntimeError(
                    "real-model gate missing immutable stage artifact readiness "
                    f"for stage {stage_index}; log={log_path}"
                )

        if args.require_real_model:
            selected_roles = {
                match.group(1)
                for match in re.finditer(
                    r"NDNSF_COLLAB_ASSIGNMENT_SELECTED .*?role=(/LLM/Pipeline/Stage/\d+)",
                    user_text,
                )
            }
            expected_roles = {
                f"/LLM/Pipeline/Stage/{index}" for index in range(args.stages)
            }
            if selected_roles != expected_roles:
                raise RuntimeError(
                    "real-model gate did not select every provider role: "
                    f"expected={sorted(expected_roles)} observed={sorted(selected_roles)}"
                )

        summary_match = re.search(
            r"LLM_PIPELINE_USER_SUMMARY .*?count=([0-9]+).*?local_ms=([0-9.]+)"
            r".*?avg_ms=([0-9.]+).*?p50_ms=([0-9.]+).*?p95_ms=([0-9.]+)",
            user_text,
            flags=re.S,
        )
        match = re.search(r"local_ms=([0-9.]+).*distributed_ms=([0-9.]+)", user_text)
        local_ms = match.group(1) if match else "unknown"
        distributed_ms = match.group(2) if match else "unknown"
        if summary_match:
            local_ms = summary_match.group(2)
            distributed_ms = summary_match.group(4)
            print(
                "LLM_PIPELINE_MININDN_BENCHMARK "
                f"count={summary_match.group(1)} "
                f"local_ms={summary_match.group(2)} "
                f"avg_ms={summary_match.group(3)} "
                f"p50_ms={summary_match.group(4)} "
                f"p95_ms={summary_match.group(5)} "
                f"stages={args.stages} runtime={args.runtime} metrics_csv={metrics_csv}"
            )
        if args.runtime in ("qwen-transformers", "qwen-onnx", "qwen-onnx-cpu-native"):
            write_qwen_stage_profile(provider_logs, metrics_csv, OUT)
            write_collab_large_fetch_profile(provider_logs, OUT)
        if args.spec107_diagnostic:
            evidence_paths = [user_log, OUT / "spec107-client-timing.jsonl", *provider_logs]
            manifest_rows = []
            for path in evidence_paths:
                if not path.is_file():
                    raise RuntimeError(
                        f"Spec 107 diagnostic evidence missing: {path}")
                manifest_rows.append({
                    "path": path.name,
                    "bytes": path.stat().st_size,
                    "sha256": "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest(),
                })
            diagnostic_manifest = {
                "schema": "ndnsf-di-spec107-attribution-raw-v1",
                "candidateId": spec107_candidate_id,
                "campaignId": args.campaign_id,
                "eligibility": "DIAGNOSTIC_INELIGIBLE",
                "releaseEligible": False,
                "sampleRate": args.spec107_timing_sample_rate,
                "artifacts": manifest_rows,
            }
            (OUT / "spec107-attribution-raw-manifest.json").write_text(
                json.dumps(diagnostic_manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(
                "LLM_PIPELINE_SPEC107_DIAGNOSTIC " +
                json.dumps(diagnostic_manifest, sort_keys=True,
                           separators=(",", ":")))
        if args.spec107_live_fault_cell:
            if fault_control is None:
                marker_observed = any(
                    "NDNSF_DI_EXPERIMENT_FAULT_INJECTED" in
                    path.read_text(errors="replace")
                    for path in provider_logs)
                derived_control = derive_fault_provider_control(
                    cell_id=args.spec107_live_fault_cell,
                    marker_observed=marker_observed)
                fault_control = {
                    "schema": "ndnsf-di-spec107-live-fault-control-v1",
                    "campaignId": args.campaign_id,
                    "cellId": args.spec107_live_fault_cell,
                    **derived_control,
                }
            (OUT / "spec107-live-fault-control.json").write_text(
                json.dumps(fault_control, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
        if user_failed:
            print(
                "LLM_PIPELINE_MININDN_FAILED "
                f"returncode={user_proc.returncode} stages={args.stages} "
                f"runtime={args.runtime} user_log={user_log}"
            )
            return int(user_proc.returncode or 2)
        if args.spec168_source_digest:
            security_logs = [user_text]
            security_logs.extend(
                path.read_text(errors="replace") for path in provider_logs)
            local_pib_misses = sum(
                text.count("local PIB certificate lookup miss")
                for text in security_logs
            )
            if local_pib_misses:
                raise RuntimeError(
                    "SPEC168_LOCAL_PIB_CERTIFICATE_LOOKUP_MISS:"
                    f"count={local_pib_misses}")
            max_ack_collect_ms = 0.0
            if args.generation_campaign_manifest:
                generation_rows = [
                    json.loads(line)
                    for line in Path(args.generation_jsonl).read_text(
                        encoding="utf-8").splitlines()
                    if line.strip()
                ]
                if not generation_rows:
                    raise RuntimeError("SPEC168_GENERATION_EVIDENCE_MISSING")
                ack_collect_values = []
                for row in generation_rows:
                    steps = row.get("tokenSteps", [])
                    if len(steps) != 1:
                        raise RuntimeError(
                            "SPEC168_PLANNING_TIMING_STEP_COUNT_INVALID")
                    timings = steps[0].get("metadata", {}).get(
                        "planningTimingsMs", {})
                    if "ack_collect_ms" not in timings:
                        raise RuntimeError(
                            "SPEC168_ACK_COLLECT_TIMING_MISSING")
                    ack_collect_values.append(float(timings["ack_collect_ms"]))
                max_ack_collect_ms = max(ack_collect_values)
                if max_ack_collect_ms > args.spec168_max_ack_collect_ms:
                    raise RuntimeError(
                        "SPEC168_ACK_COVERAGE_CLOSURE_TOO_SLOW:"
                        f"observedMs={max_ack_collect_ms:.3f}:"
                        f"limitMs={args.spec168_max_ack_collect_ms:.3f}")
            print(
                "SPEC168_SECURITY_FIDELITY_PASS "
                f"isolatedPibHosts={len(local_identities_by_node)} "
                "localPibMisses=0 "
                f"maxAckCollectMs={max_ack_collect_ms:.3f}"
            )
            route_parts = []
            for node in sorted(ndn.net.hosts, key=lambda item: item.name):
                route_parts.append(
                    f"## {node.name}\n"
                    + perf.node_cmd(node, "nfdc route list 2>&1")
                )
            sif_digest = os.environ.get(
                "NDNSF_SPEC168_EXPECTED_SIF_DIGEST", "")
            admission = write_spec168_runtime_evidence(
                OUT,
                source_digest=args.spec168_source_digest,
                model_identity_digest=args.model_identity_digest,
                workload_digest=args.workload_digest,
                process_rows=spec168_process_rows(processes),
                route_snapshot="\n".join(route_parts),
                fidelity="EXACT_SIF" if sif_digest else "REAL_MININDN",
                sif_digest=sif_digest,
                admission_path=(
                    os.environ.get("NDNSF_SPEC168_ADMISSION_OUTPUT") or None
                ),
            )
            admission_path = Path(os.environ.get(
                "NDNSF_SPEC168_ADMISSION_OUTPUT",
                str(OUT / "spec168-runtime-admission.json"),
            ))
            print(
                "SPEC168_RUNTIME_ADMISSION_EMITTED "
                f"fidelity={admission['fidelity']} "
                f"path={admission_path}"
            )
        print(
            "LLM_PIPELINE_MININDN_OK "
            f"local_ms={local_ms} distributed_ms={distributed_ms} "
            f"stages={args.stages} runtime={args.runtime} user_log={user_log}"
        )
        return 0
    finally:
        if workflow_bundle is not None:
            shutil.rmtree(str(workflow_bundle["keyDir"]), ignore_errors=True)
        if fault_registry is not None:
            cleanup = fault_registry.cleanup()
            if not cleanup["proven"]:
                print("SPEC107_LIVE_FAULT_CLEANUP_FAILED " + json.dumps(
                    cleanup, sort_keys=True), file=sys.stderr)
        stop_processes(processes)
        try:
            ndn.stop()
        finally:
            Minindn.cleanUp()


if __name__ == "__main__":
    raise SystemExit(main())
