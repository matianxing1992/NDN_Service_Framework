#!/usr/bin/env python3
"""Build and freeze the Spec 143 diagnostic binary and NDN-SVS library."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

try:
    from . import build_svs_rsa_single_worker as base
except ImportError:
    import build_svs_rsa_single_worker as base


REPO = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPO / "build/spec143-svs-zero-loss-fetch-causality"
RUNNER = REPO / "Experiments/NDN_SVS_Zero_Loss_Fetch_Causality_Minindn.py"
ANALYZER = REPO / "Experiments/analyze_svs_zero_loss_fetch_causality.py"
SCHEMA = "spec143.build-manifest.v1"
SVS_SOURCES = tuple(
    base.SVS_REPO / relative
    for relative in (
        "ndn-svs/fetcher.hpp",
        "ndn-svs/fetcher.cpp",
        "ndn-svs/mapping-provider.cpp",
        "ndn-svs/svsync-base.cpp",
        "ndn-svs/svspubsub.cpp",
    )
)


def enrich_manifest(manifest: Path) -> None:
    record = json.loads(manifest.read_text(encoding="utf-8"))
    if record.get("schema") != SCHEMA:
        raise RuntimeError("unexpected Spec 143 build manifest schema")
    record["ndnSvsRuntimeSources"] = {
        path.name: base.source_record(path) for path in SVS_SOURCES
    }
    base.write_json(manifest, record)


def verify(manifest: Path) -> None:
    record = json.loads(manifest.read_text(encoding="utf-8"))
    if record.get("schema") != SCHEMA:
        raise RuntimeError("unexpected Spec 143 build manifest schema")
    checks = [
        (Path(record["binary"]), record["binarySha256"], "binary"),
        (Path(record["library"]), record["librarySha256"], "library"),
        (
            Path(record["buildLog"]["path"]),
            record["buildLog"]["sha256"],
            "build log",
        ),
        (
            Path(record["linkage"]["path"]),
            record["linkage"]["sha256"],
            "linkage",
        ),
    ]
    for source in record["sources"].values():
        checks.append((Path(source["path"]), source["sha256"], "source"))
    for source in record["ndnSvsRuntimeSources"].values():
        checks.append((Path(source["path"]), source["sha256"], "NDN-SVS source"))
    for path, expected, label in checks:
        if not path.is_file() or base.sha256_file(path) != expected:
            raise RuntimeError(f"{label} identity changed: {path}")
    linkage = base.normalized_linkage(Path(record["binary"]))
    if base.sha256_bytes(linkage.encode("utf-8")) != base.sha256_file(
        Path(record["linkage"]["path"])
    ):
        raise RuntimeError("resolved linkage changed")
    print(f"SPEC143_BUILD_VERIFY_OK {manifest}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("build", "verify"))
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--library-dir", type=Path, default=base.SVS_REPO / "build"
    )
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    if args.command == "build":
        manifest = base.build(
            args.output,
            args.library_dir,
            manifest_schema=SCHEMA,
            binary_name="svs-zero-loss-fetch-causality",
            runner=RUNNER,
            analyzer=ANALYZER,
            builder=Path(__file__).resolve(),
        )
        enrich_manifest(manifest)
        verify(manifest)
    else:
        verify(
            args.manifest.resolve()
            if args.manifest
            else (args.output / "build-manifest.json").resolve()
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
