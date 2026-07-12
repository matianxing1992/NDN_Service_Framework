#!/usr/bin/env python3
"""MiniNDN smoke for distributed validation LLM pipeline inference."""

from __future__ import annotations

import argparse
import csv
import json
import os
import pwd
import re
import signal
import statistics
import site
import subprocess
import sys
import time
from pathlib import Path

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
OUT = REPO / "results/llm_pipeline_minindn_smoke"
LLM_DIR = REPO / "examples/python/NDNSF-DistributedInference/llm_pipeline"
CONFIG = OUT / "llm_pipeline_policy.yaml"
GEN_POLICY = "/tmp/ndnsf-di-llm-pipeline-minindn-policy"
APP_ROOT = "/example/llm-pipeline"
CONTROLLER_IDENTITY = APP_ROOT + "/controller"
GROUP_IDENTITY = APP_ROOT + "/group"
USER_IDENTITY = APP_ROOT + "/user"
PROVIDER_PREFIX = APP_ROOT + "/provider"
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
    parser.add_argument("--qwen-allow-download", action="store_true")
    parser.add_argument("--qwen-dtype", choices=("float32", "auto"), default="float32")
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
    parser.add_argument("--controller-wait-s", type=float, default=8.0)
    parser.add_argument("--provider-wait-s", type=float, default=10.0)
    parser.add_argument("--provider-start-timeout-s", type=float, default=20.0)
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=60000)
    parser.add_argument("--ndn-log", default="ndn_service_framework.*=INFO")
    parser.add_argument("--prompt", default="Explain NDNSF-DI pipeline inference.")
    parser.add_argument("--warmup-requests", type=int, default=0)
    parser.add_argument("--measured-requests", type=int, default=1)
    parser.add_argument("--max-new-tokens", type=int, default=1)
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
    return parser


def python_path_entries() -> list[str]:
    entries = [
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "pythonWrapper"),
        str(REPO / "NDNSF-DistributedRepo/pythonWrapper"),
        str(LLM_DIR),
        str(REPO / "Experiments"),
        site.getusersitepackages(),
    ]
    sudo_user = os.environ.get("SUDO_USER")
    if sudo_user:
        try:
            sudo_home = pwd.getpwnam(sudo_user).pw_dir
            version = f"python{sys.version_info.major}.{sys.version_info.minor}"
            entries.append(str(Path(sudo_home) / ".local/lib" / version / "site-packages"))
        except KeyError:
            pass
    if os.environ.get("PYTHONPATH"):
        entries.append(os.environ["PYTHONPATH"])
    deduped = []
    for entry in entries:
        if entry and entry not in deduped:
            deduped.append(entry)
    return deduped


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
                   qwen_prompt: str = "",
                   qwen_allow_download: bool = False,
                   qwen_dtype: str = "float32") -> None:
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
        "--qwen-prompt", qwen_prompt,
        "--qwen-dtype", qwen_dtype,
        *(["--qwen-allow-download"] if qwen_allow_download else []),
    ], cwd=str(REPO), env=policy_env, check=True)
    config = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
    config.setdefault("trust", {})["app_roots"] = [APP_ROOT]
    CONFIG.write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")


def generate_policy_bundle(env: dict[str, str]) -> None:
    subprocess.run([
        sys.executable,
        "-c",
        "from ndnsf_distributed_inference.policy import main; raise SystemExit(main())",
        "--config", str(CONFIG),
        "--out-dir", str(GEN_POLICY),
        "--print-summary",
    ], cwd=str(REPO), env=env, check=True)


