#!/usr/bin/env python3
"""Freeze and run the complete three-arm, five-block certificate-cache matrix."""
import argparse
import json
import os
from pathlib import Path
import random
import signal
import subprocess
import sys
import time
from credential_network_inputs import digest, qualify, verify_qualification
from run_credential_network import ARMS, candidate, save
from analyze_credential_network import analyze

HERE = Path(__file__).resolve().parent


def campaign_candidate():
    value = candidate()
    value["campaign_sources"] = {str(p): digest(p) for p in (
        Path(__file__), HERE / "credential_network_inputs.py", HERE / "summarize_credential_network.py")}
    return value


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--workspace", default="/var/tmp/ndnsf-spec204-credential-network")
    parser.add_argument("--qualification", type=Path)
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise SystemExit("MiniNDN campaign requires root")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    archive = args.archive.resolve()
    frozen = campaign_candidate()
    matrix = []
    for repetition in range(1, 6):
        scales = [1, 4, 16]
        random.Random(20450 + repetition).shuffle(scales)
        arms = list(ARMS[repetition % 3:] + ARMS[:repetition % 3])
        for permissions in scales:
            # Alternate cache order within and across blocks, with every pair retained.
            for index, arm in enumerate(arms):
                policies = ("reuse", "cold") if (index + repetition) % 2 else ("cold", "reuse")
                for cache in policies:
                    matrix.append({"arm": arm, "permissions": permissions, "repetition": repetition, "cache": cache})
    manifest = {"schema": "ndnsf-credential-network-campaign-v1", "status": "PARTIAL",
                "candidate": frozen, "matrix": matrix, "cells": [], "started_wall_ns": time.time_ns()}
    save(output / "manifest.json", manifest)
    started = time.monotonic()
    def interrupted(signum, frame):
        raise KeyboardInterrupt("supervisor signal " + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    try:
        if args.qualification:
            receipt = args.qualification.resolve()
            verify_qualification(archive, 5, receipt)
        else:
            receipt = qualify(archive, 5, output / "input-qualification")
        manifest["qualification"] = str(receipt)
        manifest["qualification_sha256"] = digest(receipt)
        save(output / "manifest.json", manifest)
        for index, cell in enumerate(matrix):
            if campaign_candidate() != frozen:
                raise RuntimeError("candidate changed before cell")
            verify_qualification(archive, 5, receipt)
            label = f"c{index:02d}-{cell['arm']}-p{cell['permissions']}-r{cell['repetition']}-{cell['cache']}"
            manifest["active_cell"] = label
            save(output / "manifest.json", manifest)
            command = [sys.executable, str(HERE / "run_credential_network.py"), "--archive", str(archive),
                "--archive-repetitions", "5", "--qualification", str(receipt), "--output-dir", str(output / label),
                "--workspace", args.workspace, "--arm", cell["arm"], "--permissions", str(cell["permissions"]),
                "--repetition", str(cell["repetition"]), "--cache", cell["cache"]]
            print("CREDENTIAL_CELL_START", index + 1, "/", len(matrix), label, flush=True)
            with (output / (label + ".log")).open("w") as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                try:
                    code = process.wait(timeout=180)
                except BaseException:
                    process.send_signal(signal.SIGINT)
                    try:
                        process.wait(timeout=45)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    raise
            if code != 0:
                raise RuntimeError("cell failed: " + label)
            summary = analyze(output / label)
            if summary["smoke"] or not summary["qualified"]:
                raise RuntimeError("formal cell not qualified")
            save(output / (label + "-summary.json"), summary)
            if campaign_candidate() != frozen or digest(receipt) != manifest["qualification_sha256"]:
                raise RuntimeError("candidate/receipt changed during cell")
            manifest["cells"].append({**cell, "id": label, "summary": label + "-summary.json"})
            manifest["elapsed_seconds"] = time.monotonic() - started
            save(output / "manifest.json", manifest)
            print("CREDENTIAL_CELL_COMPLETE", len(manifest["cells"]), "/", len(matrix), label, flush=True)
        manifest["status"] = "CAMPAIGN_COMPLETE"
    except BaseException as error:
        manifest["first_failure"] = type(error).__name__ + ": " + str(error)
        raise
    finally:
        manifest["elapsed_seconds"] = time.monotonic() - started
        save(output / "manifest.json", manifest)
    print("CREDENTIAL_CAMPAIGN_COMPLETE", output, flush=True)
