#!/usr/bin/env python3
"""Spec 146 acoustic-only wrapper over the frozen Spec 144 cell contract."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

import NDNSF_UAV_Sensor_Stream_Generality_Minindn as frozen


_FROZEN_BUILD_COMMANDS = frozen.build_commands


def build_commands(output: Path, transports: dict[str, str],
                   workload: str) -> dict[str, str]:
    if workload != "acoustic":
        raise ValueError("Spec 146 accepts only the frozen acoustic workload")
    commands = _FROZEN_BUILD_COMMANDS(output, transports, workload)
    for role in commands:
        commands[role] = commands[role].replace(
            "NDNSF_TIMELINE_TRACE=1",
            "NDNSF_TIMELINE_TRACE=1 "
            "NDNSF_STREAM_PACKET_TIMELINE_TRACE=0")
    if os.environ.get("SPEC146_TRACE_NACKS") == "1":
        sample_rate = os.environ.get("SPEC146_TRACE_SAMPLE_RATE", "1")
        if not sample_rate.isdigit() or int(sample_rate) < 1:
            raise ValueError("SPEC146_TRACE_SAMPLE_RATE must be a positive integer")
        for role in commands:
            commands[role] = commands[role].replace(
                "NDN_LOG=ndn_service_framework.ServiceProvider=WARN",
                "NDN_LOG='ndn_service_framework.ServiceProvider=WARN:"
                "ndn_service_framework.TimelineTrace=DEBUG'")
            commands[role] = commands[role].replace(
                "NDNSF_STREAM_PACKET_TIMELINE_TRACE=0",
                "NDNSF_STREAM_PACKET_TIMELINE_TRACE=1")
            commands[role] = commands[role].replace(
                "NDNSF_TIMELINE_TRACE_SAMPLE_RATE=1",
                f"NDNSF_TIMELINE_TRACE_SAMPLE_RATE={sample_rate}")
    commands["provider"] = commands["provider"].replace(
        "--warmup-seconds 5 --measurement-seconds 60",
        "--warmup-seconds 5 --measurement-seconds 60 "
        "--post-measurement-hold-seconds 20")
    commands["consumer"] = commands["consumer"].replace(
        "--warmup-seconds 5 --measurement-seconds 60",
        "--warmup-seconds 5 --measurement-seconds 60 "
        "--post-measurement-hold-seconds 5")
    return commands


def run(output: Path, profile: str, repetition: int = 1,
        formal: bool = False) -> dict:
    output = output.resolve()
    if "spec144" in str(output).lower():
        raise RuntimeError("Spec 146 refuses every Spec 144 destination")
    original = frozen.build_commands
    frozen.build_commands = build_commands
    try:
        result = frozen.run(output, "acoustic", profile, repetition,
                            formal=formal)
    finally:
        frozen.build_commands = original
    consumer_path = output / "consumer-status.json"
    summary_path = output / "summary.json"
    if consumer_path.is_file() and summary_path.is_file():
        consumer = json.loads(consumer_path.read_text(encoding="utf-8"))
        native = consumer.get("nativeStatus", {})
        analysis = result.setdefault("analysis", {})
        recovery = analysis.setdefault("recovery", {})
        eligible_sources = int(native.get("recoveryEligibleSources", 0))
        terminal_sources = int(native.get("terminalMissingSources", 0))
        recoverable_groups = int(native.get("recoverableGroups", 0))
        recovered_sources = int(native.get("recovered", 0))
        recovered_groups = int(native.get("recoveredGroups", 0))
        recovery.update({
            "terminalMissingSources": terminal_sources,
            "recoveryEligibleSources": eligible_sources,
            "recoverableGroups": recoverable_groups,
            "recoveredSources": recovered_sources,
            "recoveredGroups": recovered_groups,
            "algorithmInvocations": int(native.get("recoveryAttempts", 0)),
            "recoveryExhaustions": int(native.get("recoveryExhaustions", 0)),
            "sourceRecoveryRatio": {
                "numerator": recovered_sources,
                "denominator": eligible_sources,
                "value": (None if eligible_sources == 0
                          else recovered_sources / eligible_sources),
                "unavailableReason": (
                    "no-recovery-eligible-sources" if eligible_sources == 0 else ""),
            },
            "groupRecoveryRatio": {
                "numerator": recovered_groups,
                "denominator": recoverable_groups,
                "value": (None if recoverable_groups == 0
                          else recovered_groups / recoverable_groups),
                "unavailableReason": (
                    "no-recoverable-groups" if recoverable_groups == 0 else ""),
            },
        })
        result["schemaVersion"] = "spec146-acoustic-stability-cell-v1"
        summary_path.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", default="zero-loss",
                        choices=tuple(frozen.PROFILES))
    parser.add_argument("--repetition", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--formal", action="store_true")
    args = parser.parse_args()
    result = run(args.output, args.profile, args.repetition,
                 formal=args.formal)
    print({
        "cellId": result.get("cellId"),
        "passed": result.get("passed"),
        "output": str(args.output.resolve()),
    })
    return 0 if result.get("passed") else 2


if __name__ == "__main__":
    raise SystemExit(main())
