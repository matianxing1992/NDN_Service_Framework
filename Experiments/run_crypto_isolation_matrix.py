#!/usr/bin/env python3

import argparse
import csv
import json
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
HARNESS = REPO_ROOT / "Experiments" / "NDNSF_NewAPI_Minindn_Perf.py"


MATRIX = [
    {
        "id": "A",
        "label": "1 provider, 100 rps, normal crypto",
        "providers": 1,
        "flags": [],
    },
    {
        "id": "B",
        "label": "3 providers, 100 rps, normal crypto",
        "providers": 3,
        "flags": [],
    },
    {
        "id": "C",
        "label": "3 providers, 100 rps, ACK plaintext diagnostic",
        "providers": 3,
        "flags": ["--diag-plaintext-ack"],
    },
    {
        "id": "D",
        "label": "3 providers, 100 rps, response plaintext diagnostic",
        "providers": 3,
        "flags": ["--diag-plaintext-response"],
    },
    {
        "id": "E",
        "label": "3 providers, 100 rps, ACK + response plaintext diagnostic",
        "providers": 3,
        "flags": ["--diag-plaintext-ack", "--diag-plaintext-response"],
    },
]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the diagnostic-only NDNSF NAC-ABE crypto isolation matrix.")
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--rate-rps", type=float, default=100.0)
    parser.add_argument("--output-dir", default="")
    parser.add_argument("--topology-file", default="")
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument("--ack-timeout-ms", type=int, default=1000)
    parser.add_argument("--max-outstanding", type=int, default=512)
    parser.add_argument("--provider-ready-timeout-seconds", type=int, default=30)
    parser.add_argument("--provider-start-gap-seconds", type=int, default=2)
    parser.add_argument("--post-ready-settle-seconds", type=int, default=2)
    parser.add_argument("--nlsr-converge-seconds", type=int, default=0)
    parser.add_argument("--matrix", default="A,B,C,D,E",
                        help="Comma-separated subset of matrix IDs to run.")
    parser.add_argument("--post-processing-timeout-seconds", type=int, default=90,
                        help="Extra wall-clock budget after workload drain before a run is killed.")
    parser.add_argument("--retry-on-startup-failure", type=int, default=1,
                        help="Retry a run this many times after NFD socket startup failure.")
    parser.add_argument("--extra-arg", action="append", default=[],
                        help="Extra argument passed through to the MiniNDN harness.")
    return parser.parse_args()


def load_json(path):
    with path.open() as f:
        return json.load(f)


def run_shell(command, timeout=30):
    return subprocess.run(
        command,
        cwd=str(REPO_ROOT),
        shell=isinstance(command, str),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout)


