#!/usr/bin/env python3
"""Spec 142: NDN-SVS publication-worker validation under the NDNSF profile."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import time
from typing import Any, Iterator

try:
    from . import NDN_SVS_RSA_Single_Worker_Minindn as base
except ImportError:
    import NDN_SVS_RSA_Single_Worker_Minindn as base


REPO = Path(__file__).resolve().parents[1]
RESULT_ROOT = REPO / "results/spec142-svs-ndnsf-runtime-profile"
DEFAULT_BINARY = (
    REPO / "build/spec142-svs-ndnsf-runtime-profile-r4/svs-ndnsf-profile-worker"
)
DEFAULT_LIBRARY_DIR = Path("/home/tianxing/NDN/ndn-svs/build")
DEFAULT_BUILD_MANIFEST = (
    REPO / "build/spec142-svs-ndnsf-runtime-profile-r4/build-manifest.json"
)

RUNTIME_PROFILE_NAME = "ndnsf-v3"
PEER_SUMMARY_SCHEMA = "spec142.peer-summary.v1"
CELL_TERMINAL_SCHEMA = "spec142.cell-terminal.v1"
RUNTIME_MANIFEST_SCHEMA = "spec142.runtime-profile-manifest.v1"
QUALIFICATION_VERDICT_SCHEMA = "spec142.qualification-verdict.v1"
TIMING = (10, 60, 10)
QUALIFICATION_MATRIX = (
    ("face-inline-rsa", 400),
    ("worker-rsa", 400),
)
FORMAL_MATRIX = (
    ("face-inline-rsa", 600),
    ("worker-rsa", 600),
    ("face-inline-rsa", 800),
    ("worker-rsa", 800),
)
MAX_PACING_ERROR = 0.02
MIN_DELIVERY_RATIO = 0.98

PROFILE_FIELDS = (
    "runtimeProfile",
    "protocolVersion",
    "syncInterestLifetimeMs",
    "syncSuppressionMs",
    "periodicSyncMs",
    "useTimestamp",
    "applicationPayloadBytes",
    "maxPiggyDataSize",
    "maxApplicationParametersSize",
    "mappingFetchWindow",
    "mappingFetchRetries",
    "mappingFetchFailureBackoffMs",
    "publicationFetchWindow",
    "publicationFetchRetries",
    "publicationFetchInnerRetries",
    "publicationFetchInterestLifetimeMs",
    "publicationFetchMinInterestLifetimeMs",
    "publicationFetchMaxInterestLifetimeMs",
    "publicationFetchFailureBackoffMs",
    "publicationFetchMaxBackoffMs",
    "parallelSyncProcessing",
    "parallelSyncProcessingWorkers",
    "parallelSyncProcessingQueue",
    "parallelSyncProduction",
    "parallelSyncProductionWorkers",
    "parallelSyncProductionQueue",
    "parallelSyncProductionSigning",
    "parallelSyncProductionExtraBlock",
    "syncInterestBatching",
    "syncInterestBatchWindowMs",
    "publicationWorkerQueueCapacity",
)


def sha256(path: Path) -> str:
    return base.sha256(path)


def adaptive_fetch_window(rate: int) -> int:
    return max(32, min(128, (int(rate) * 64 + 99) // 100))


def expected_profile(rate: int) -> dict[str, object]:
    return {
        "runtimeProfile": RUNTIME_PROFILE_NAME,
        "protocolVersion": 3,
        "syncInterestLifetimeMs": 1000,
        "syncSuppressionMs": 1,
        "periodicSyncMs": 30000,
        "useTimestamp": False,
        "applicationPayloadBytes": 256,
        "maxPiggyDataSize": 800,
        "maxApplicationParametersSize": 4096,
        "mappingFetchWindow": 10,
        "mappingFetchRetries": 0,
        "mappingFetchFailureBackoffMs": 200,
        "publicationFetchWindow": adaptive_fetch_window(rate),
        "publicationFetchRetries": 2,
        "publicationFetchInnerRetries": 2,
        "publicationFetchInterestLifetimeMs": 500,
        "publicationFetchMinInterestLifetimeMs": 250,
        "publicationFetchMaxInterestLifetimeMs": 2000,
        "publicationFetchFailureBackoffMs": 50,
        "publicationFetchMaxBackoffMs": 2000,
        "parallelSyncProcessing": True,
        "parallelSyncProcessingWorkers": 4,
        "parallelSyncProcessingQueue": 256,
        "parallelSyncProduction": True,
        "parallelSyncProductionWorkers": 4,
        "parallelSyncProductionQueue": 256,
        "parallelSyncProductionSigning": False,
        "parallelSyncProductionExtraBlock": True,
        "syncInterestBatching": False,
        "syncInterestBatchWindowMs": 0,
        "publicationWorkerQueueCapacity": 4096,
    }


def mode_independent_profile_differences(
    inline: dict[str, Any], worker: dict[str, Any]
) -> list[str]:
    differences = []
    for key in PROFILE_FIELDS:
        if inline.get(key) != worker.get(key):
            differences.append(
                f"{key}: inline={inline.get(key)!r} worker={worker.get(key)!r}"
            )
    return differences


def _profile_error(peer: str, key: str, actual: object, expected: object) -> str:
    return (
        f"PROFILE_INVALID:{peer}:{key}={actual!r} expected={expected!r}"
    )


def validate_peer_profile(
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
            _profile_error(
                peer, "schema", summary.get("schema"), expected_schema
            )
        )
    for key, expected in expected_profile(rate).items():
        if summary.get(key) != expected:
            errors.append(_profile_error(peer, key, summary.get(key), expected))

    expected_workers = 0 if mode == "face-inline-rsa" else 1
    if summary.get("publicationWorkers") != expected_workers:
        errors.append(
            _profile_error(
                peer,
                "publicationWorkers",
                summary.get("publicationWorkers"),
                expected_workers,
            )
        )

    scheduled = rate * measure
    attempted = int(summary.get("attemptedMeasured", 0))
    if summary.get("scheduledMeasured") != scheduled:
        errors.append(
            f"{peer}:scheduledMeasured={summary.get('scheduledMeasured')!r} "
            f"expected={scheduled}"
        )
    attempted_ratio = attempted / scheduled if scheduled else 0.0
    if not 1.0 - MAX_PACING_ERROR <= attempted_ratio <= 1.0 + MAX_PACING_ERROR:
        errors.append(
            f"{peer}:attempted-ratio={attempted_ratio:.6f} outside "
            f"[{1.0 - MAX_PACING_ERROR:.2f},{1.0 + MAX_PACING_ERROR:.2f}]"
        )
    delivered = int(summary.get("deliveredMeasured", 0))
    delivery_ratio = delivered / attempted if attempted else 0.0
    if delivery_ratio < MIN_DELIVERY_RATIO:
        errors.append(
            f"LOAD_UNSUSTAINED:{peer}:delivery-ratio={delivery_ratio:.6f} "
            f"below {MIN_DELIVERY_RATIO:.2f}"
        )

    face_thread = int(summary.get("faceThreadHash", 0))
    pacer_thread = int(summary.get("pacerThreadHash", 0))
    call_thread = int(summary.get("publishCallThreadHash", 0))
    if face_thread == 0 or pacer_thread == 0 or face_thread == pacer_thread:
        errors.append(f"{peer}:Face/APP pacer thread identity invalid")
    if summary.get("pacerFailed") is not False:
        errors.append(f"{peer}:pacer failed: {summary.get('pacerError', '')}")
    if mode == "face-inline-rsa":
        if call_thread != face_thread or int(summary.get("publishCallsOnFace", 0)) <= 0:
            errors.append(f"{peer}:control publication was not called on Face")
        if int(summary.get("publishCallsOnPacer", 0)) != 0:
            errors.append(f"{peer}:control publication was called on APP pacer")
    else:
        if call_thread != pacer_thread or int(summary.get("publishCallsOnPacer", 0)) <= 0:
            errors.append(f"{peer}:worker publication was not called on APP pacer")
        if int(summary.get("publishCallsOnFace", 0)) != 0:
            errors.append(f"{peer}:worker publication was called on Face")

    if summary.get("dataSignatureType") != 1:
        errors.append(f"{peer}:publication Data is not RSA signed")
    if summary.get("syncEnvelopeSignatureType") != 1:
        errors.append(f"{peer}:V3 Sync envelope Data is not RSA signed")
    if (
        summary.get("interestSignatureType") != 0
        or summary.get("syncInterestSigned") is not False
    ):
        errors.append(
            f"{peer}:V3 Sync Interest was incorrectly treated as a V2 signed Interest"
        )
    if int(summary.get("dataValid", 0)) <= 0:
        errors.append(f"{peer}:RSA Data validation evidence is missing")
    for key in ("dataInvalid", "interestInvalid", "invalid"):
        if int(summary.get(key, 0)) != 0:
            errors.append(f"{peer}:{key}={summary.get(key)}")
    if int(summary.get("maxActiveSigners", 0)) != 1:
        errors.append(f"{peer}:serialized RSA signer ownership not proved")
    if int(summary.get("workerOutstanding", 0)) != 0:
        errors.append(f"{peer}:workerOutstanding is nonzero at drain end")
    if int(summary.get("faceDispatchAbandoned", 0)) != 0:
        errors.append(f"{peer}:Face publication calls were abandoned")

    for key in (
        "signedPublicationWireBytesCount",
        "signedPublicationWireBytesTotal",
        "signedPublicationWireBytesMax",
    ):
        if int(summary.get(key, 0)) <= 0:
            errors.append(f"{peer}:{key} evidence is missing")

    for prefix in ("publicationFetch", "mappingFetch"):
        for metric in ("Retries", "Nacks", "Timeouts"):
            start_key = f"{prefix}{metric}AtMeasureStart"
            end_key = f"{prefix}{metric}AtMeasureEnd"
            try:
                start = int(summary[start_key])
                end = int(summary[end_key])
            except (KeyError, TypeError, ValueError):
                errors.append(
                    _profile_error(
                        peer,
                        f"{prefix}{metric}DuringMeasure",
                        "missing-boundary",
                        0,
                    )
                )
                continue
            delta = end - start
            if delta != 0:
                errors.append(
                    _profile_error(
                        peer, f"{prefix}{metric}DuringMeasure", delta, 0
                    )
                )
    return errors


def classify_terminal(
    *, error: str, admission_errors: list[str]
) -> dict[str, object]:
    profile_errors = [
        item for item in admission_errors if item.startswith("PROFILE_INVALID:")
    ]
    load_errors = [
        item for item in admission_errors if item.startswith("LOAD_UNSUSTAINED:")
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
    outcome = "LOAD_UNSUSTAINED" if load_errors else "COMPLETE"
    return {
        "validity": validity,
        "outcome": outcome,
        "profileErrors": profile_errors,
        "harnessErrors": harness_errors,
        "loadErrors": load_errors,
    }


def require_formal_authorization(campaign: Path, manifest_sha: str) -> None:
    verdict_path = campaign / "qualification-verdict.json"
    if not verdict_path.is_file():
        raise RuntimeError("formal stage requires a qualification verdict")
    verdict = json.loads(verdict_path.read_text(encoding="utf-8"))
    if (
        verdict.get("schema") != QUALIFICATION_VERDICT_SCHEMA
        or verdict.get("status") != "PASS"
    ):
        raise RuntimeError("formal stage requires a passed qualification")
    if verdict.get("runtimeProfileManifestSha256") != manifest_sha:
        raise RuntimeError("qualification manifest does not match runtime manifest")


def peer_arguments() -> tuple[str, ...]:
    return ("--runtime-profile", RUNTIME_PROFILE_NAME)


@contextmanager
def stage_lock(campaign: Path, stage: str) -> Iterator[None]:
    lock = campaign / f".{stage}.lock"
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    try:
        os.write(descriptor, f"{os.getpid()}\n".encode("ascii"))
        os.close(descriptor)
        descriptor = -1
        yield
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        lock.unlink(missing_ok=True)


def validate_build_identity(
    binary: Path, library: Path, build_manifest: Path
) -> dict[str, Any]:
    if not binary.is_file() or not library.is_file() or not build_manifest.is_file():
        raise RuntimeError("binary, runtime library, or build manifest is missing")
    record = json.loads(build_manifest.read_text(encoding="utf-8"))
    if record.get("schema") != "spec142.build-manifest.v1":
        raise RuntimeError("unexpected Spec 142 build manifest schema")
    if record.get("binarySha256") != sha256(binary):
        raise RuntimeError("binary identity differs from build manifest")
    if record.get("librarySha256") != sha256(library):
        raise RuntimeError("runtime NDN-SVS library differs from build manifest")
    linkage_path = Path(record["linkage"]["path"])
    if not linkage_path.is_file() or record["linkage"]["sha256"] != sha256(linkage_path):
        raise RuntimeError("linkage receipt differs from build manifest")
    linkage = subprocess.run(
        ["ldd", str(binary)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout
    if str(library.resolve()) not in linkage:
        raise RuntimeError("benchmark does not resolve the frozen NDN-SVS library")
    return record


def _local_pair(
    directory: Path,
    binary: Path,
    library_dir: Path,
    arguments: dict[str, list[str]],
    timeout: float,
) -> dict[str, dict[str, Any]]:
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = str(library_dir)
    environments = {peer: environment for peer in base.PEERS}
    return_codes = base.run_local_pair(
        directory,
        {peer: [str(binary), *arguments[peer]] for peer in base.PEERS},
        environments,
        timeout,
    )
    summaries = {}
    for peer in base.PEERS:
        if return_codes[peer] != 0:
            raise RuntimeError(f"{directory.name}:{peer}:return code {return_codes[peer]}")
        path = directory / f"{peer}-summary.json"
        if not path.is_file():
            raise RuntimeError(f"{directory.name}:{peer}:summary missing")
        summaries[peer] = json.loads(path.read_text(encoding="utf-8"))
    return summaries


def run_preflight(
    campaign: Path,
    binary: Path,
    library_dir: Path,
    build_manifest: Path,
) -> dict[str, Any]:
    library = library_dir / "libndn-svs.so"
    build = validate_build_identity(binary, library, build_manifest)
    profile_probes: dict[str, dict[str, Any]] = {}
    pacer_probes: dict[str, dict[str, Any]] = {}
    errors: list[str] = []

    for mode in ("face-inline-rsa", "worker-rsa"):
        profile_dir = campaign / f"profile-{mode}"
        profile_args = {
            peer: [
                "--profile-only",
                "--runtime-profile",
                RUNTIME_PROFILE_NAME,
                "--mode",
                mode,
                "--peer-id",
                peer,
                "--rate",
                "800",
                "--summary",
                str(profile_dir / f"{peer}-summary.json"),
                "--summary-schema",
                PEER_SUMMARY_SCHEMA,
            ]
            for peer in base.PEERS
        }
        summaries = _local_pair(
            profile_dir, binary, library_dir, profile_args, timeout=20
        )
        for peer, summary in summaries.items():
            profile_probes[f"{mode}:{peer}"] = summary
            for key, expected in expected_profile(800).items():
                if summary.get(key) != expected:
                    errors.append(_profile_error(peer, key, summary.get(key), expected))

        pacer_dir = campaign / f"pacer-{mode}-800"
        pacer_args = {
            peer: [
                "--pacer-only",
                "--mode",
                mode,
                "--peer-id",
                peer,
                "--rate",
                "800",
                "--warmup",
                "1",
                "--measure",
                "60",
                "--summary",
                str(pacer_dir / f"{peer}-summary.json"),
            ]
            for peer in base.PEERS
        }
        pacers = _local_pair(
            pacer_dir, binary, library_dir, pacer_args, timeout=75
        )
        for peer, summary in pacers.items():
            pacer_probes[f"{mode}:{peer}"] = summary
            expected = 800 * 60
            attempted = int(summary.get("attemptedMeasured", 0))
            if summary.get("passed") is not True:
                errors.append(f"{mode}:{peer}:pacer preflight failed")
            if not 0.98 <= attempted / expected <= 1.02:
                errors.append(
                    f"{mode}:{peer}:attempted={attempted} expected={expected}"
                )

    inline = profile_probes["face-inline-rsa:peer-a"]
    worker = profile_probes["worker-rsa:peer-a"]
    errors.extend(mode_independent_profile_differences(inline, worker))
    manifest = {
        "schema": RUNTIME_MANIFEST_SCHEMA,
        "createdUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "status": "PASS" if not errors else "FAIL",
        "errors": errors,
        "runtimeProfile": RUNTIME_PROFILE_NAME,
        "binary": str(binary),
        "binarySha256": sha256(binary),
        "library": str(library),
        "librarySha256": sha256(library),
        "buildManifest": str(build_manifest),
        "buildManifestSha256": sha256(build_manifest),
        "build": build,
        "profileProbes": profile_probes,
        "pacerProbes": pacer_probes,
        "topology": {
            "nodes": 2,
            "bandwidthMbps": 100,
            "oneWayDelayMs": 10,
            "configuredLossPct": 0,
            "bothPeersPublishAndSubscribe": True,
        },
        "timing": {"warmup": 10, "measure": 60, "drain": 10},
        "cpuAffinity": sorted(os.sched_getaffinity(0)),
        "qualificationMatrix": [
            {"ordinal": ordinal, "mode": mode, "ratePerPeer": rate}
            for ordinal, (mode, rate) in enumerate(QUALIFICATION_MATRIX, 1)
        ],
        "formalMatrix": [
            {"ordinal": ordinal, "mode": mode, "ratePerPeer": rate}
            for ordinal, (mode, rate) in enumerate(FORMAL_MATRIX, 3)
        ],
    }
    base.write_json(campaign / "runtime-profile-manifest.json", manifest)
    return manifest


def run_stage(
    campaign: Path,
    binary: Path,
    library_dir: Path,
    stage: str,
) -> list[dict[str, Any]]:
    manifest_path = campaign / "runtime-profile-manifest.json"
    if not manifest_path.is_file():
        raise RuntimeError("runtime-profile-manifest.json is missing")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("status") != "PASS":
        raise RuntimeError("runtime profile preflight did not pass")
    manifest_sha = sha256(manifest_path)
    if sha256(binary) != manifest.get("binarySha256"):
        raise RuntimeError("binary differs from runtime profile manifest")
    library = library_dir / "libndn-svs.so"
    if sha256(library) != manifest.get("librarySha256"):
        raise RuntimeError("library differs from runtime profile manifest")

    if stage == "qualification":
        matrix = QUALIFICATION_MATRIX
        start_ordinal = 1
    elif stage == "formal":
        require_formal_authorization(campaign, manifest_sha)
        matrix = FORMAL_MATRIX
        start_ordinal = 3
    else:
        raise RuntimeError(f"unsupported stage: {stage}")

    terminals: list[dict[str, Any]] = []
    with stage_lock(campaign, stage):
        terminal_path = campaign / f"{stage}-terminals.json"
        if terminal_path.exists():
            raise RuntimeError(f"{stage} stage already has terminal evidence")
        for offset, (mode, rate) in enumerate(matrix):
            terminal = base.run_cell(
                campaign,
                binary,
                library_dir,
                start_ordinal + offset,
                mode,
                rate,
                TIMING,
                experiment_namespace="spec142",
                summary_schema=PEER_SUMMARY_SCHEMA,
                record_delivery_samples=True,
                extra_peer_arguments=peer_arguments(),
                admission_validator=validate_peer_profile,
                terminal_schema=CELL_TERMINAL_SCHEMA,
                terminal_classifier=classify_terminal,
            )
            terminals.append(terminal)
        base.write_json(terminal_path, terminals)

        if stage == "qualification":
            passed = all(
                terminal.get("validity") == "PROFILE_VALID"
                for terminal in terminals
            )
            verdict = {
                "schema": QUALIFICATION_VERDICT_SCHEMA,
                "status": "PASS" if passed else "FAIL",
                "runtimeProfileManifestSha256": manifest_sha,
                "terminalPath": str(terminal_path),
                "terminalSha256": sha256(terminal_path),
                "validity": [
                    {
                        "cellId": terminal["cellId"],
                        "validity": terminal.get("validity"),
                        "outcome": terminal.get("outcome"),
                    }
                    for terminal in terminals
                ],
            }
            base.write_json(campaign / "qualification-verdict.json", verdict)
    return terminals


def _new_campaign(output: Path | None) -> Path:
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    campaign = (
        output.resolve()
        if output is not None
        else RESULT_ROOT / f"campaign-{stamp}"
    )
    campaign.mkdir(parents=True, exist_ok=False)
    return campaign


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--stage", choices=("preflight", "qualification", "formal"), required=True
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
            raise SystemExit("--campaign is not valid for a new preflight")
        campaign = _new_campaign(args.output)
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
        raise SystemExit("MiniNDN stages must execute as root")
    if args.output is not None or args.campaign is None:
        raise SystemExit("qualification/formal requires --campaign only")
    campaign = args.campaign.resolve()
    try:
        terminals = run_stage(
            campaign, binary, library_dir, args.stage
        )
    finally:
        base.restore_invoking_user_ownership(campaign)
    print(campaign)
    return 0 if all(
        terminal.get("validity") == "PROFILE_VALID" for terminal in terminals
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
