#!/usr/bin/env python3
"""Run the frozen-config Spec 141 600/800 pps MiniNDN extension."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import time

try:
    from . import NDN_SVS_Latency_Distribution_Minindn as spec140
except ImportError:
    import NDN_SVS_Latency_Distribution_Minindn as spec140


REPO = Path(__file__).resolve().parents[1]
BASE = spec140.base
BINARY = spec140.DEFAULT_BINARY
LIBRARY_DIR = spec140.DEFAULT_LIBRARY_DIR
RESULT_ROOT = REPO / "results/spec141-svs-latency-rate-extension"
MATRIX = (
    ("face-inline-rsa", 600),
    ("worker-rsa", 600),
    ("face-inline-rsa", 800),
    ("worker-rsa", 800),
)
TIMING = spec140.TIMING
EXPECTED_BINARY_SHA256 = (
    "a5789d075ec0fbd702add6cc4084bddd0e91cc996b12ca6166e665fd6ec9204a"
)
ACCEPTED_TERMINALS = {"COMPLETE", "LOAD_UNSUSTAINED"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=BINARY)
    parser.add_argument("--library-dir", type=Path, default=LIBRARY_DIR)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if os.geteuid() != 0:
        raise SystemExit("MiniNDN runner must execute as root")
    binary = args.binary.resolve()
    library_dir = args.library_dir.resolve()
    library = library_dir / "libndn-svs.so"
    if not binary.is_file() or not library.is_file():
        raise SystemExit("binary or local libndn-svs.so is missing")
    if BASE.sha256(binary) != EXPECTED_BINARY_SHA256:
        raise SystemExit("binary does not match the frozen Spec 140 identity")

    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    campaign = (
        args.output.resolve()
        if args.output
        else RESULT_ROOT / f"extension-{stamp}"
    )
    protected = (
        REPO / "results/spec136-rsa-single-worker",
        REPO / "results/spec140-svs-latency-distribution",
    )
    if any(spec140.is_within(campaign, root) for root in protected):
        raise SystemExit("Spec 141 refuses to write beneath a frozen result root")
    campaign.mkdir(parents=True, exist_ok=False)

    BASE.write_json(
        campaign / "campaign-manifest.json",
        {
            "schema": "spec141.campaign.v1",
            "campaignKind": "latency-rate-extension",
            "formal": False,
            "automaticRetry": False,
            "binary": str(binary),
            "binarySha256": BASE.sha256(binary),
            "library": str(library),
            "librarySha256": BASE.sha256(library),
            "frozenConfigSource": str(
                REPO / "specs/140-svs-latency-distribution-diagnostic"
            ),
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
            "treatmentCallerPath": (
                "app-direct-to-worker-backed-publishAsync"
            ),
            "summarySchema": "spec140.peer-summary.v1",
            "rawDeliverySamples": True,
            "rateUnit": "publications-per-second-per-peer",
            "effectiveCpuAffinity": sorted(os.sched_getaffinity(0)),
        },
    )

    terminals = []
    try:
        for ordinal, (mode, rate) in enumerate(MATRIX, 1):
            terminals.append(
                BASE.run_cell(
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
        BASE.write_json(campaign / "campaign-terminals.json", terminals)
    finally:
        BASE.restore_invoking_user_ownership(campaign.parent)

    print(campaign)
    return 0 if len(terminals) == len(MATRIX) and all(
        terminal["status"] in ACCEPTED_TERMINALS for terminal in terminals
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
