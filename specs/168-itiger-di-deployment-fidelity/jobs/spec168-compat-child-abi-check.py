#!/usr/bin/env python3
"""Prove a compatibility child still maps the candidate native ABI closure."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path

import ndnsf._ndnsf


def main() -> int:
    core = Path(os.environ["SPEC168_NATIVE_CORE_LIBRARY"]).resolve()
    extension = Path(os.environ["SPEC168_NATIVE_PYTHON_EXTENSION"]).resolve()
    if Path(ndnsf._ndnsf.__file__).resolve() != extension:
        raise SystemExit("SPEC168_COMPAT_CHILD_EXTENSION_MISMATCH")
    if str(core) not in Path("/proc/self/maps").read_text(encoding="utf-8"):
        raise SystemExit("SPEC168_COMPAT_CHILD_NATIVE_CORE_SHADOWED")
    print(
        "SPEC168_COMPAT_CHILD_NATIVE_ABI_PASS",
        "coreSha256=sha256:" + hashlib.sha256(core.read_bytes()).hexdigest(),
        "extensionSha256=sha256:" + hashlib.sha256(extension.read_bytes()).hexdigest(),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
