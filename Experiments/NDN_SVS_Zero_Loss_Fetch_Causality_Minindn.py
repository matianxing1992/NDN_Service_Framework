#!/usr/bin/env python3
"""Run the exactly-once Spec 143 zero-loss Fetch causality diagnostic."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import time
from typing import Any

try:
    from . import NDN_SVS_RSA_Single_Worker_Minindn as base
    from . import NDN_SVS_NDNSF_Profile_Worker_Minindn as profile
    from . import analyze_svs_zero_loss_fetch_causality as analyzer
    from . import build_svs_zero_loss_fetch_causality as builder
except ImportError:
    import NDN_SVS_RSA_Single_Worker_Minindn as base
    import NDN_SVS_NDNSF_Profile_Worker_Minindn as profile
    import analyze_svs_zero_loss_fetch_causality as analyzer
    import build_svs_zero_loss_fetch_causality as builder


REPO = Path(__file__).resolve().parents[1]
RESULT_ROOT = REPO / "results/spec143-svs-zero-loss-fetch-causality"
DEFAULT_BINARY = (
    REPO
    / "build/spec143-svs-zero-loss-fetch-causality/"
    "svs-zero-loss-fetch-causality"
)
DEFAULT_LIBRARY_DIR = Path("/home/tianxing/NDN/ndn-svs/build")
DEFAULT_BUILD_MANIFEST = (
    REPO / "build/spec143-svs-zero-loss-fetch-causality/build-manifest.json"
)
SPEC142_BASELINE = (
    REPO
    / "results/spec142-svs-ndnsf-runtime-profile/"
    "campaign-20260724T012559Z"
)
PEER_SUMMARY_SCHEMA = "spec143.peer-summary.v1"
TERMINAL_SCHEMA = "spec143.diagnostic-terminal.v1"
RUNTIME_MANIFEST_SCHEMA = "spec143.runtime-profile-manifest.v1"
TIMING = (10, 60, 10)
NDN_LOG = (
    "*=WARN:"
    "ndn_svs.Fetcher=TRACE:"
    "ndn_svs.SVSyncBase=TRACE:"
    "ndn_svs.MappingProvider=TRACE:"
    "ndn_svs.SVSPubSub=TRACE"
)


def validate_build_identity(
    binary: Path, library: Path, manifest_path: Path
) -> dict[str, Any]:
    builder.verify(manifest_path)
    record = json.loads(manifest_path.read_text(encoding="utf-8"))
    if record.get("schema") != builder.SCHEMA:
        raise RuntimeError("unexpected Spec 143 build manifest schema")
    if Path(record["binary"]).resolve() != binary.resolve():
        raise RuntimeError("binary path differs from build manifest")
    if Path(record["library"]).resolve() != library.resolve():
        raise RuntimeError("library path differs from build manifest")
    return record


def validate_peer(
    summary: dict[str, Any],
    mode: str,
    rate: int,
    measure: int,
    peer: str,
    expected_schema: str = PEER_SUMMARY_SCHEMA,
) -> list[str]:
    errors: list[str] = []
    if summary.get("schema") != expected_schema:
        errors.append(
            f"PROFILE_INVALID:{peer}:schema={summary.get('schema')!r} "
            f"expected={expected_schema!r}"
        )
    for key, expected in profile.expected_profile(rate).items():
        if summary.get(key) != expected:
            errors.append(
                f"PROFILE_INVALID:{peer}:{key}={summary.get(key)!r} "
                f"expected={expected!r}"
            )
    if summary.get("publicationWorkers") != 1 or mode != "worker-rsa":
        errors.append(f"PROFILE_INVALID:{peer}:worker treatment mismatch")
    scheduled = rate * measure
    attempted = int(summary.get("attemptedMeasured", 0))
    if int(summary.get("scheduledMeasured", -1)) != scheduled:
        errors.append(f"{peer}:scheduled count mismatch")
    attempted_ratio = attempted / scheduled if scheduled else 0.0
    if not 0.98 <= attempted_ratio <= 1.02:
        errors.append(
            f"{peer}:attempted-ratio={attempted_ratio:.6f} outside [0.98,1.02]"
        )
    face_thread = int(summary.get("faceThreadHash", 0))
    pacer_thread = int(summary.get("pacerThreadHash", 0))
    call_thread = int(summary.get("publishCallThreadHash", 0))
    if (
        face_thread == 0
        or pacer_thread == 0
        or face_thread == pacer_thread
        or call_thread != pacer_thread
        or int(summary.get("publishCallsOnFace", 0)) != 0
        or int(summary.get("publishCallsOnPacer", 0)) <= 0
    ):
        errors.append(f"{peer}:worker/Face/APP thread contract invalid")
    if summary.get("pacerFailed") is not False:
        errors.append(f"{peer}:pacer failed: {summary.get('pacerError', '')}")
    if (
        summary.get("dataSignatureType") != 1
        or summary.get("syncEnvelopeSignatureType") != 1
        or int(summary.get("dataValid", 0)) <= 0
    ):
        errors.append(f"{peer}:RSA sign/validation evidence missing")
    for key in (
        "dataInvalid",
        "interestInvalid",
        "invalid",
        "workerOutstanding",
        "faceDispatchAbandoned",
    ):
        if int(summary.get(key, 0)) != 0:
            errors.append(f"{peer}:{key}={summary.get(key)}")
    for key in (
        "publicationFetchInnerRetriesAtMeasureStart",
        "publicationFetchInnerRetriesAtMeasureEnd",
        "publicationFetchOuterRetryActivationsAtMeasureStart",
        "publicationFetchOuterRetryActivationsAtMeasureEnd",
        "resourceMeasureStartSteadyNs",
        "resourceMeasureEndSteadyNs",
        "resourceMeasureWallNs",
        "resourceUserCpuUs",
        "resourceSystemCpuUs",
        "resourceTotalCpuUs",
        "resourceCpuPctOneCore",
        "resourceCpuPctFourCore",
        "resourceThreadsAtMeasureStart",
        "resourceThreadsAtMeasureEnd",
    ):
        if key not in summary:
            errors.append(f"{peer}:{key} missing")
    return errors


def classify_terminal(
    *, error: str, admission_errors: list[str]
) -> dict[str, object]:
    profile_errors = [
        item for item in admission_errors if item.startswith("PROFILE_INVALID:")
    ]
    harness_errors = [
        item
        for item in admission_errors
        if not item.startswith(("PROFILE_INVALID:", "LOAD_UNSUSTAINED:"))
    ]
    validity = (
        "HARNESS_FAILED"
        if error or harness_errors
        else "PROFILE_INVALID"
        if profile_errors
        else "PROFILE_VALID"
    )
    return {
        "validity": validity,
        "outcome": "COMPLETE",
        "profileErrors": profile_errors,
        "harnessErrors": harness_errors,
        "loadErrors": [],
    }


def new_campaign(output: Path | None) -> Path:
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    campaign = (
        output.resolve()
        if output is not None
        else RESULT_ROOT / f"diagnostic-{stamp}"
    )
    campaign.mkdir(parents=True, exist_ok=False)
    return campaign


def run_preflight(
    campaign: Path,
    binary: Path,
    library_dir: Path,
    build_manifest: Path,
) -> dict[str, Any]:
    library = library_dir / "libndn-svs.so"
    build = validate_build_identity(binary, library, build_manifest)
    profile_dir = campaign / "profile-worker-rsa"
    arguments = {
        peer: [
            "--profile-only",
            "--runtime-profile",
            profile.RUNTIME_PROFILE_NAME,
            "--mode",
            "worker-rsa",
            "--peer-id",
            peer,
            "--rate",
            "400",
            "--summary",
            str(profile_dir / f"{peer}-summary.json"),
            "--summary-schema",
            PEER_SUMMARY_SCHEMA,
        ]
        for peer in base.PEERS
    }
    summaries = profile._local_pair(
        profile_dir, binary, library_dir, arguments, timeout=20
    )
    errors: list[str] = []
    for peer, summary in summaries.items():
        for key, expected in profile.expected_profile(400).items():
            if summary.get(key) != expected:
                errors.append(
                    f"{peer}:{key}={summary.get(key)!r} expected={expected!r}"
                )
        if summary.get("publicationWorkers") != 1:
            errors.append(f"{peer}:publicationWorkers is not one")
    baseline_hash = base.tree_sha256(SPEC142_BASELINE)
    manifest = {
        "schema": RUNTIME_MANIFEST_SCHEMA,
        "createdUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "status": "PASS" if not errors else "FAIL",
        "errors": errors,
        "binary": str(binary),
        "binarySha256": base.sha256(binary),
        "library": str(library),
        "librarySha256": base.sha256(library),
        "buildManifest": str(build_manifest),
        "buildManifestSha256": base.sha256(build_manifest),
        "build": build,
        "resolvedProfiles": summaries,
        "diagnosticNdnLog": NDN_LOG,
        "topology": {
            "nodes": 2,
            "bandwidthMbps": 100,
            "oneWayDelayMs": 10,
            "configuredLossPct": 0,
            "bothPeersPublishAndSubscribe": True,
        },
        "timing": {"warmup": 10, "measure": 60, "drain": 10},
        "cpuAffinity": sorted(os.sched_getaffinity(0)),
        "cell": {"mode": "worker-rsa", "ratePerPeer": 400},
        "spec142Baseline": {
            "path": str(SPEC142_BASELINE),
            "treeSha256Before": baseline_hash,
        },
        "inlineExecutionAuthorized": False,
    }
    base.write_json(campaign / "runtime-profile-manifest.json", manifest)
    return manifest


def link_traces(cell: Path) -> None:
    for peer in base.PEERS:
        stderr = cell / f"{peer}.stderr"
        trace = cell / f"{peer}.trace.log"
        if trace.exists():
            raise RuntimeError(f"trace already exists: {trace}")
        os.link(stderr, trace)


def run_worker_stage(
    campaign: Path, binary: Path, library_dir: Path
) -> dict[str, Any]:
    manifest_path = campaign / "runtime-profile-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("status") != "PASS":
        raise RuntimeError("runtime preflight did not pass")
    if base.sha256(binary) != manifest.get("binarySha256"):
        raise RuntimeError("binary identity changed after preflight")
    library = library_dir / "libndn-svs.so"
    if base.sha256(library) != manifest.get("librarySha256"):
        raise RuntimeError("library identity changed after preflight")
    stage_receipt = campaign / "worker-stage.json"
    if stage_receipt.exists():
        raise RuntimeError("worker stage already has terminal evidence")

    with profile.stage_lock(campaign, "worker-400"):
        terminal = base.run_cell(
            campaign,
            binary,
            library_dir,
            1,
            "worker-rsa",
            400,
            TIMING,
            experiment_namespace="spec143",
            summary_schema=PEER_SUMMARY_SCHEMA,
            record_delivery_samples=True,
            extra_peer_arguments=("--runtime-profile", profile.RUNTIME_PROFILE_NAME),
            admission_validator=validate_peer,
            terminal_schema=TERMINAL_SCHEMA,
            ndn_log=NDN_LOG,
            terminal_classifier=classify_terminal,
        )
        cell = campaign / terminal["cellId"]
        link_traces(cell)
        analysis = analyzer.analyze_cell(cell)
        analyzer.write_json(cell / "analysis.json", analysis)
        (cell / "report.md").write_text(
            analyzer.render_report(analysis), encoding="utf-8"
        )
        diagnosis = analysis["classification"]
        baseline_after = base.tree_sha256(SPEC142_BASELINE)
        baseline_before = manifest["spec142Baseline"]["treeSha256Before"]
        terminal["spec142BaselineUnchanged"] = baseline_after == baseline_before
        terminal["spec142BaselineTreeSha256After"] = baseline_after
        terminal["diagnosisStatus"] = (
            diagnosis["status"]
            if terminal.get("validity") == "PROFILE_VALID"
            and analysis["resource"]["valid"]
            and baseline_after == baseline_before
            else "HARNESS_FAILED"
        )
        terminal["classificationSummary"] = str(
            cell / "classification-summary.json"
        )
        terminal["classificationSummarySha256"] = base.sha256(
            cell / "classification-summary.json"
        )
        terminal["inlineEligibility"] = {
            "eligible": diagnosis["timeoutCount"] == 0,
            "authorized": False,
            "reason": (
                "worker cell did not reproduce a timeout"
                if diagnosis["timeoutCount"] == 0
                else "worker cell reproduced the boundary; inline is unnecessary"
            ),
        }
        base.write_json(cell / "terminal.json", terminal)
        stage = {
            "schema": "spec143.worker-stage.v1",
            "cell": str(cell),
            "terminal": str(cell / "terminal.json"),
            "terminalSha256": base.sha256(cell / "terminal.json"),
            "diagnosisStatus": terminal["diagnosisStatus"],
            "inlineExecutionAuthorized": False,
        }
        base.write_json(stage_receipt, stage)
        return terminal


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--stage", choices=("preflight", "worker-400"), required=True
    )
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--library-dir", type=Path, default=DEFAULT_LIBRARY_DIR)
    parser.add_argument(
        "--build-manifest", type=Path, default=DEFAULT_BUILD_MANIFEST
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--campaign", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    library_dir = args.library_dir.resolve()

    if args.stage == "preflight":
        if args.campaign is not None:
            raise SystemExit("--campaign is invalid for a new preflight")
        campaign = new_campaign(args.output)
        try:
            manifest = run_preflight(
                campaign,
                binary,
                library_dir,
                args.build_manifest.resolve(),
            )
        finally:
            base.restore_invoking_user_ownership(campaign)
        print(campaign)
        return 0 if manifest["status"] == "PASS" else 1

    if os.geteuid() != 0:
        raise SystemExit("worker-400 MiniNDN stage must execute as root")
    if args.output is not None or args.campaign is None:
        raise SystemExit("worker-400 requires --campaign only")
    campaign = args.campaign.resolve()
    try:
        terminal = run_worker_stage(campaign, binary, library_dir)
    finally:
        base.restore_invoking_user_ownership(campaign)
    print(campaign)
    return 0 if terminal["diagnosisStatus"] in {"DIAGNOSED", "INCONCLUSIVE"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
