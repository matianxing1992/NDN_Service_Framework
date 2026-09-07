#!/usr/bin/env python3
"""Pin reference decoder boundaries; does not exercise the native product."""

import hashlib
import json
from pathlib import Path

import tokenizers


def main():
    if tokenizers.__version__ != "0.20.3":
        raise RuntimeError("reference requires tokenizers==0.20.3")
    path = Path(__file__).parent / "tokenizer" / "vectors.json"
    wire = path.read_bytes()
    expected = "6f44a2e62bebccd4dee2ff214d4d65f99528899e6863705eb1ec431b098ed34b"
    if hashlib.sha256(wire).hexdigest() != expected:
        raise RuntimeError("reference fixture identity changed")
    fixture = next(f for f in json.loads(wire)["fixtures"]
                   if f["name"] == "byte-fallback-special")
    tok = tokenizers.Tokenizer.from_str(fixture["tokenizerJson"])
    observations = []
    cases = [
        ("complete_then_invalid", ["<0x61>", "<0xFF>"], ["a", "\ufffd\ufffd"]),
        ("complete_then_incomplete", ["<0x61>", "<0xE5>"], ["a", "\ufffd\ufffd"]),
        ("utf8_character", ["<0xE5>", "<0xA5>", "<0xBD>"],
         ["\ufffd", "\ufffd\ufffd", "好"]),
        ("valid_replacement_character", ["<0xEF>", "<0xBF>", "<0xBD>"],
         ["\ufffd", "\ufffd\ufffd", "\ufffd"]),
    ]
    for name, tokens, expected_prefixes in cases:
        ids = [tok.token_to_id(t) for t in tokens]
        if any(t is None for t in ids):
            raise RuntimeError("fixture does not contain byte token")
        actual = [tok.decode(ids[:n], skip_special_tokens=True)
                  for n in range(1, len(ids) + 1)]
        if actual != expected_prefixes:
            raise AssertionError((name, actual, expected_prefixes))
        observations.append({"name": name, "ids": ids,
                             "prefixes": actual,
                             "finalText": actual[-1]})
    for name, tokens, skip_special, expected_text in [
        ("ordinary_token_closes_run", ["<0x61>", "你", "<0xFF>"], True, "a你\ufffd"),
        ("skipped_special_does_not_close_run", ["<0x61>", "</s>", "<0xFF>"],
         True, "\ufffd\ufffd"),
        ("retained_special_closes_run", ["<0x61>", "</s>", "<0xFF>"],
         False, "a</s>\ufffd"),
    ]:
        ids = [tok.token_to_id(t) for t in tokens]
        if any(t is None for t in ids):
            raise RuntimeError("fixture token missing")
        actual = tok.decode(ids, skip_special_tokens=skip_special)
        if actual != expected_text:
            raise AssertionError((name, actual, expected_text))
        observations.append({"name": name, "ids": ids,
                             "skipSpecialTokens": skip_special,
                             "finalText": actual})
    print(json.dumps({"schema": "spec182-stream-boundary-reference-v1",
                      "scope": "reference-only; native NOT_RUN",
                      "tokenizers": tokenizers.__version__,
                      "fixtureSha256": expected,
                      "cases": observations}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
