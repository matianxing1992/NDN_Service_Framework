#!/usr/bin/env python3
"""Print frozen dependency oracles using the pre-migration Python tokenizer.

This offline generator is never called by the C++ consumer or native runtime.
"""
import hashlib
import json
from pathlib import Path

import tokenizers
from tokenizers import AddedToken, Tokenizer, decoders, models, normalizers, processors

assert tokenizers.__version__ == "0.20.3", "reference version must remain frozen"
root = Path(__file__).resolve().parents[5]
base = root / "tests/fixtures/spec175/tiny-causal-lm-v1/standalone"
sources = [("legacy-ascii", (base / "tokenizer.json").read_text()),
           ("legacy-unicode", (base / "unicode/tokenizer.json").read_text())]
vocab = {"[UNK]": 0, "<s>": 1, "</s>": 2, "a": 3, "你": 4}
vocab.update({f"<0x{byte:02X}>": byte + 5 for byte in range(256)})
backend = Tokenizer(models.BPE(vocab=vocab, merges=[], unk_token="[UNK]", byte_fallback=True))
backend.normalizer = normalizers.NFC()
backend.decoder = decoders.ByteFallback()
backend.add_special_tokens([AddedToken("<s>", special=True), AddedToken("</s>", special=True)])
backend.post_processor = processors.TemplateProcessing(
    single="<s> $A </s>", special_tokens=[("<s>", 1), ("</s>", 2)])
sources.append(("byte-fallback-special", backend.to_str()))
fixtures = []
for name, wire in sources:
    tokenizer = Tokenizer.from_str(wire)
    cases = []
    for text in ["a", "你好🙂", "a café", "", "<s>a</s>", "é", "e\u0301"]:
        for add_special in (False, True):
            ids = tokenizer.encode(text, add_special_tokens=add_special).ids
            for skip_special in (False, True):
                cases.append({"input": text, "addSpecial": add_special,
                              "skipSpecial": skip_special, "ids": ids,
                              "decoded": tokenizer.decode(ids, skip_special_tokens=skip_special)})
    fixtures.append({"name": name, "tokenizerJson": wire,
                     "sha256": hashlib.sha256(wire.encode()).hexdigest(), "cases": cases})
print(json.dumps({"schema": "spec182-tokenizer-dependency-vectors-v1",
                  "reference": "tokenizers==0.20.3", "fixtures": fixtures},
                 ensure_ascii=False, indent=2))
