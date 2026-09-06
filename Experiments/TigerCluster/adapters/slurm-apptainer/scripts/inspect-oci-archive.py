#!/usr/bin/env python3
"""Validate an OCI archive and print its single platform manifest digest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import PurePosixPath
import re
import tarfile


DIGEST = re.compile(r"^sha256:([a-f0-9]{64})$")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive")
    args = parser.parse_args()
    with tarfile.open(args.archive, "r") as archive:
        names = {member.name: member for member in archive.getmembers()}
        for name in names:
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts:
                raise SystemExit("OCI_ARCHIVE_UNSAFE_PATH")
        index_member = names.get("index.json")
        if index_member is None or not index_member.isfile():
            raise SystemExit("OCI_ARCHIVE_INDEX_MISSING")
        index_stream = archive.extractfile(index_member)
        if index_stream is None:
            raise SystemExit("OCI_ARCHIVE_INDEX_UNREADABLE")
        index = json.load(index_stream)
        manifests = index.get("manifests")
        if not isinstance(manifests, list) or len(manifests) != 1:
            raise SystemExit("OCI_ARCHIVE_MANIFEST_COUNT_INVALID")
        digest = manifests[0].get("digest", "")
        match = DIGEST.fullmatch(digest)
        if match is None:
            raise SystemExit("OCI_ARCHIVE_MANIFEST_DIGEST_INVALID")
        blob_name = f"blobs/sha256/{match.group(1)}"
        blob_member = names.get(blob_name)
        if blob_member is None or not blob_member.isfile():
            raise SystemExit("OCI_ARCHIVE_MANIFEST_BLOB_MISSING")
        blob = archive.extractfile(blob_member)
        if blob is None:
            raise SystemExit("OCI_ARCHIVE_MANIFEST_BLOB_UNREADABLE")
        actual = hashlib.sha256()
        for chunk in iter(lambda: blob.read(1024 * 1024), b""):
            actual.update(chunk)
        if actual.hexdigest() != match.group(1):
            raise SystemExit("OCI_ARCHIVE_MANIFEST_BLOB_TAMPERED")
    print(digest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
