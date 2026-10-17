#!/usr/bin/env python3
"""Independent phase/cardinality and IPv4 accounting for native credential-network cells."""
import argparse
import json
from pathlib import Path
import re
import statistics
from analyze_dynamic import ip_counts, require, rows
from run_credential_network import digest
from credential_network_inputs import verify_qualification


def one(data, event, epoch=None, **fields):
    found = [r for r in data if r["event"] == event and (epoch is None or r.get("epoch") == epoch)
             and all(r.get(key) == value for key, value in fields.items())]
    require(len(found) == 1, f"missing/duplicate {event}/{epoch}/{fields}")
    return found[0]


def analyze(directory):
    m = json.loads((directory / "manifest.json").read_text())
    require(m["schema"] == "ndnsf-credential-network-cell-v1" and m["status"] in ("RUN_COMPLETE", "NEGATIVE_COMPLETE"), "incomplete cell")
    require(m["inputs_unchanged"] and not m.get("cleanup_errors"), "source drift/cleanup")
    expected = {"server", *(f"user{i}" for i in range(8)), *("capture-" + p for p in ("gs", "drone1", "drone2")),
                *(p + "-nfd" for p in ("gs", "relay", "drone1", "drone2"))}
    require({c["label"] for c in m["children"]} == expected and len(m["children"]) == len(expected), "child inventory")
    for child in m["children"]:
        code = 2 if child["label"].startswith("user") and m["negative"] != "none" else 0
        require(child["exit"] == code, "actor exit")
    for peer in ("gs", "drone1", "drone2"):
        require(m["capture"][peer]["dropped_packets"] == 0, "capture loss")
        require(re.findall(r"(\d+) packets dropped by kernel", (directory / ("capture-" + peer + ".log")).read_text()) == ["0"], "capture raw closure")
        counts = ip_counts(directory / (peer + ".pcap"), m["anchor"], 0, 2**63-1)
        records = sum(part["packets"] for part in counts.values())
        captured = re.findall(r"(\d+) packets captured", (directory / ("capture-" + peer + ".log")).read_text())
        received = re.findall(r"(\d+) packets received by filter", (directory / ("capture-" + peer + ".log")).read_text())
        require(captured == received == [str(records)] and records > 0, "Linux/veth pcap record/capture inventory")
    archive = Path(m["archive"])
    if m.get("qualification"):
        receipt = directory / "qualification.json"
        require(digest(receipt) == m["qualification_sha256"], "copied native qualification drift")
        verify_qualification(archive, m["archive_repetitions"], receipt)
    for name, expected_hash in m["archive_hashes"].items():
        require(digest(archive / name) == expected_hash, "archive drift")
    fixture = json.loads((directory / "case.json").read_text())
    original = json.loads((archive / m["case"] / "manifest.json").read_text())
    require(fixture == original and (fixture["arm"], fixture["permissions"], fixture["repetition"]) ==
            (m["arm"], m["permissions"], m["repetition"]), "public case identity")
    oracle = rows(directory / "archive-oracle.jsonl")
    require(len(oracle) == 1 and oracle[0]["oracle"] and oracle[0]["cases"] == 9 * m["archive_repetitions"], "native archive qualification")
    logs = [rows(directory / f"user{i}.jsonl") for i in range(8)]
    for i, data in enumerate(logs):
        ready = one(data, "ready")
        require((ready["user"], ready["arm"], ready["cache"]) == (i, m["arm"], m["cache"]), "actor profile")
        boot = m["bootstrap"][str(i)]
        require(boot["node"] == ("drone1" if i < 4 else "drone2") and
                boot["own_cert_sha256"] == m["archive_hashes"][f"recipient-{i}.cert"] and
                boot["own_bag_sha256"] == m["archive_hashes"][f"recipient-{i}.safebag"], "own bootstrap binding")
    one(rows(directory / "server.jsonl"), "ready")
    if m["negative"] != "none":
        require(m["status"] == "NEGATIVE_COMPLETE" and len(m["epochs"]) == 1, "negative profile")
        failed = one(logs[0], "failure", 1)
        require(not any(r["event"] in ("management_installed", "phase_complete", "access_result") for r in logs[0]), "negative installed state")
        if m["negative"] == "certificate-pin":
            require(failed["reason"] == "certificate fingerprint mismatch", "wrong negative boundary")
        else:
            changed = json.loads((directory / "wrong-recipient.json").read_text())
            expected_fixture = json.loads(json.dumps(original))
            donor = [o for o in original["objects"] if o["epoch"] == 1 and o["kind"] == "management"
                     and o["recipient"] == "/system-benefits/user/1"]
            targets = [o for o in expected_fixture["objects"] if o["epoch"] == 1 and o["kind"] == "management"
                       and o["recipient"] == "/system-benefits/user/0"]
            require(len(targets) == len(donor) and donor, "negative donor cardinality")
            for target, source in zip(targets, donor):
                target["packets"] = source["packets"]
            require(changed == expected_fixture, "negative differs beyond actual donor references")
            require(failed["reason"] == "Failed to decrypt ciphertext", "wrong-key decryption boundary")
            require(any(r["event"] == "fetch_complete" and r["kind"] == "management" for r in logs[0]), "negative never fetched actual material")
        require(not any(r["event"] == "failure" for data in logs[1:] for r in data), "other recipient failure")
        return {"qualified": True, "negative": m["negative"], "scope": "native network rejection, no performance claim"}
    require(m["status"] == "RUN_COMPLETE" and [p["epoch"] for p in m["epochs"]] == [1, 2, 3], "epoch matrix")
    require(m["smoke"] or m["observation_end_ns"] - m["observation_begin_ns"] >= 60_000_000_000, "formal observation scope")
    results = []
    previous_end = m["observation_begin_ns"]
    for phase in m["epochs"]:
        epoch = phase["epoch"]
        t0, t1, p0, p1 = [phase[key] for key in ("management_begin_ns", "management_end_ns", "probe_begin_ns", "probe_end_ns")]
        require(previous_end <= t0 < t1 <= p0 < p1 <= m["observation_end_ns"], "global barrier ordering")
        previous_end = p1
        summary = {"epoch": epoch, "management_barrier_ms": (t1-t0)/1e6,
                   "management_cpu_ms": 0, "install_cpu_ms": 0, "certificate_cpu_ms": 0,
                   "certificate_interests": 0, "certificate_data_bytes": 0,
                   "management_interests": 0, "management_data_bytes": 0,
                   "management_interest_bytes": 0, "probe_cpu_ms": 0, "probe_data_bytes": 0,
                   "access_checks": 0, "per_user_management_ms": []}
        for i, data in enumerate(logs):
            require(not any(r["event"] == "failure" for r in data), "native failure")
            begin = one(data, "phase_begin", epoch)
            installed = one(data, "management_installed", epoch)
            complete = one(data, "phase_complete", epoch)
            require(t0 <= begin["ns"] <= installed["ns"] < t1 and p0 <= complete["ns"] < p1, "native barriers")
            require(complete["oracle"] and complete["ns"] - begin["ns"] < 10_000_000_000, "phase oracle/deadline")
            require(phase["installed"][str(i)] == installed and phase["complete"][str(i)] == complete, "harness/raw barrier agreement")
            state = one(data, "recipient_state_prepared", epoch)
            require(state["initial_bootstrap"] == (epoch == 1), "recipient state unexpectedly recreated")
            if epoch == 1 or m["cache"] == "cold":
                certificate = one(data, "fetch_complete", epoch, kind="certificate")
                require(certificate["interests"] == 1 and certificate["data_wire_bytes"] == m["certificate_wire_bytes"], "certificate fetch")
                validation = one(data, "certificate_validated", epoch)
                summary["certificate_cpu_ms"] += certificate["cpu_ms"] + validation["cpu_ms"]
                summary["certificate_interests"] += 1
                summary["certificate_data_bytes"] += certificate["data_wire_bytes"]
                require(not any(r["event"] == "certificate_cache_hit" and r["epoch"] == epoch for r in data), "cold cache inconsistency")
            else:
                one(data, "certificate_cache_hit", epoch)
                require(not any(r["event"] == "fetch_complete" and r["epoch"] == epoch and r["kind"] == "certificate" for r in data), "cache hit still fetched")
            management = one(data, "fetch_complete", epoch, kind="management")
            packets = [packet for obj in fixture["objects"] if obj["epoch"] == epoch and
                       (obj["kind"] == "public-parameters" or obj["kind"] == "management" and obj["recipient"] == f"/system-benefits/user/{i}")
                       for packet in obj["packets"]]
            require(management["interests"] == len(packets) and management["data_wire_bytes"] == sum(p["wire_bytes"] for p in packets), "management acquisition accounting")
            probes = one(data, "fetch_complete", epoch, kind="probes")
            probe_packets = [packet for obj in fixture["objects"] if obj["epoch"] == epoch and
                             obj["kind"] in ("probe-business", "probe-content-key") for packet in obj["packets"]]
            require(p0 <= probes["ns"] < p1 and probes["interests"] == len(probe_packets) and
                    probes["data_wire_bytes"] == sum(p["wire_bytes"] for p in probe_packets), "probe isolation/accounting")
            count = m["permissions"] + (epoch != 1)
            require(complete["access_checks"] == count, "access cardinality")
            for permission in range(count):
                access = one(data, "access_result", epoch, permission=permission)
                expected_access = i > 0 and permission < m["permissions"] if epoch == 3 else permission < m["permissions"] or i == 0
                require(access["allowed"] == expected_access, "permission/grant/revocation matrix")
            summary["management_cpu_ms"] += installed["management_cpu_ms"]
            summary["install_cpu_ms"] += installed["cpu_ms"]
            summary["management_interests"] += management["interests"]
            summary["management_data_bytes"] += management["data_wire_bytes"]
            summary["management_interest_bytes"] += management["interest_wire_bytes"]
            summary["probe_cpu_ms"] += complete["probe_cpu_ms"]
            summary["probe_data_bytes"] += probes["data_wire_bytes"]
            summary["access_checks"] += count
            summary["per_user_management_ms"].append(installed["phase_elapsed_ms"])
        summary["mean_recipient_management_ms"] = statistics.mean(summary["per_user_management_ms"])
        summary["management_ip_bytes"] = sum(ip_counts(directory / (peer + ".pcap"), m["anchor"], t0, t1)["window"]["ip_bytes"]
                                             for peer in ("gs", "drone1", "drone2"))
        summary["probe_ip_bytes"] = sum(ip_counts(directory / (peer + ".pcap"), m["anchor"], p0, p1)["window"]["ip_bytes"]
                                        for peer in ("gs", "drone1", "drone2"))
        for peer in ("gs", "drone1", "drone2"):
            if epoch == 1:
                require(ip_counts(directory / (peer + ".pcap"), m["anchor"], t0, t1)["window"]["ip_bytes"] > 0, "missing initial distribution link")
            require(ip_counts(directory / (peer + ".pcap"), m["anchor"], p0, p1)["window"]["ip_bytes"] > 0, "missing probe link")
        results.append(summary)
    require(sum(r["event"] == "access_result" for d in logs for r in d) == sum(r["access_checks"] for r in results), "extra/missing accesses")
    return {"qualified": True, "scope": "native MiniNDN event distribution; not production Authority transport",
            "smoke": m["smoke"], "arm": m["arm"], "permissions": m["permissions"], "repetition": m["repetition"],
            "cache": m["cache"], "phases": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.directory), indent=2))
