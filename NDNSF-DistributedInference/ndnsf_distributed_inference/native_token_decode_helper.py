"""Small deployment-safe tokenizer subprocess used by the native Provider.

The helper deliberately loads only ``tokenizer.json`` through the standalone
Rust-backed ``tokenizers`` package.  It is not an LLM runtime and must not
import PyTorch or Transformers.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def _decode_standalone_tokenizer(
    path: Path,
    expected_digest: str,
    token_ids: list[int],
) -> str:
    """Decode without importing the application/adapters package graph.

    The native Provider invokes this helper with the deployment interpreter.
    Importing ``adapters`` would execute the full planner package and can load
    the optional C++ Python extension, which is neither needed nor guaranteed
    to have the same ABI as the native Provider.  Keep this process boundary
    limited to the Rust-backed ``tokenizers`` wheel and tokenizer.json.
    """
    source = path.expanduser().resolve()
    if not source.is_file() or source.name != "tokenizer.json":
        raise ValueError("Qwen runtime requires an exact tokenizer.json")
    wire = source.read_bytes()
    digest = "sha256:" + hashlib.sha256(wire).hexdigest()
    if expected_digest and expected_digest != digest:
        raise ValueError("Qwen tokenizer digest mismatch")
    try:
        from tokenizers import Tokenizer
    except ImportError as exc:  # pragma: no cover - container gate covers it
        raise RuntimeError("Qwen runtime requires standalone tokenizers") from exc
    tokenizer = Tokenizer.from_str(wire.decode("utf-8"))
    return str(tokenizer.decode(token_ids, skip_special_tokens=True))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tokenizer", required=True, type=Path)
    parser.add_argument("--expected-digest", required=True)
    parser.add_argument("--ids-json", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    token_ids = json.loads(args.ids_json)
    if (not isinstance(token_ids, list)
            or any(not isinstance(value, int) or isinstance(value, bool) or value < 0
                   for value in token_ids)):
        raise SystemExit("token ID list is invalid")
    text = _decode_standalone_tokenizer(
        args.tokenizer, args.expected_digest, token_ids)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
