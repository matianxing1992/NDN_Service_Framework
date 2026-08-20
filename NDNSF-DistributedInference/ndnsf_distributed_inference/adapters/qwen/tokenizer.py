"""Standalone Qwen tokenizer boundary for ONNX deployment runtimes.

The deployment package intentionally uses the Rust-backed ``tokenizers``
library directly.  Hugging Face Transformers belongs only to the sealed
offline export/conformance environment and is never imported here.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
from typing import Sequence


@dataclass(frozen=True)
class StandaloneQwenTokenizer:
    """Digest-bound tokenizer.json wrapper with a minimal runtime surface."""

    _tokenizer: object
    tokenizer_digest: str

    @classmethod
    def from_file(
        cls,
        path: str | Path,
        *,
        expected_digest: str = "",
    ) -> "StandaloneQwenTokenizer":
        source = Path(path).expanduser().resolve()
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
        return cls(Tokenizer.from_str(wire.decode("utf-8")), digest)

    def encode(self, text: str, *, add_special_tokens: bool = True) -> tuple[int, ...]:
        encoded = self._tokenizer.encode(
            str(text), add_special_tokens=bool(add_special_tokens))
        return tuple(int(value) for value in encoded.ids)

    def decode(self, token_ids: Sequence[int], *, skip_special_tokens: bool = True) -> str:
        return str(self._tokenizer.decode(
            [int(value) for value in token_ids],
            skip_special_tokens=bool(skip_special_tokens)))


__all__ = ["StandaloneQwenTokenizer"]
