#!/usr/bin/env python3
"""Validate and summarize the matched fixed-provider mobility reruns."""

import argparse
import csv
import hashlib
import json
import random
import re
from pathlib import Path


SYSTEMS = ("ndnsf", "grpc-single", "nsc-single")
RADII = (100, 150, 200)
DEADLINE_MS = 5000

RESULT_LOGS = {
    "ndnsf": ("ndnsf-user.log",
              r"INTERMITTENT_USER_REQUEST_RESULT[^\n]*status=SUCCESS latency_ms=([0-9.]+)"),
    "grpc-single": ("grpc-client.log",
                    r"GRPC_FAILOVER_ATTEMPT[^\n]*status=OK latency_ms=([0-9.]+)"),
    "nsc-single": ("nsc-consumer.log",
                   r"NSC_REQUEST_RESULT[^\n]*status=SUCCESS latency_ms=([0-9.]+)"),
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[int((len(ordered) - 1) * fraction)]


def paired_interval(differences, repetitions=10000):
    rng = random.Random(20621)
    means = [sum(rng.choice(differences) for _ in differences) / len(differences)
             for _ in range(repetitions)]
    return [percentile(means, 0.025), percentile(means, 0.975)]


def coverage(path):
    epochs = {}
    with path.open(newline="") as source:
        for row in csv.DictReader(source):
            second = int(float(row["time_s"]))
            if second < 120:
                epochs.setdefault(second, {})[row["provider"]] = int(row["in_range"])
    if sorted(epochs) != list(range(120)):
        raise ValueError(f"coverage trace does not contain 120 epochs: {path}")
    providers = sorted({provider for state in epochs.values() for provider in state})
    if len(providers) != 3 or any(set(state) != set(providers) for state in epochs.values()):
        raise ValueError(f"coverage trace lacks three Providers: {path}")
    return {
        "any_reachable_epochs": sum(any(state.values()) for state in epochs.values()),
        "providers": {
            provider: {
                "reachable_epochs": sum(epochs[second][provider] for second in range(120)),
                "transitions": sum(epochs[second][provider] != epochs[second - 1][provider]
                                   for second in range(1, 120)),
            }
            for provider in providers
        },
    }


def successful_latencies(cell_dir, system):
    filename, pattern = RESULT_LOGS[system]
    content = (cell_dir / filename).read_text(errors="replace")
    return [float(value) for value in re.findall(pattern, content)]


def provider_execution_ids(cell_dir):
    marker = "INTERMITTENT_PROVIDER_REQUEST_EXECUTION "
    request_id = re.compile(r"/20\d{6}T\d{6}\.\d+-[0-9a-fA-F]+")
    count = 0
    ids = []
    interleaved_lines = 0
    for log_path in sorted(cell_dir.glob("ndnsf-provider-*.log")):
        for line in log_path.read_text(errors="replace").splitlines():
            occurrences = line.count(marker)
            if occurrences == 0:
                continue
            found = request_id.findall(line[line.index(marker):])
            if len(found) != occurrences:
                raise ValueError(f"cannot reconstruct Provider markers: {log_path}")
            count += occurrences
            ids.extend(found)
            interleaved_lines += occurrences > 1
    if len(ids) != len(set(ids)):
        raise ValueError(f"duplicate Provider request execution: {cell_dir}")
    return count, set(ids), interleaved_lines


def verify_cell_evidence(cell_dir, manifest_path):
    evidence = json.loads(manifest_path.read_text())
    for item in evidence["files"]:
        relative = Path(item["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"unsafe evidence path: {relative}")
        path = cell_dir / relative
        if path.stat().st_size != item["size_bytes"] or sha256(path) != item["sha256"]:
            raise ValueError(f"evidence content changed: {path}")


def load_campaign(path):
    summary = json.loads((path / "campaign-summary.json").read_text())
    manifest = summary["manifest"]
    config = manifest["configuration"]
    if summary["status"] not in ("passed", "failed") or len(summary["cells"]) != 9:
        raise ValueError(f"campaign incomplete: {path}")
    corrected_parent_cells = 0
    if (config["ranges_m"] != [float(radius) for radius in RADII] or
            config["systems"] != list(SYSTEMS) or
            config["duration_s"] != 120 or config["rate_rps"] != 5 or
            config["global_deadline_ms"] != 5000 or not config["block_network"]):
        raise ValueError(f"campaign conditions changed: {path}")
    cells = {}
    traces = {}
    for cell in summary["cells"]:
        radius = int(cell["range_m"])
        key = radius, cell["system_id"]
        parser_failure = (cell["status"] == "failed" and
                          cell.get("error") ==
                          "parent validation failed: parent NDNSF execution accounting failed" and
                          cell["system_id"] == "ndnsf")
        if (key in cells or radius not in RADII or cell["system_id"] not in SYSTEMS or
                (cell["status"] != "passed" and not parser_failure) or
                cell["sent"] != 600 or
                not 0 <= cell["success"] <= cell["sent"] or
                (cell["success"] > 0 and cell["mean_ms"] <= 0)):
            raise ValueError(f"invalid or duplicate cell: {path} {key}")
        evidence_path = path / "cells" / cell["cell_id"] / "evidence-hashes.json"
        if (cell.get("evidence_manifest_sha256") != sha256(evidence_path) or
                cell.get("evidence_manifest_file") != str(evidence_path)):
            raise ValueError(f"evidence manifest mismatch: {path} {key}")
        verify_cell_evidence(evidence_path.parent, evidence_path)
        if cell["system_id"] == "ndnsf":
            execution_count, execution_ids, interleaved_lines = provider_execution_ids(
                path / "cells" / cell["cell_id"])
            user_log = (path / "cells" / cell["cell_id"] / "ndnsf-user.log").read_text(
                errors="replace")
            accepted_ids = set(re.findall(
                r"INTERMITTENT_USER_REQUEST_RESULT[^\n]*requestId=(/[^ ]+) status=SUCCESS",
                user_log))
            if (len(accepted_ids) != cell["success"] or
                    not accepted_ids <= execution_ids or
                    not cell["provider_executions"] <= execution_count <= cell["sent"] or
                    cell["unique_request_ids_executed"] != cell["provider_executions"]):
                raise ValueError(f"NDNSF execution evidence mismatch: {path} {key}")
            cell["provider_executions_reconstructed"] = execution_count
            cell["provider_marker_interleaved_lines"] = interleaved_lines
            if parser_failure:
                if (execution_count <= cell["provider_executions"] or
                        interleaved_lines == 0 or execution_count < cell["success"]):
                    raise ValueError(f"unexplained parent parser failure: {path} {key}")
                corrected_parent_cells += 1
            cell["parent_parser_failure_corrected"] = parser_failure
        cell["successful_latencies"] = successful_latencies(
            path / "cells" / cell["cell_id"], cell["system_id"])
        if len(cell["successful_latencies"]) != cell["success"]:
            raise ValueError(f"success log and summary disagree: {path} {key}")
        cells[key] = cell
        traces.setdefault(radius, set()).add(cell["trace_sha256"])
    if set(cells) != {(radius, system) for radius in RADII for system in SYSTEMS}:
        raise ValueError(f"missing cells: {path}")
    if (summary["status"] == "failed") != (corrected_parent_cells == 1):
        raise ValueError(f"unexplained parent campaign status: {path}")
    for radius in RADII:
        trace = path / "traces" / f"range_{radius}.csv"
        if len(traces[radius]) != 1 or sha256(trace) not in traces[radius]:
            raise ValueError(f"trace mismatch: {path} {radius}")
    return config, manifest["source_hashes"], cells


def analyze(paths):
    campaigns = {}
    source_versions = {}
    reference_config = None
    for path in paths:
        config, hashes, cells = load_campaign(path)
        seed = config["seed"]
        if seed in campaigns:
            raise ValueError(f"duplicate seed {seed}")
        comparable = {key: value for key, value in config.items() if key != "seed"}
        if reference_config is not None and comparable != reference_config:
            raise ValueError(f"different campaign settings at seed {seed}")
        reference_config = comparable
        campaigns[seed] = (path, cells)
        for item in hashes:
            identity = item["kind"], Path(item["path"]).name
            source_versions.setdefault(identity, {})[seed] = item["sha256"]
    if set(campaigns) != {20, 21, 22, 23, 24}:
        raise ValueError("the five prespecified seeds are required")
    for (kind, name), versions in source_versions.items():
        if set(versions) != set(campaigns):
            raise ValueError(f"missing provenance entry: {kind}:{name}")
        if len(set(versions.values())) > 1 and (kind, name) != (
                "source", "NDNSF_NewAPI_Minindn_Perf.py"):
            raise ValueError(f"source or binary changed across seeds: {kind}:{name}")
        if len(set(versions.values())) > 2:
            raise ValueError(f"unexpected helper source versions: {name}")
    rows = []
    for seed, (path, cells) in sorted(campaigns.items()):
        for radius in RADII:
            trace = coverage(path / "traces" / f"range_{radius}.csv")
            for system in SYSTEMS:
                cell = cells[radius, system]
                timely_latencies = [value for value in cell["successful_latencies"]
                                    if value <= DEADLINE_MS]
                success = len(timely_latencies)
                rows.append({
                    "seed": seed,
                    "radius_m": radius,
                    "system": system,
                    "sent": cell["sent"],
                    "success": success,
                    "success_rate": success / cell["sent"],
                    "reported_success": cell["success"],
                    "accepted_after_deadline": cell["success"] - success,
                    "ndnsf_executions_without_timely_result":
                        cell["provider_executions_reconstructed"] - success
                        if system == "ndnsf" else None,
                    "ndnsf_provider_markers_reported":
                        cell["provider_executions"] if system == "ndnsf" else None,
                    "ndnsf_provider_markers_reconstructed":
                        cell["provider_executions_reconstructed"]
                        if system == "ndnsf" else None,
                    "ndnsf_interleaved_marker_lines":
                        cell["provider_marker_interleaved_lines"]
                        if system == "ndnsf" else None,
                    "parent_parser_failure_corrected":
                        cell.get("parent_parser_failure_corrected", False),
                    "successful_mean_ms": sum(timely_latencies) / success if success else None,
                    "deadline_penalized_mean_ms":
                        (sum(timely_latencies) +
                         (cell["sent"] - success) * DEADLINE_MS) /
                        cell["sent"],
                    "coverage": trace,
                    "trace_sha256": cell["trace_sha256"],
                })
    contrasts = []
    measured = {(row["seed"], row["radius_m"], row["system"]): row for row in rows}
    for radius in RADII:
        for baseline in SYSTEMS[1:]:
            differences = []
            for seed in sorted(campaigns):
                differences.append(
                    measured[seed, radius, "ndnsf"]["success_rate"] -
                    measured[seed, radius, baseline]["success_rate"])
            contrasts.append({
                "radius_m": radius,
                "baseline": baseline,
                "paired_success_rate_difference": sum(differences) / len(differences),
                "seed_bootstrap_95_interval": paired_interval(differences),
            })
    return {
        "schema": "ndnsf-historical-wireless-rerun-analysis-v1",
        "analyzer_sha256": sha256(Path(__file__).resolve()),
        "strict_deadline_ms": DEADLINE_MS,
        "seed_count": len(campaigns),
        "campaign_paths": {str(seed): str(path) for seed, (path, _) in campaigns.items()},
        "source_campaign_statuses": {
            str(seed): ("failed_parent_parser_corrected"
                        if any(cell.get("parent_parser_failure_corrected", False)
                               for cell in cells.values()) else "passed")
            for seed, (_, cells) in campaigns.items()
        },
        "source_versions": {f"{kind}:{name}": versions
                            for (kind, name), versions in sorted(source_versions.items())},
        "rows": rows,
        "contrasts": contrasts,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign-dir", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.campaign_dir)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(f"analyzed {result['seed_count']} seeds -> {args.output}")


if __name__ == "__main__":
    main()