def write_native_qwen_bundle(out_dir: Path) -> tuple[Path, Path]:
    manifest_source = json.loads(
        (out_dir / "qwen-onnx-service-manifest.json").read_text(encoding="utf-8"))
    roles = [str(stage["role"]) for stage in manifest_source["stages"]]
    dependencies = []
    manifest_dependencies = []
    for index in range(2):
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
            "executionProvider": "cpu",
            "allowCpuFallback": "false",
            "passthroughTensors": ",".join(passthrough),
            "kvTensorMap": ",".join(
                f"{input_name}={output_name}"
                for input_name, output_name in zip(
                    stage["cacheInputs"], stage["cacheOutputs"])
            ),
            "kvOutputTensors": ",".join(stage["cacheOutputs"]),
            "kvOutputScope": "kv-state",
            "outputBundleScope": (
                "final-response" if index == 2 else f"pipeline-stage-{index}-to-{index + 1}"),
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
    plan = {"version": 2, "services": [{
        "schemaVersion": 2, "service": SERVICE,
        "model": "/Model/Qwen2.5-0.5B-Instruct", "modelFamily": "llm",
        "modelFormat": "onnx", "plannerKind": "llm-pipeline",
        "runtimeBackend": "onnxruntime", "roles": roles,
        "dependencies": dependencies,
    }]}
    service_manifest = {"services": [{
        "name": SERVICE, "model": "/Model/Qwen2.5-0.5B-Instruct",
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


def main() -> int:
    global OUT, CONFIG
    args = build_parser().parse_args()
    if args.stages != 3:
        raise SystemExit("this MiniNDN smoke currently maps exactly 3 stages")
    if args.runtime == "tiny-transformers" and args.layers == 24:
        args.layers = args.transformer_layers
    sys.argv = [sys.argv[0]]
    setLogLevel("info")
    OUT = Path(args.output_dir).expanduser().resolve()
    CONFIG = OUT / "llm_pipeline_policy.yaml"
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
        policy_runtime = "qwen-onnx" if args.runtime == "qwen-onnx-cpu-native" else args.runtime
        prepare_policy(
            args.stages,
            args.layers,
            runtime=policy_runtime,
            transformer_layers=args.transformer_layers,
            qwen_model=args.qwen_model,
            qwen_prompt=args.prompt,
            qwen_allow_download=args.qwen_allow_download,
            qwen_dtype=args.qwen_dtype,
        )
    base_env = {
        **os.environ,
        "PYTHONFAULTHANDLER": "1",
        "PYTHONUNBUFFERED": "1",
        "PYTHONPATH": ":".join(python_path_entries()),
        "NDN_LOG": args.ndn_log,
        "NDNSF_RESPONSE_LARGE_DATA_THRESHOLD": "1024",
    }
    if args.large_fetch_timing:
        base_env["NDNSF_COLLAB_LARGE_FETCH_TIMING"] = "1"
    if args.runtime == "qwen-onnx-cpu-native":
        base_env["NDNSF_DI_RUNTIME_TIMING"] = "1"
    base_env.pop("NDN_CLIENT_TRANSPORT", None)
    generate_policy_bundle(base_env)
    native_plan = native_manifest = None
    if args.runtime == "qwen-onnx-cpu-native":
        native_plan, native_manifest = write_native_qwen_bundle(OUT)

    subprocess.run(["pkill", "-f", "llm_pipeline/(provider|user)\\.py"],
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
        for node_name, identity in zip(STAGE_NODES, STAGE_IDENTITIES):
            rh.addOrigin([ndn.net[node_name]], [identity, identity + "/KEY"])
        rh.addOrigin(ndn.net.hosts, [GROUP_IDENTITY])
        rh.calculateRoutes()
        log(f"Waiting {args.nlsr_wait_s:.1f}s for NLSR convergence")
        time.sleep(args.nlsr_wait_s)
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, APP_ROOT, Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, GROUP_IDENTITY, Nfdc.STRATEGY_MULTICAST)

        node_identities = [
            (CONTROLLER_NODE, CONTROLLER_IDENTITY),
            (USER_NODE, USER_IDENTITY),
            *list(zip(STAGE_NODES, STAGE_IDENTITIES)),
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

        provider_logs = []
        for stage_index, (node_name, provider_id) in enumerate(zip(STAGE_NODES, STAGE_PROVIDER_IDS)):
            provider_id_arg = (
                f" --provider-id {provider_id} "
                if provider_id else
                " "
            )
            if args.runtime == "qwen-onnx-cpu-native":
                if native_plan is None or native_manifest is None:
                    raise RuntimeError("native Qwen plan/manifest were not generated")
                provider_command = (
                    f"cd {perf.shell_quote(REPO)} && exec "
                    f"{perf.shell_quote(REPO / 'build/examples/di-native-provider')} "
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
                    f"--compute-delay-ms {args.compute_delay_ms}"
                )
                ready_marker = "LLM_PIPELINE_PROVIDER_READY"
            proc, log_path = start_process(
                ndn, node_name, f"stage{stage_index}-provider",
                provider_command, node_env[node_name], processes,
            )
            provider_logs.append(log_path)
            if not wait_log(log_path, ready_marker, args.provider_start_timeout_s, proc):
                raise RuntimeError(f"stage provider did not start; log={log_path}")
        log(f"Waiting {args.provider_wait_s:.1f}s for providers")
        time.sleep(args.provider_wait_s)

        user_log = OUT / "llm-pipeline-user.log"
        metrics_csv = OUT / "llm-pipeline-user-measured.csv"
        user_out = user_log.open("wb")
        user_runtime = "qwen-onnx" if args.runtime == "qwen-onnx-cpu-native" else args.runtime
        native_user_args = (
            "--native-cpu-provider --qwen-service-manifest {}".format(
                perf.shell_quote(OUT / "qwen-onnx-service-manifest.json"))
            if args.runtime == "qwen-onnx-cpu-native" else ""
        )
        user_proc = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(LLM_DIR / "user.py") + common +
            " --prompt {} --stages {} --compute-delay-ms {} "
            "--runtime {} --transformer-layers {} "
            "--qwen-runtime-summary {} "
            "--context-input-mode {} --delta-token-ids {} "
            "--ack-timeout-ms {} --timeout-ms {} "
            "--warmup-requests {} --measured-requests {} "
            "--max-new-tokens {} "
            "--native-first-kv-mode {} "
            "--expected-token-ids {} "
            "--measured-duration-s {} --request-interval-ms {} --campaign-id {} "
            "--metrics-csv {} {} {}".format(
                perf.shell_quote(args.prompt),
                args.stages,
                args.compute_delay_ms,
                user_runtime,
                args.transformer_layers,
                perf.shell_quote(OUT / "qwen-pipeline-runtime.json"),
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
                "--publish-input-reference" if args.publish_input_reference else "",
                native_user_args,
            ),
            envDict=node_env[USER_NODE],
            shell=True,
            stdout=user_out,
            stderr=subprocess.STDOUT,
        )
        processes.append((user_proc, user_out, user_log))
        user_proc.wait(timeout=max(
            180.0,
            args.measured_duration_s + args.timeout_ms / 1000.0 + 30.0,
        ))
        user_text = user_log.read_text(errors="replace")
        print(user_text)
        expected_user_marker = (
            "LLM_PIPELINE_OPEN_LOOP_SUMMARY"
            if args.runtime == "qwen-onnx-cpu-native" and args.measured_duration_s > 0 else
            "LLM_PIPELINE_USER_RESPONSE"
        )
        if expected_user_marker not in user_text:
            raise RuntimeError(f"LLM pipeline user failed; log={user_log}")
        user_failed = user_proc.returncode != 0
        for stage_index, log_path in enumerate(provider_logs):
            text = log_path.read_text(errors="replace")
            expected = (
                "NDNSF_DI_ONNX_TIMING"
                if args.runtime == "qwen-onnx-cpu-native" else
                "LLM_PIPELINE_QWEN_ONNX_STAGE_FINAL"
                if args.runtime == "qwen-onnx" and stage_index == args.stages - 1 else
                "LLM_PIPELINE_QWEN_ONNX_STAGE_OUTPUT"
                if args.runtime == "qwen-onnx" else
                "LLM_PIPELINE_QWEN_STAGE_FINAL"
                if args.runtime == "qwen-transformers" and stage_index == args.stages - 1 else
                "LLM_PIPELINE_QWEN_STAGE_OUTPUT"
                if args.runtime == "qwen-transformers" else
                "LLM_PIPELINE_TRANSFORMER_STAGE_FINAL"
                if args.runtime == "tiny-transformers" and stage_index == args.stages - 1 else
                "LLM_PIPELINE_TRANSFORMER_STAGE_OUTPUT"
                if args.runtime == "tiny-transformers" else
                "LLM_PIPELINE_STAGE_FINAL"
                if stage_index == args.stages - 1 else
                "LLM_PIPELINE_STAGE_OUTPUT"
            )
            if expected not in text:
                raise RuntimeError(f"stage {stage_index} missing {expected}; log={log_path}")

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
        if user_failed:
            print(
                "LLM_PIPELINE_MININDN_FAILED "
                f"returncode={user_proc.returncode} stages={args.stages} "
                f"runtime={args.runtime} user_log={user_log}"
            )
            return int(user_proc.returncode or 2)
        print(
            "LLM_PIPELINE_MININDN_OK "
            f"local_ms={local_ms} distributed_ms={distributed_ms} "
            f"stages={args.stages} runtime={args.runtime} user_log={user_log}"
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
