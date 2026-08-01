#!/usr/bin/env python3
"""Verify the Qwen3.6 overlay without downloading or loading model weights."""

from __future__ import annotations

import argparse
import importlib.metadata
import json
from pathlib import Path
import re
import subprocess


ALLOWED_PIP_CHECK = re.compile(
    r"^torch 2\.6\.0\+cu124 requires nvidia-[a-z0-9-]+, which is not installed\.$"
)


def verify(lock_path: Path) -> dict:
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    observed = {}
    for package, row in sorted(lock["wheelClosure"].items()):
        version = importlib.metadata.version(package)
        if version != row["version"]:
            raise RuntimeError(
                f"QWEN36_RUNTIME_VERSION_MISMATCH:{package}:{version}"
            )
        observed[package] = version

    check = subprocess.run(
        [
            "/opt/venv/bin/python",
            "-m",
            "pip",
            "--disable-pip-version-check",
            "--no-cache-dir",
            "check",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    unexpected = [
        line for line in check.stdout.splitlines()
        if line.strip() and ALLOWED_PIP_CHECK.fullmatch(line.strip()) is None
    ]
    if unexpected:
        raise RuntimeError("QWEN36_RUNTIME_PIP_CHECK_FAILED:" + "|".join(unexpected))

    import torch
    import transformers
    from transformers.masking_utils import (
        create_causal_mask,
        create_recurrent_attention_mask,
    )
    from transformers.models.qwen3_5.configuration_qwen3_5 import (
        Qwen3_5TextConfig,
    )
    from transformers.models.qwen3_5.modeling_qwen3_5 import (
        Qwen3_5DecoderLayer,
        Qwen3_5RMSNorm,
        Qwen3_5TextRotaryEmbedding,
    )

    _ = (
        create_causal_mask,
        create_recurrent_attention_mask,
        Qwen3_5TextConfig,
        Qwen3_5DecoderLayer,
        Qwen3_5RMSNorm,
        Qwen3_5TextRotaryEmbedding,
    )
    return {
        "schemaVersion": "ndnsf-di-qwen36-runtime-probe-v1",
        "status": "PASS",
        "transformers": transformers.__version__,
        "torch": torch.__version__,
        "cudaAvailable": bool(torch.cuda.is_available()),
        "packages": observed,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True)
    args = parser.parse_args()
    try:
        value = verify(Path(args.lock))
    except Exception as error:
        print(json.dumps({
            "schemaVersion": "ndnsf-di-qwen36-runtime-probe-v1",
            "status": "FAIL",
            "reason": str(error),
        }, sort_keys=True))
        return 4
    print(json.dumps(value, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