def preflight_cleanup(provider_count):
    provider_sockets = " ".join(
        "/run/nfd/provider{}.sock".format(i + 1) for i in range(provider_count))
    subprocess.run(["sudo", "pkill", "-TERM", "-f", "/build/examples/App_"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    subprocess.run(["sudo", "pkill", "-KILL", "-f", "/build/examples/App_"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    command = r"""
set +e
mn -c >/dev/null 2>&1
rm -f /run/nfd/controller.sock /run/nfd/user.sock /run/nfd/router.sock {provider_sockets}
rm -rf /tmp/minindn /tmp/mn-* /tmp/*.nfd.sock /tmp/ndnsf-svs-registration-*.lock \
       /tmp/ndnsf-keychain-init-*.lock /tmp/ndnsf-provider-route-registration-*.lock
ip netns list 2>/dev/null | awk '/^(controller|user|router|{provider_regex})[[:space:]]/ {{print $1}}' | \
  xargs -r -n1 ip netns delete 2>/dev/null
true
""".format(
        provider_sockets=provider_sockets,
        provider_regex="|".join("provider{}".format(i + 1) for i in range(provider_count)))
    result = run_shell(["sudo", "bash", "-lc", command], timeout=45)
    if result.returncode != 0:
        print("cleanup warning: {}".format(result.stdout), flush=True)


def startup_socket_failure(text):
    return (
        ("could not connect to NDN forwarder" in text and
         ".sock" in text and
         "Face register failed" in text) or
        "NFD socket startup failure" in text)


def run_with_retry(command, run_dir, provider_count, timeout_s, retries):
    attempts_dir = run_dir / "matrix-attempts"
    attempts_dir.mkdir(parents=True, exist_ok=True)
    attempt = 0
    while True:
        preflight_cleanup(provider_count)
        env = dict(os.environ)
        # NFD also reads NDN_LOG and aborts on application-style logger specs.
        # Let the child harness set App_* logging via app_env(); do not poison NFD.
        env.pop("NDN_LOG", None)
        try:
            completed = subprocess.run(
                command,
                cwd=str(REPO_ROOT),
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=timeout_s)
        except subprocess.TimeoutExpired as e:
            output = e.stdout or ""
            if isinstance(output, bytes):
                output = output.decode(errors="replace")
            (attempts_dir / "attempt-{}-timeout.log".format(attempt + 1)).write_text(output)
            preflight_cleanup(provider_count)
            raise RuntimeError("run timed out after {}s".format(timeout_s))

        attempt_log = attempts_dir / "attempt-{}.log".format(attempt + 1)
        attempt_log.write_text(completed.stdout or "")
        if completed.returncode == 0:
            return

        if attempt < retries and startup_socket_failure(completed.stdout or ""):
            print("NFD socket startup failure; cleanup and retry once", flush=True)
            attempt += 1
            time.sleep(3)
            continue

        raise RuntimeError(
            "run failed with exit status {}; see {}".format(
                completed.returncode, attempt_log))


def flatten_crypto(summary):
    crypto = summary.get("crypto_diagnostics", {}).get("summary", {})
    result = {}
    for key, stats in crypto.items():
        prefix = "crypto_{}".format(key.replace(".", "_"))
        result["{}_count".format(prefix)] = stats.get("total_count", 0)
        result["{}_success".format(prefix)] = stats.get("success_count", 0)
        result["{}_failure".format(prefix)] = stats.get("failure_count", 0)
        result["{}_p50_us".format(prefix)] = stats.get("p50_duration_us", 0.0)
        result["{}_p95_us".format(prefix)] = stats.get("p95_duration_us", 0.0)
        result["{}_p99_us".format(prefix)] = stats.get("p99_duration_us", 0.0)
        result["{}_throughput_s".format(prefix)] = stats.get("throughput_per_second", 0.0)
    return result


def write_report(output_dir, rows):
    csv_path = output_dir / "crypto_isolation_matrix.csv"
    base_fields = [
        "id", "label", "providers", "plaintext_ack", "plaintext_response",
        "success_rate", "sent_count", "completed_count", "timed_out_count",
        "ack_published", "ack_processed", "selection_sent", "responses_received",
        "actual_requests_per_second", "actual_successful_responses_per_second",
        "status", "error", "summary_json",
    ]
    crypto_fields = sorted({field for row in rows for field in row if field.startswith("crypto_")})
    with csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=base_fields + crypto_fields)
        writer.writeheader()
        writer.writerows(rows)

    md_path = output_dir / "crypto_isolation_matrix.md"
    lines = [
        "# Diagnostic-Only NAC-ABE Crypto Isolation Matrix",
        "",
        "These runs intentionally use harness-only instrumentation. Plaintext/bypass rows are not security results.",
        "",
        "| ID | Providers | ACK plaintext | Response plaintext | Success rate | Sent | Completed | Timeouts | Actual rps | Success rps |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            "| {id} | {providers} | {plaintext_ack} | {plaintext_response} | "
            "{success_rate:.2%} | {sent_count} | {completed_count} | "
            "{timed_out_count} | {actual_requests_per_second:.2f} | "
            "{actual_successful_responses_per_second:.2f} |".format(**row))
    lines.extend([
        "",
        "Per-stage crypto p50/p95/p99/throughput details are in `crypto_isolation_matrix.csv` and each run's `crypto_diagnostics_summary.json`.",
    ])
    md_path.write_text("\n".join(lines) + "\n")
    return csv_path, md_path


def main():
    args = parse_args()
    selected = {item.strip().upper() for item in args.matrix.split(",") if item.strip()}
    output_dir = Path(args.output_dir) if args.output_dir else (
        REPO_ROOT / "results" / "crypto_isolation_matrix_{}".format(
            datetime.now().strftime("%Y%m%d_%H%M%S")))
    output_dir.mkdir(parents=True, exist_ok=True)

    rows = []
    for item in MATRIX:
        if item["id"] not in selected:
            continue
        run_dir = output_dir / "{}_{}".format(item["id"], item["label"].split(",", 1)[0].replace(" ", "_"))
        command = [
            sys.executable,
            str(HARNESS),
            "--crypto-diagnostics",
            "--workload-mode", "open-loop",
            "--strategy", "custom-selection",
            "--rate-rps", str(args.rate_rps),
            "--duration", str(args.duration),
            "--providers", str(item["providers"]),
            "--output-dir", str(run_dir),
            "--timeout-ms", str(args.timeout_ms),
            "--request-timeout-ms", str(args.timeout_ms),
            "--ack-timeout-ms", str(args.ack_timeout_ms),
            "--max-outstanding", str(args.max_outstanding),
            "--provider-ready-timeout-seconds", str(args.provider_ready_timeout_seconds),
            "--provider-start-gap-seconds", str(args.provider_start_gap_seconds),
            "--post-ready-settle-seconds", str(args.post_ready_settle_seconds),
            "--nlsr-converge-seconds", str(args.nlsr_converge_seconds),
            "--skip-post-run-diagnostics",
        ]
        if args.topology_file:
            command.extend(["--topology-file", args.topology_file])
        command.extend(item["flags"])
        command.extend(args.extra_arg)

        print("Running matrix {}: {}".format(item["id"], item["label"]), flush=True)
        print(" ".join(command), flush=True)
        timeout_s = int(
            120 + args.duration + args.timeout_ms / 1000.0 +
            args.ack_timeout_ms / 1000.0 + args.post_processing_timeout_seconds)
        try:
            run_with_retry(
                command,
                run_dir,
                item["providers"],
                timeout_s,
                args.retry_on_startup_failure)
        except Exception as e:
            row = {
                "id": item["id"],
                "label": item["label"],
                "providers": item["providers"],
                "plaintext_ack": "--diag-plaintext-ack" in item["flags"],
                "plaintext_response": "--diag-plaintext-response" in item["flags"],
                "success_rate": 0.0,
                "sent_count": 0,
                "completed_count": 0,
                "timed_out_count": 0,
                "ack_published": 0,
                "ack_processed": 0,
                "selection_sent": 0,
                "responses_received": 0,
                "actual_requests_per_second": 0.0,
                "actual_successful_responses_per_second": 0.0,
                "status": "failed",
                "error": str(e),
                "summary_json": "",
            }
            rows.append(row)
            write_report(output_dir, rows)
            print("Matrix {} failed: {}".format(item["id"], e), flush=True)
            continue

        summary_path = run_dir / "summary.json"
        if not summary_path.exists():
            row = {
                "id": item["id"],
                "label": item["label"],
                "providers": item["providers"],
                "plaintext_ack": "--diag-plaintext-ack" in item["flags"],
                "plaintext_response": "--diag-plaintext-response" in item["flags"],
                "success_rate": 0.0,
                "sent_count": 0,
                "completed_count": 0,
                "timed_out_count": 0,
                "ack_published": 0,
                "ack_processed": 0,
                "selection_sent": 0,
                "responses_received": 0,
                "actual_requests_per_second": 0.0,
                "actual_successful_responses_per_second": 0.0,
                "status": "failed",
                "error": "summary.json missing after run",
                "summary_json": str(summary_path),
            }
            rows.append(row)
            write_report(output_dir, rows)
            print("Matrix {} failed: summary.json missing".format(item["id"]), flush=True)
            continue
        summary = load_json(summary_path)
        crypto = summary.get("crypto_diagnostics", {}).get("summary", {})
        row = {
            "id": item["id"],
            "label": item["label"],
            "providers": item["providers"],
            "plaintext_ack": "--diag-plaintext-ack" in item["flags"],
            "plaintext_response": "--diag-plaintext-response" in item["flags"],
            "success_rate": float(summary.get("success_rate", 0.0)),
            "sent_count": int(summary.get("sent_count", 0) or 0),
            "completed_count": int(summary.get("completed_count", 0) or 0),
            "timed_out_count": int(summary.get("timed_out_count", 0) or 0),
            "ack_published": int(sum(
                stats.get("total_count", 0)
                for key, stats in crypto.items()
                if key.startswith("provider.ack.encrypt."))),
            "ack_processed": int(sum(
                stats.get("success_count", 0)
                for key, stats in crypto.items()
                if key.startswith("user.ack.decrypt."))),
            "selection_sent": int(sum(
                stats.get("total_count", 0)
                for key, stats in crypto.items()
                if key.startswith("user.selection.encrypt."))),
            "responses_received": int(sum(
                stats.get("success_count", 0)
                for key, stats in crypto.items()
                if key.startswith("user.response.decrypt."))),
            "actual_requests_per_second": float(summary.get("actual_requests_per_second", 0.0)),
            "actual_successful_responses_per_second": float(
                summary.get("actual_successful_responses_per_second", 0.0)),
            "status": "ok",
            "error": "",
            "summary_json": str(summary_path),
        }
        row.update(flatten_crypto(summary))
        rows.append(row)
        write_report(output_dir, rows)

    csv_path, md_path = write_report(output_dir, rows)
    print("matrix_csv={}".format(csv_path))
    print("matrix_report={}".format(md_path))


if __name__ == "__main__":
    main()
