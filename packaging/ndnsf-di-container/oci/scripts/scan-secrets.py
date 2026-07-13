#!/usr/bin/env python3
"""Streaming redacted secret scanner for OCI, SIF, profiles, logs, and evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile
from typing import Iterable


PATTERNS = {
    "private-key": re.compile(br"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----", re.I),
    "bearer-token": re.compile(br"authorization\s*[:=]\s*bearer\s+[^\s]+", re.I),
    "huggingface-token": re.compile(br"\bhf_[A-Za-z0-9]{20,}\b"),
    "aws-secret": re.compile(br"AWS_SECRET_ACCESS_KEY\s*[:=]\s*[^\s,}]+", re.I),
    "password-field": re.compile(br"(?:password|passwd|passphrase|mfa)\s*[\"']?\s*[:=]\s*[\"']?[^\s,}\"']{4,}", re.I),
    "ndnsf-token-material": re.compile(br"(?:userToken|providerToken)\s*[\"']?\s*[:=]\s*[\"'][^\"']+", re.I),
}
PAYLOAD_PATTERNS = {
    "raw-prompt": re.compile(br"[\"']prompt[\"']\s*:\s*[\"'][^\"']+[\"']", re.I),
    "raw-tensor": re.compile(br"[\"'](?:tensor|tensorPayload)[\"']\s*:\s*[\"'][^\"']+[\"']", re.I),
}
FORBIDDEN_NAMES = re.compile(
    r"(?:^|/)(?:\.env(?:\..*)?|id_rsa|credentials)(?:$|/)|\.(?:key|pem|p12|pfx)$",
    re.I,
)
CHUNK_BYTES = 1024 * 1024
OVERLAP_BYTES = 4096


def _finding(path: str, kind: str, offset: int) -> dict[str, object]:
    identity = hashlib.sha256(f"{path}\0{kind}\0{offset}".encode()).hexdigest()
    return {"path": path, "kind": kind, "offset": offset, "findingId": "sha256:" + identity}


SCOPES = (
    "artifact",
    "profile",
    "log",
    "evidence",
    "source",
    "release-evidence",
)


def _patterns(scope: str):
    result = dict(PATTERNS)
    if scope in {"log", "evidence", "release-evidence"}:
        result.update(PAYLOAD_PATTERNS)
    return result


def _scan_chunks(chunks: Iterable[bytes], display_path: str, scope: str):
    findings = []
    tail = b""
    consumed = 0
    for chunk in chunks:
        combined = tail + chunk
        base = max(0, consumed - len(tail))
        for kind, pattern in _patterns(scope).items():
            for match in pattern.finditer(combined):
                absolute = base + match.start()
                if absolute < consumed - len(tail):
                    continue
                findings.append(_finding(display_path, kind, absolute))
        consumed += len(chunk)
        tail = combined[-OVERLAP_BYTES:]
    unique = {(row["path"], row["kind"], row["offset"]): row for row in findings}
    return list(unique.values()), consumed


def _file_chunks(path: Path):
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(CHUNK_BYTES), b""):
            yield chunk


def scan(paths: Iterable[Path], *, scope: str) -> dict[str, object]:
    findings = []
    files = 0
    bytes_scanned = 0
    archives = 0
    for root in paths:
        candidates = [root] if root.is_file() else sorted(item for item in root.rglob("*") if item.is_file())
        for path in candidates:
            display = path.as_posix()
            if FORBIDDEN_NAMES.search(display):
                findings.append(_finding(display, "forbidden-filename", 0))
            # A SIF commonly begins with long zero-filled regions that Python's
            # tar probe can misclassify as an empty archive. SIF is always a
            # raw binary scan target; OCI tar archives still use member scans.
            if path.suffix.lower() != ".sif" and tarfile.is_tarfile(path):
                archives += 1
                with tarfile.open(path, "r:*") as archive:
                    for member in archive.getmembers():
                        if not member.isfile():
                            continue
                        member_display = display + "!" + member.name
                        if FORBIDDEN_NAMES.search(member.name):
                            findings.append(_finding(member_display, "forbidden-filename", 0))
                        stream = archive.extractfile(member)
                        if stream is None:
                            continue
                        member_findings, count = _scan_chunks(iter(lambda: stream.read(CHUNK_BYTES), b""), member_display, scope)
                        findings.extend(member_findings)
                        bytes_scanned += count
                        files += 1
                continue
            file_findings, count = _scan_chunks(_file_chunks(path), display, scope)
            findings.extend(file_findings)
            bytes_scanned += count
            files += 1
    findings.sort(key=lambda row: (str(row["path"]), str(row["kind"]), int(row["offset"])))
    return {
        "schemaVersion": "spec110-secret-scan-v1",
        "status": "PASS" if not findings else "FAIL",
        "scope": scope,
        "scannedFiles": files,
        "scannedBytes": bytes_scanned,
        "scannedArchives": archives,
        "findingCount": len(findings),
        "findings": findings,
        "matchedContentRedacted": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--path", action="append", required=True)
    parser.add_argument("--scope", choices=SCOPES, required=True)
    parser.add_argument("--output")
    args = parser.parse_args()
    report = scan([Path(value) for value in args.path], scope=args.scope)
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        target = Path(args.output)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0 if report["status"] == "PASS" else 7


if __name__ == "__main__":
    raise SystemExit(main())
