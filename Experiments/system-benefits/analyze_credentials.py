#!/usr/bin/env python3
"""Qualify one complete native component run; never pool smoke/formal runs."""
import argparse
import hashlib
import json
import math
import statistics
from pathlib import Path

ARMS = ("AGGREGATED_ABE", "PERMISSION_ABE", "BATCHED_RSA")
PHASES = ("initial", "grant", "revoke")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def estimate(values):
    mean = statistics.mean(values)
    # Matched repetitions are the units. For the preregistered n=5, t_4=.975.
    half = 2.776445 * statistics.stdev(values) / math.sqrt(5) if len(values) == 5 else None
    return {"n": len(values), "mean": mean, "ci95_half_width": half}


def analyze(path, repetitions):
    raw = path.read_bytes()
    rows = [json.loads(line) for line in raw.splitlines() if line.strip()]
    require(not any(r.get("archive_export",False) for r in rows),
            "archive export includes file I/O; not qualified component timing")
    require(len(rows) == repetitions * 3 * 3 * 6 + 2, "incomplete/extra rows")
    bootstrap = [r for r in rows if r.get("event") == "identity_bootstrap"]
    complete = [r for r in rows if r.get("event") == "complete"]
    require(len(bootstrap) == len(complete) == 1, "bootstrap/complete cardinality")
    require(rows[-1] == complete[0], "complete must be final record")
    require(complete[0].get("oracle") is True and complete[0]["repetitions"] == repetitions,
            "completion oracle/repetitions")
    require(bootstrap[0]["users"] == 8 and bootstrap[0]["negative_principals"] == 1
            and bootstrap[0]["rsa_bits"] == 2048 and bootstrap[0]["shared_by_all_arms"] is True,
            "identity profile mismatch")
    costs, accesses = {}, {}
    for row in rows:
        if "event" in row:
            continue
        require(row.get("oracle") is True, "native oracle failed")
        require(row["arm"] in ARMS and row["permissions"] in (1, 4, 16)
                and 1 <= row["repetition"] <= repetitions, "unexpected cell")
        for key, value in row.items():
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                require(math.isfinite(value) and value >= 0, "invalid numeric field " + key)
        prefix = (row["arm"], row["permissions"], row["repetition"])
        if row["schema"] == "ndnsf-system-cost-v1":
            require(row["phase"] in PHASES and row["users"] == 8, "phase/user profile")
            key = prefix + (row["phase"],)
            require(key not in costs, "duplicate management cell")
            expected = 1 if row["phase"] == "grant" else (8 if row["phase"] == "initial" else 7)
            if row["arm"] == "PERMISSION_ABE" and row["phase"] != "grant":
                expected *= row["permissions"]
            require(row["management_objects"] == expected, "wrong material object count")
            require(row["management_packets"] >= expected and row["management_wire_bytes"] > 0,
                    "missing signed management wire")
            require((row["public_parameter_wire_bytes_once"] == 0)
                    == (row["arm"] == "BATCHED_RSA" or row["phase"] == "grant"), "PP profile")
            costs[key] = row
        else:
            require(row["schema"] == "ndnsf-system-access-v1", "unknown schema")
            require(row["epoch"] in (1, 2, 3) and row["plain_bytes"] == 64
                    and row["warm_repeated_accesses"] == 100, "access profile")
            key = prefix + (row["epoch"],)
            require(key not in accesses, "duplicate access cell")
            require(row["business_wire_bytes"] > 64, "missing business Data wire")
            require((row["content_key_wire_bytes_once"] == 0) == (row["arm"] == "BATCHED_RSA"),
                    "missing/extra separately signed wrapped CK")
            accesses[key] = row
    require(len(costs) == len(accesses) == repetitions * 27, "incomplete matrix")
    summary = []
    for p in (1, 4, 16):
        for phase in PHASES:
            group = {a: [costs[(a, p, r, phase)] for r in range(1, repetitions + 1)] for a in ARMS}
            for arm, cells in group.items():
                metrics = {k: estimate([r[k] for r in cells]) for k in (
                    "management_elapsed_ms", "management_cpu_ms", "management_wire_bytes",
                    "public_parameter_wire_bytes_once", "management_objects")}
                # Paired differences, not differences of independently pooled means.
                metrics["cpu_difference_from_batched_rsa_ms"] = estimate([
                    cells[i]["management_cpu_ms"] - group["BATCHED_RSA"][i]["management_cpu_ms"]
                    for i in range(repetitions)])
                metrics["wire_difference_from_batched_rsa_bytes"] = estimate([
                    cells[i]["management_wire_bytes"] + cells[i]["public_parameter_wire_bytes_once"]
                    - group["BATCHED_RSA"][i]["management_wire_bytes"]
                    for i in range(repetitions)])
                summary.append({"arm": arm, "permissions": p, "phase": phase, "metrics": metrics})
    return {"qualified": True, "scope": "native local component; no network or system recovery claim",
            "input_sha256": hashlib.sha256(raw).hexdigest(), "repetitions": repetitions,
            "identity_bootstrap": bootstrap[0], "management": summary,
            "access": [{"arm": a, "permissions": p, "epoch": e,
                        "metrics": {k: estimate([accesses[(a, p, r, e)][k]
                                                  for r in range(1, repetitions + 1)])
                                    for k in ("publication_cpu_ms", "cold_access_cpu_ms", "warm_total_cpu_ms",
                                              "business_wire_bytes", "content_key_wire_bytes_once")}}
                       for p in (1, 4, 16) for a in ARMS for e in (1, 2, 3)]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("--repetitions", type=int, required=True, choices=(1, 5))
    args = parser.parse_args()
    print(json.dumps(analyze(args.input, args.repetitions), indent=2, sort_keys=True))
