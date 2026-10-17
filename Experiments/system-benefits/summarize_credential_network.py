#!/usr/bin/env python3
"""Requalify all 90 cells before summarizing paired permission/certificate differences."""
import argparse
import itertools
import json
from pathlib import Path
import statistics
from analyze_credential_network import analyze, require
from run_credential_network import ARMS
from credential_network_inputs import digest, verify_qualification

METRICS = ("mean_recipient_management_ms", "management_cpu_ms", "management_ip_bytes",
           "management_data_bytes", "management_interests", "certificate_interests", "certificate_data_bytes")


def estimate(values):
    require(len(values) == 5, "five paired blocks required")
    mean = statistics.mean(values)
    half = 2.776445105 * statistics.stdev(values) / (5**.5)
    return {"mean": mean, "ci95": [mean-half, mean+half], "blocks": 5}


def summarize(directory):
    manifest = json.loads((directory / "manifest.json").read_text())
    require(manifest["schema"] == "ndnsf-credential-network-campaign-v1" and manifest["status"] == "CAMPAIGN_COMPLETE", "incomplete campaign")
    expected = set(itertools.product(ARMS, (1, 4, 16), range(1, 6), ("reuse", "cold")))
    def key(cell):
        return cell["arm"], cell["permissions"], cell["repetition"], cell["cache"]
    require(len(manifest["cells"]) == len(manifest["matrix"]) == 90 and
            {key(c) for c in manifest["cells"]} == {key(c) for c in manifest["matrix"]} == expected, "full comparison matrix")
    receipt = Path(manifest["qualification"])
    require(digest(receipt) == manifest["qualification_sha256"], "qualification receipt drift")
    qualification = json.loads(receipt.read_text())
    verify_qualification(Path(qualification["archive"]), 5, receipt)
    frozen = {k: v for k, v in manifest["candidate"].items() if k != "campaign_sources"}
    by_cell = {}
    for cell in manifest["cells"]:
        own = directory / cell["id"]
        native = json.loads((own / "manifest.json").read_text())
        require(native["candidate"] == frozen, "mixed candidate")
        require(native["archive"] == qualification["archive"] and native["archive_repetitions"] == 5 and
                native["qualification_sha256"] == manifest["qualification_sha256"] and
                digest(own / "qualification.json") == manifest["qualification_sha256"], "mixed archive/qualification invalidates pairing")
        result = analyze(own)
        require(result["qualified"] and not result["smoke"] and key(result) == key(cell), "cell profile drift")
        require(result == json.loads((directory / cell["summary"]).read_text()), "saved summary differs from raw recomputation")
        by_cell[key(cell)] = {r["epoch"]: r for r in result["phases"]}
    costs, aggregation, reuse = [], [], []
    for arm, permissions, cache, epoch in itertools.product(ARMS, (1, 4, 16), ("reuse", "cold"), (1, 2, 3)):
        values = [by_cell[(arm, permissions, r, cache)][epoch] for r in range(1, 6)]
        costs.append({"arm": arm, "permissions": permissions, "cache": cache, "epoch": epoch,
                      "metrics": {metric: estimate([v[metric] for v in values]) for metric in METRICS}})
    for permissions, epoch, alternative in itertools.product((1, 4, 16), (1, 2, 3), ARMS[1:]):
        aggregation.append({"permissions": permissions, "epoch": epoch, "cache": "reuse", "difference": "aggregated-minus-" + alternative,
            "metrics": {metric: estimate([by_cell[(ARMS[0], permissions, r, "reuse")][epoch][metric] -
                                          by_cell[(alternative, permissions, r, "reuse")][epoch][metric] for r in range(1, 6)])
                        for metric in METRICS}})
    for arm, permissions, epoch in itertools.product(ARMS, (1, 4, 16), (1, 2, 3)):
        reuse.append({"arm": arm, "permissions": permissions, "epoch": epoch, "difference": "cold-minus-reuse",
            "metrics": {metric: estimate([by_cell[(arm, permissions, r, "cold")][epoch][metric] -
                                          by_cell[(arm, permissions, r, "reuse")][epoch][metric] for r in range(1, 6)])
                        for metric in METRICS}})
    return {"schema": "ndnsf-credential-network-summary-v1", "qualified_cells": 90,
            "scope": "comparison transport event costs, not production Authority recovery or a sequential update break-even",
            "cpu_scope": "sum of recipient process CPU through management installation, excluding probes; do not add to prior component roundtrip CPU",
            "network_scope": "all three physical-link IPv4 bytes, both directions, management-only intervals; NFD caching allowed",
            "cache_scope": "application certificate reuse ablation shared by all arms; NFD certificate caches stay enabled",
            "statistics": "equal-weight five-block means and paired t4 intervals; same fresh cryptographic input for each cache pair",
            "costs": costs, "aggregation_differences": aggregation, "reuse_differences": reuse}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.directory), indent=2))
