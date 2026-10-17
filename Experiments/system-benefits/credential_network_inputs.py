#!/usr/bin/env python3
"""Reuse native archive qualification only while every original input remains identical."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ORACLE = Path("/usr/local/bin/ndnsf-credential-archive-oracle")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def archive_hashes(archive):
    paths = list(archive.rglob("*"))
    if archive.is_symlink() or any(p.is_symlink() for p in paths):
        raise ValueError("archive symlinks not accepted")
    return {str(p.relative_to(archive)): digest(p) for p in sorted(paths) if p.is_file()}


def oracle_identity():
    closure = subprocess.check_output(["ldd", str(ORACLE)], text=True,
                                     env=dict(os.environ, LD_LIBRARY_PATH="/usr/local/lib"))
    if "not found" in closure or str(HERE.parents[1]) in closure:
        raise ValueError("oracle must use a complete installed library closure")
    libraries = {str(Path(word).resolve()): digest(word) for word in closure.split()
                 if word.startswith("/") and Path(word).is_file()}
    if not libraries:
        raise ValueError("empty oracle library closure")
    return {"oracle_binary_sha256": digest(ORACLE), "receiver_sha256": digest(HERE / "credential-receiver.hpp"),
            "oracle_source_sha256": digest(HERE / "credential-archive-oracle.cpp"), "libraries": libraries}


def verify_qualification(archive, repetitions, receipt):
    value = json.loads(receipt.read_text())
    raw = receipt.parent / "archive-oracle.jsonl"
    if (value.get("schema") != "ndnsf-credential-input-qualification-v1" or
            value.get("archive") != str(archive.resolve()) or value.get("repetitions") != repetitions or
            value.get("oracle_exit") != 0 or value.get("oracle_identity") != oracle_identity() or
            value.get("raw_sha256") != digest(raw) or value.get("archive_hashes") != archive_hashes(archive)):
        raise ValueError("native archive qualification identity mismatch")
    data = [json.loads(line) for line in raw.read_text().splitlines()]
    if len(data) != 1 or data[0].get("oracle") is not True or data[0].get("cases") != 9 * repetitions:
        raise ValueError("native archive oracle matrix incomplete")
    return value


def qualify(archive, repetitions, directory):
    directory.mkdir(parents=True, exist_ok=False)
    before = archive_hashes(archive)
    identity = oracle_identity()
    with (directory / "archive-oracle.jsonl").open("wb") as out, (directory / "archive-oracle.log").open("wb") as err:
        subprocess.run([str(ORACLE), str(archive), str(repetitions)], stdout=out, stderr=err, check=True, timeout=600,
                       env=dict(os.environ, LD_LIBRARY_PATH="/usr/local/lib"))
    if before != archive_hashes(archive) or identity != oracle_identity():
        raise ValueError("archive/oracle identity changed during native qualification")
    receipt = directory / "qualification.json"
    value = {"schema": "ndnsf-credential-input-qualification-v1", "archive": str(archive.resolve()),
             "repetitions": repetitions, "oracle_exit": 0, "archive_hashes": before,
             "oracle_identity": identity,
             "raw_sha256": digest(directory / "archive-oracle.jsonl"),
             "scope": "native offline archive matrix; actual network qualification is per cell"}
    receipt.write_text(json.dumps(value, indent=2) + "\n")
    verify_qualification(archive, repetitions, receipt)
    return receipt
