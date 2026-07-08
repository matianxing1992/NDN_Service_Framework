#!/usr/bin/env python3
"""Regression checks for NDNSF stream vs large-data documentation.

This guards an important API boundary:

* streams are ongoing sequences with ordering/freshness/buffering semantics;
* large-data objects are fetched by exact NDN names through segmented retrieval.

The goal is not to lint prose. The goal is to prevent future README edits from
turning StreamChunk into a vague replacement for SegmentFetcher or repo-backed
large-data retrieval.
"""

from __future__ import annotations

from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


REQUIRED_BY_FILE = {
    "README.md": [
        "ongoing sequence with freshness/ordering/buffer state -> stream substrate",
        "known object name with complete-object retrieval        -> large-data path",
        "StreamChunk is not a generic replacement for SegmentFetcher-style",
    ],
    "pythonWrapper/README.md": [
        "Use the stream API when the application needs stream behavior",
        "the application already has a specific object name",
        "Do not wrap those complete objects in",
    ],
    "NDNSF-UAV-APP/README.md": [
        "Transfer API boundary: live downlink uses the NDNSF stream substrate",
        "Local camera recording is not a",
        "Start/Stop Video controls the live stream only",
    ],
    "NDNSF-UAV-APP/README_ch.md": [
        "传输 API 边界：实时图传使用 NDNSF streaming substrate",
        "本地录像不是 live stream",
        "Start/Stop Video 只控制实时图传",
    ],
    "NDNSF-DistributedInference/README.md": [
        "Dependency transfer boundary: DI model artifacts",
        "NDNSF large-data reference, repo materialization",
        "exact-name object retrieval",
    ],
    "NDNSF-DistributedInference/README_ch.md": [
        "Dependency transfer 边界：DI 的 model artifacts",
        "正常路径是 NDNSF",
        "不是 exact-name object retrieval 的替代机制",
    ],
}


FORBIDDEN_CASE_INSENSITIVE = [
    "streamchunk replaces segmentfetcher",
    "streamchunk replaces large-data",
    "streamchunk replaces large data",
    "streamchunk is a replacement for segmentfetcher",
    "streamchunk is a replacement for large-data",
    "recording is a live stream",
    "recording uses the live stream path",
    "tensor bundle stream replaces large-data",
    "tensor bundles should use streamchunk instead of large-data",
]


def read(path: str) -> str:
    full = REPO / path
    if not full.exists():
        raise AssertionError(f"missing required document: {path}")
    return full.read_text(encoding="utf-8")


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"{label}: missing required text: {needle}")


def reject_case_insensitive(text: str, needle: str, label: str) -> None:
    if needle.lower() in text.lower():
        raise AssertionError(f"{label}: misleading transfer-boundary text: {needle}")


def main() -> int:
    documents = {path: read(path) for path in REQUIRED_BY_FILE}

    for path, required in REQUIRED_BY_FILE.items():
        for needle in required:
            require(documents[path], needle, path)

    for path, text in documents.items():
        for needle in FORBIDDEN_CASE_INSENSITIVE:
            reject_case_insensitive(text, needle, path)

    print("NDNSF_TRANSFER_BOUNDARY_DOCUMENTATION_REGRESSION_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
