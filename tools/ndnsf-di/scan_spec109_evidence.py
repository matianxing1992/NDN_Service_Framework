#!/usr/bin/env python3
"""Bounded text scanner for credentials and unrestricted payload evidence."""
from __future__ import annotations

import argparse
import io
import json
from pathlib import Path
import re
import tarfile


PATTERNS = {
    "private-key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "bearer-token": re.compile(r"(?i)authorization\s*[:=]\s*bearer\s+\S+"),
    "hf-token": re.compile(r"\bhf_[A-Za-z0-9]{20,}\b"),
    "password-field": re.compile(r"(?i)\b(?:password|passwd|mfa)\s*[:=]\s*[^\s,}]{4,}"),
    "raw-prompt-field": re.compile(r'(?i)["\'](?:prompt|tensor|tokenPayload)["\']\s*:\s*["\'][^"\']+'),
}
SKIP_SUFFIXES = {".safetensors", ".onnx", ".sif", ".bin", ".pt", ".pth"}
SYNTHETIC_FIXTURE_PATHS = {
    "tests/container/itiger-qwen/unit/test_redaction.py",
}


def _is_synthetic_fixture(path: str) -> bool:
    normalized = path.replace("\\", "/")
    return any(normalized.endswith(value) for value in SYNTHETIC_FIXTURE_PATHS)


def _scan_text(text: str, display_path: str, findings: list[dict]) -> None:
    for kind, pattern in PATTERNS.items():
        for match in pattern.finditer(text):
            findings.append({"path": display_path, "kind": kind,
                             "line": text.count("\n", 0, match.start()) + 1})


def scan_paths(paths: list[Path], maximum_bytes: int = 8 * 1024 * 1024) -> dict:
    findings = []
    scanned = 0; skipped = 0; skipped_fixtures = 0; scanned_archives = 0
    for root in paths:
        candidates = [root] if root.is_file() else sorted(p for p in root.rglob("*") if p.is_file())
        for path in candidates:
            if _is_synthetic_fixture(path.as_posix()):
                skipped_fixtures += 1; continue
            if path.suffix.lower() in SKIP_SUFFIXES or path.stat().st_size > maximum_bytes:
                skipped += 1; continue
            if tarfile.is_tarfile(path):
                scanned_archives += 1
                try:
                    with tarfile.open(path, mode="r:*") as archive:
                        for member in archive.getmembers():
                            if not member.isfile():
                                continue
                            if _is_synthetic_fixture(member.name):
                                skipped_fixtures += 1; continue
                            if member.size > maximum_bytes:
                                skipped += 1; continue
                            stream = archive.extractfile(member)
                            if stream is None:
                                skipped += 1; continue
                            try:
                                text = io.TextIOWrapper(stream, encoding="utf-8").read()
                            except (UnicodeError, OSError):
                                skipped += 1; continue
                            scanned += 1
                            _scan_text(text, f"{path}!{member.name}", findings)
                except (OSError, tarfile.TarError):
                    skipped += 1
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeError, OSError):
                skipped += 1; continue
            scanned += 1
            _scan_text(text, str(path), findings)
    return {"schemaVersion": "1.0", "status": "PASS" if not findings else "FAIL",
            "scannedFiles": scanned, "skippedBinaryOrLargeFiles": skipped,
            "skippedSyntheticFixtures": skipped_fixtures,
            "scannedArchives": scanned_archives,
            "findings": findings, "redacted": True}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--path", action="append", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    report = scan_paths([Path(value) for value in args.path])
    target = Path(args.output); target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "findings": len(report["findings"])}))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
