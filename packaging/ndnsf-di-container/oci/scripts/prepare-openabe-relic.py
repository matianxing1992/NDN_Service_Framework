#!/usr/bin/env python3
"""Materialize OpenABE's exact, patched RELIC tree without network access."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import tarfile


RELIC_REVISION = "b984e901ba78c83ea4093ea96addd13628c8c2d0"
REPLACEMENTS = (
    (b"MIN", b"RLC_MIN"),
    (b"MAX", b"RLC_MAX"),
    (b"ALIGN", b"RLC_ALIGN"),
    (b"rsa_t", b"rlc_rsa_t"),
    (b"rsa_st", b"rlc_rsa_st"),
)


def prepare(source: Path, openabe: Path) -> Path:
    downloader = openabe / "deps/relic/download_relic.sh"
    if not downloader.is_file() or f"COMMIT={RELIC_REVISION}" not in downloader.read_text():
        raise RuntimeError("OPENABE_RELIC_CONTRACT_MISMATCH")
    if not (source / "CMakeLists.txt").is_file():
        raise RuntimeError("SEALED_RELIC_SOURCE_INVALID")
    makefile = openabe / "deps/relic/Makefile"
    make_text = makefile.read_text()
    old_arch = '-DARCH="ARM" -DWSIZE=32'
    if make_text.count(old_arch) != 2:
        raise RuntimeError("OPENABE_RELIC_ARCH_CONTRACT_MISMATCH")
    makefile.write_text(make_text.replace(old_arch, '-DARCH="X64" -DWSIZE=64'))
    target = openabe / "deps/relic/relic-toolkit-0.5.0"
    if target.exists():
        raise RuntimeError("OPENABE_RELIC_TARGET_EXISTS")
    shutil.copytree(source, target, symlinks=True)
    for path in sorted(target.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        value = path.read_bytes()
        updated = value
        for old, new in REPLACEMENTS:
            updated = updated.replace(old, new)
        if updated != value:
            path.write_bytes(updated)
    blake = target / "src/md/blake2.h"
    blake_value = blake.read_bytes()
    aligned = b"RLC_ALIGNME( 64 ) typedef struct"
    if (
        blake_value.count(aligned) != 2
        or blake_value.count(b"#pragma pack(push, 1)") != 1
        or blake_value.count(b"#pragma pack(pop)") != 1
    ):
        raise RuntimeError("OPENABE_RELIC_BLAKE2_CONTRACT_MISMATCH")
    blake.write_bytes(
        blake_value.replace(aligned, b"typedef struct")
        .replace(b"#pragma pack(push, 1)\n", b"")
        .replace(b"#pragma pack(pop)\n", b"")
    )
    label = target / "include/relic_label.h"
    lines = label.read_bytes().splitlines(keepends=True)
    label.write_bytes(b"".join(line for line in lines if not line.startswith(b"#define ep2_mul ")))
    tarball = target.parent / (target.name + ".tar.gz")
    with tarfile.open(tarball, "w:gz") as archive:
        archive.add(target, arcname=target.name)
    return target


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--openabe", required=True)
    args = parser.parse_args()
    print(prepare(Path(args.source).resolve(), Path(args.openabe).resolve()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
