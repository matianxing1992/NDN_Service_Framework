#!/usr/bin/env python3
"""Focused source and executable gate for the protected UAV LiveStream adapter."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
from typing import Iterable


REPO = Path(__file__).resolve().parents[1]


def check(scan_paths: Iterable[Path] = ()) -> dict:
    provider = (REPO / "NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp").read_text()
    consumer = (REPO / "NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp").read_text()
    core = (REPO / "ndn-service-framework/Stream.cpp").read_text()
    schema = (REPO / "NDNSF-UAV-APP/configs/uav-stream-trust-schema.conf").read_text()
    checks = {
        "provider-uses-core-owner": "createLiveStream" in provider,
        "consumer-uses-core-handle": "openLiveStream" in consumer,
        "no-manual-video-pump": "requestVideoPackets" not in consumer,
        "no-uav-xor-state": all(token not in consumer for token in
                                ("FecFrameState", "processFecChunk", "recoverFecDataSymbol")),
        "core-has-no-uav-secret": all(token not in core for token in
                                      ("streamKey", "nonceSalt", "AES-256-GCM")),
        "provider-protects-before-core": 0 <= provider.find("protectUavVideoPacket") <
                                         provider.find("publishSample"),
        "consumer-decrypts-after-core": "admitLiveVideoItem" in consumer and
                                        "unprotectUavVideoPacket" in consumer,
        "provider-namespace-validation": "type hierarchical" in schema,
        "runtime-selects-uav-schema": "uav-stream-trust-schema.conf" in
            (REPO / "NDNSF-UAV-APP/configs/uav_runtime.conf").read_text(),
        "generic-response-log-is-payload-free":
            '\" payload=\" << payloadText' not in consumer,
    }
    ready_log_start = consumer.find('\"GS_VIDEO_STREAM_READY\"')
    ready_log = consumer[ready_log_start:ready_log_start + 1000]
    checks["ready-log-is-secret-free"] = ready_log_start >= 0 and all(
        token not in ready_log for token in ("stream_key_hex", "nonce_salt_hex"))

    secret_matches: list[str] = []
    for scan_path in scan_paths:
        paths = scan_path.rglob("*.log") if scan_path.is_dir() else [scan_path]
        for path in paths:
            text = path.read_text(encoding="utf-8", errors="replace")
            for token in ("stream_key_hex=", "nonce_salt_hex="):
                if token in text:
                    secret_matches.append(f"{path}:{token[:-1]}")
    checks["persisted-secret-scan"] = not secret_matches
    return {"passed": all(checks.values()), "checks": checks,
            "secretMatches": secret_matches}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--secret-scan", action="store_true",
                        help="Enable the static secret-log gate.")
    parser.add_argument("--scan-path", action="append", default=[], type=Path,
                        help="Also reject plaintext stream secrets in persisted *.log files.")
    args = parser.parse_args()
    result = check(args.scan_path)
    if result["passed"] and not args.source_only:
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = f"{REPO / 'build'}:/usr/local/lib:" + env.get(
            "LD_LIBRARY_PATH", "")
        completed = subprocess.run(
            [str(REPO / "build/unit-tests"), "--run_test=Stream,UavProtocolState",
             "--log_level=message"], cwd=REPO, env=env, text=True,
            capture_output=True)
        result["unitReturnCode"] = completed.returncode
        result["unitTail"] = (completed.stdout + completed.stderr)[-2000:]
        result["passed"] = completed.returncode == 0
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
