#!/usr/bin/env python3
"""Compare a standard UTF-8 stream oracle with fixed HF ByteLevel decoding."""
import codecs
import json

import tokenizers
from tokenizers.decoders import ByteLevel


def byte_alphabet():
    # Canonical GPT-2 byte alphabet used by HF ByteLevel; no BPE implementation.
    values = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
    chars = list(values)
    extra = 0
    for value in range(256):
        if value not in values:
            values.append(value)
            chars.append(256 + extra)
            extra += 1
    return dict(zip(values, map(chr, chars)))


def main():
    if tokenizers.__version__ != "0.20.3":
        raise RuntimeError("reference requires tokenizers==0.20.3")
    alphabet = byte_alphabet()
    if len(set(alphabet.values())) != 256 or alphabet[32] != "Ġ":
        raise AssertionError("invalid byte alphabet")
    full_decoder = ByteLevel()
    count = 0
    for first in range(256):
        for second in range(256):
            raw = bytes([first, second])
            stream = codecs.getincrementaldecoder("utf-8")("replace")
            deltas = [stream.decode(raw[:1], final=False),
                      stream.decode(raw[1:], final=False),
                      stream.decode(b"", final=True)]
            expected = full_decoder.decode([alphabet[first], alphabet[second]])
            if "".join(deltas) != expected:
                raise AssertionError((raw.hex(), deltas, expected))
            count += 1
    cases = []
    for raw in ["好".encode(), "🙂".encode(), "�".encode(),
                b"a\xe5", b"a\xff", b"\xed\xa0\x80", b"\xf4\x90\x80\x80",
                b"\xe5\xa5\xbd\xff", b"\xf0\x9f\x99\x82\xe5"]:
        stream = codecs.getincrementaldecoder("utf-8")("replace")
        deltas = [stream.decode(bytes([b]), final=False) for b in raw]
        flushed = stream.decode(b"", final=True)
        expected = full_decoder.decode([alphabet[b] for b in raw])
        if "".join(deltas) + flushed != expected:
            raise AssertionError((raw.hex(), deltas, flushed, expected))
        cases.append({"bytesHex": raw.hex(), "deltas": deltas,
                      "flush": flushed, "finalText": expected})
    # A token containing any non-alphabet character falls back as a WHOLE to
    # its UTF-8 bytes in HF; mapping its other characters individually is wrong.
    for token in ["你好", "Ġ你好", "�"]:
        if full_decoder.decode([token]) != token:
            raise AssertionError(("whole-token fallback", token))
    print(json.dumps({"scope": "reference-only; native NOT_RUN",
                      "tokenizers": tokenizers.__version__, "bytePairs": count,
                      "wholeTokenFallbackCases": 3, "cases": cases},
                     ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
