#!/usr/bin/env python3
"""Run the independent Spec 140 two-cell MiniNDN diagnostic."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import time

try:
    from . import NDN_SVS_RSA_Single_Worker_Minindn as base
except ImportError:
    import NDN_SVS_RSA_Single_Worker_Minindn as base


REPO = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = (
    REPO
    / "build/spec140-svs-latency-distribution/svs-rsa-latency-distribution"
)
DEFAULT_LIBRARY_DIR = Path("/home/tianxing/NDN/ndn-svs/build")
RESULT_ROOT = REPO / "results/spec140-svs-latency-distribution"
MATRIX = (("face-inline-rsa", 400), ("worker-rsa", 400))
TIMING = (10, 60, 10)


def is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--library-dir", type=Path, default=DEFAULT_LIBRARY_DIR)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if os.geteuid() != 0:
        raise SystemExit("MiniNDN runner must execute as root")
    binary = args.binary.resolve()
    library_dir = args.library_dir.resolve()
    library = library_dir / "libndn-svs.so"
    if not binary.is_file() or not library.is_file():
        raise SystemExit("binary or local libndn-svs.so is missing")

    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    campaign = (
        args.output.resolve()
        if args.output
        else RESULT_ROOT / f"diagnostic-{stamp}"
    )
    frozen_root = REPO / "results/spec136-rsa-single-worker"
    if is_within(campaign, frozen_root):
        raise SystemExit("Spec 140 refuses to write beneath the frozen Spec 136 root")
    campaign.mkdir(parents=True, exist_ok=False)

    manifest = {
        "schema": "spec140.campaign.v1",
        "campaignKind": "latency-distribution-diagnostic",
        "formal": False,
        "automaticRetry": False,
        "binary": str(binary),
        "binarySha256": base.sha256(binary),
        "library": str(library),
        "librarySha256": base.sha256(library),
        "matrix": [
            {"ordinal": ordinal, "mode": mode, "ratePerPeer": rate}
            for ordinal, (mode, rate) in enumerate(MATRIX, 1)
        ],
        "timing": {
            "warmup": TIMING[0],
            "measure": TIMING[1],
            "drain": TIMING[2],
        },
        "twoNodes": True,
        "bothPeersPublishAndSubscribe": True,
        "pacerKind": "independent-app-thread",
        "controlCallerPath": "app-post-to-face",
        "treatmentCallerPath": "app-direct-to-worker-backed-publishAsync",
        "summarySchema": "spec140.peer-summary.v1",
        "rawDeliverySamples": True,
        "rateUnit": "publications-per-second-per-peer",
        "effectiveCpuAffinity": sorted(os.sched_getaffinity(0)),
    }
    base.write_json(campaign / "campaign-manifest.json", manifest)

    terminals = []
    try:
        for ordinal, (mode, rate) in enumerate(MATRIX, 1):
            terminals.append(
                base.run_cell(
                    campaign,
                    binary,
                    library_dir,
                    ordinal,
                    mode,
                    rate,
                    TIMING,
                    experiment_namespace="spec140",
                    summary_schema="spec140.peer-summary.v1",
                    record_delivery_samples=True,
                )
            )
        base.write_json(campaign / "campaign-terminals.json", terminals)
    finally:
        base.restore_invoking_user_ownership(campaign.parent)

    print(campaign)
    return 0 if len(terminals) == len(MATRIX) and all(
        terminal["status"] == "COMPLETE" for terminal in terminals
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
