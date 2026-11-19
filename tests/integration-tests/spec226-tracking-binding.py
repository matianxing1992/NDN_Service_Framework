#!/usr/bin/env python3
"""Exercise the installed thin binding against an isolated native NFD/Controller.

This tests Job admission/cancel/drain, not inference or browser qualification.
The caller supplies isolated credentials and a real native Controller.
"""

import argparse
import hashlib
import json
import os
import tempfile
import time
from pathlib import Path

from ndnsf import ServiceUser
from ndnsf import _ndnsf


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job-config", type=Path, required=True)
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--controller-cert", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--expect-unavailable", action="store_true")
    args = parser.parse_args()
    os.environ["NDNSF_CONTROLLER_CERT_FILE"] = str(args.controller_cert.resolve(strict=True))
    definition = json.loads(args.job_config.read_text())
    checks = []
    user = ServiceUser(group=args.group, controller=args.controller, user=args.user,
                       trust_schema=args.trust_schema)
    user.start()
    try:
        if args.expect_unavailable:
            assert not user.tracking_available()
            for operation in (
                    lambda: user.start_tracking(args.job_config),
                    lambda: user.read_tracking_frame("absent", 1, 0, "", "UAV1", ""),
                    lambda: user.stop_tracking(1)):
                try:
                    operation()
                except RuntimeError as error:
                    assert "capability unavailable" in str(error), error
                else:
                    raise AssertionError("core-only module admitted tracking")
            maps = Path("/proc/self/maps").read_text()
            assert "libndnsf-uav-tracking" not in maps
            receipt = {"status": "PASS", "checks": "core-only-capability-rejections",
                       "module": _ndnsf.__file__, "loadedTrackingLibraries": []}
            args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
            print(json.dumps(receipt))
            return
        assert user.tracking_available(), "installed native tracking capability missing"
        admitted = user.start_tracking(args.job_config)
        assert admitted["accepted"], admitted
        first = admitted["snapshot"]
        assert first["jobId"] == definition["jobId"]
        assert first["runId"] == definition["runId"]
        checks.append("native-admission-and-identity")
        assert user.cancel_tracking("not-this-job") == "WRONG_JOB"
        try:
            user.read_tracking_frame("old-job", first["generation"], 0,
                                     "sha256:" + "a" * 64, "UAV1", "sha256:" + "b" * 64)
        except ValueError:
            checks.append("wrong-frame-identity-rejected")
        else:
            raise AssertionError("unadmitted frame read succeeded")
        assert user.cancel_tracking(first["jobId"]) in ("REQUESTED", "ALREADY_REQUESTED")
        stopped = user.stop_tracking(30000)
        assert stopped["completed"] and stopped["drained"], stopped
        assert stopped["cleanupState"] == "CLEAN", stopped
        repeated = user.stop_tracking(30000)
        assert repeated["completed"] and repeated["drained"], repeated
        assert repeated["cleanupState"] == "CLEAN", repeated
        assert user.tracking_snapshot()["state"] == "CANCELLED"
        checks.append("cancel-repeated-stop-clean-and-snapshot")
        duplicate = user.start_tracking(args.job_config)
        assert not duplicate["accepted"] and duplicate["admission"] == "INVALID", duplicate
        assert "identity" in duplicate["reason"], duplicate
        assert duplicate["snapshot"]["jobId"] == first["jobId"]
        assert duplicate["snapshot"]["runId"] == first["runId"]
        assert duplicate["snapshot"]["generation"] == first["generation"]
        checks.append("same-execution-cannot-restart")
        with tempfile.TemporaryDirectory(prefix="spec226-binding-next-") as temporary:
            definition["jobId"] += "-next"
            definition["runId"] += "-next"
            definition["outputDirectory"] = temporary
            next_config = Path(temporary) / "next.json"
            next_config.write_text(json.dumps(definition))
            admitted = user.start_tracking(next_config)
            assert admitted["accepted"], admitted
            assert admitted["snapshot"]["generation"] == first["generation"] + 1
            assert user.cancel_tracking(first["jobId"]) == "WRONG_JOB"
            stopped = user.stop_tracking(30000)
            assert stopped["completed"] and stopped["drained"], stopped
            assert stopped["cleanupState"] == "CLEAN", stopped
            checks.append("new-execution-generation-and-old-cancel-rejection")
    finally:
        try:
            if not args.expect_unavailable:
                user.stop_tracking(30000)
        finally:
            user.stop()
    module = Path(_ndnsf.__file__).resolve()
    maps = Path("/proc/self/maps").read_text().splitlines()
    receipt = {"checks": checks, "module": str(module),
               "moduleSha256": hashlib.sha256(module.read_bytes()).hexdigest(),
               "loadedTrackingLibraries": sorted({line.split()[-1] for line in maps
                                                  if "libndnsf-uav-tracking" in line}),
               "completedAtUnixNs": time.time_ns()}
    args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"status": "PASS", "checks": len(checks)}))


if __name__ == "__main__":
    main()
