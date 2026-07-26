#!/usr/bin/env python3
"""Build and verify the Spec 142 NDNSF-profile worker benchmark."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

try:
    from . import build_svs_rsa_single_worker as base
except ImportError:
    import build_svs_rsa_single_worker as base


REPO = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPO / "build/spec142-svs-ndnsf-runtime-profile-r4"
RUNNER = REPO / "Experiments/NDN_SVS_NDNSF_Profile_Worker_Minindn.py"
SCHEMA = "spec142.build-manifest.v1"
SVS_SOURCES = (
    base.SVS_REPO / "ndn-svs/fetcher.hpp",
    base.SVS_REPO / "ndn-svs/fetcher.cpp",
    base.SVS_REPO / "ndn-svs/svspubsub.hpp",
    base.SVS_REPO / "ndn-svs/svspubsub.cpp",
)


def enrich_manifest(manifest: Path) -> None:
    record = json.loads(manifest.read_text(encoding="utf-8"))
    if record.get("schema") != SCHEMA:
        raise RuntimeError("unexpected Spec 142 build manifest schema")
    record["ndnSvsRuntimeSources"] = {
        path.name + ":" + path.parent.name: base.source_record(path)
        for path in SVS_SOURCES
    }
    base.write_json(manifest, record)


def verify(manifest: Path) -> None:
    record = json.loads(manifest.read_text(encoding="utf-8"))
    if record.get("schema") != SCHEMA:
        raise RuntimeError("unexpected Spec 142 build manifest schema")
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
    print(f"SPEC142_BUILD_VERIFY_OK {manifest}")


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
            binary_name="svs-ndnsf-profile-worker",
            runner=RUNNER,
            analyzer=None,
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
