#!/usr/bin/env python3
"""Author the T007-B stable-prefix vector set (frozen output: stable-vectors.json).

Profiles covered per native-token-stream-design.md "Stable Prefix Algorithms":
  - byte-fallback-special (real ByteFallback fixture, bytes copied verbatim
    from vectors.json so its sha256 stays the frozen 3aef42... artifact)
  - bytelevel          (new synthetic ByteLevel fixture: ids 0..255 are the
    canonical GPT-2 alphabet char of byte `id`, plus whole-token fallbacks)
  - legacy-ascii       (null decoder) / legacy-unicode (WordPiece) fixtures
    copied verbatim from vectors.json; their stable text is the full decode of
    the prefix, computed here straight from HF
  - strip-reject       (unsupported decoder pipeline) with no rows: every
    stable call must fail closed while full decode keeps working

Expected prefixes are derived from HF itself where HF is authoritative
(ByteFallback closed-part decode, null/WordPiece prefix decode) and from a
byte-granular incremental UTF-8 stream (final=False) for ByteLevel non-final
text; every row's finalText is asserted to equal HF decode(ids, skip) before
freezing.  Requires tokenizers==0.20.3.
"""
import codecs
import hashlib
import json
import sys

import tokenizers
from tokenizers import Tokenizer

OUT = "stable-vectors.json"
VECTORS = "vectors.json"

SPECIALS = {"<s>", "</s>"}


def byte_alphabet():
    values = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
    chars = list(values)
    extra = 0
    for value in range(256):
        if value not in values:
            values.append(value)
            chars.append(256 + extra)
            extra += 1
    return dict(zip(values, map(chr, chars)))


def digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def content_by_id(tokenizer_json):
    """id -> content with added-first precedence (decode() lookup order)."""
    model = json.loads(tokenizer_json)
    result = {int(id_): token for token, id_ in model["model"]["vocab"].items()}
    for added in model.get("added_tokens", []):
        result[int(added["id"])] = added["content"]
    return result


def is_special_content(content):
    return content in SPECIALS


def is_byte_token(content):
    if len(content) != 6 or not content.startswith("<0x") or not content.endswith(">"):
        return False
    try:
        int(content[3:5], 16)
        return True
    except ValueError:
        return False


def bytelevel_bytes(content_map, alphabet, ids):
    raw = bytearray()
    for id_ in ids:
        content = content_map[id_]
        chars = list(content)
        if all(c in alphabet.values() for c in chars):
            byte_of = {c: b for b, c in alphabet.items()}
            raw.extend(byte_of[c] for c in chars)
        else:
            raw.extend(content.encode("utf-8"))
    return bytes(raw)


def utf8_error(seq, start):
    """std::str::from_utf8 error attribution for seq[start:]: returns
    (valid_up_to, error_len|None) with the exact std rules the frozen
    algorithm walks: ASCII and complete sequences pass; a lead byte whose
    second byte violates the tight E0/ED/F0/F4 range is a determined error
    consuming only the lead; a continuation failure after k consumed bytes
    consumes k; a truncated tail yields error_len None (held)."""
    i = start
    n = len(seq)
    while i < n:
        c = seq[i]
        if c < 0x80:
            i += 1
            continue
        if 0xC2 <= c <= 0xDF:
            if i + 1 >= n:
                return (i - start, None)
            if not 0x80 <= seq[i + 1] <= 0xBF:
                return (i - start, 1)
            i += 2
            continue
        if 0xE0 <= c <= 0xEF:
            tight = (0xA0, 0xBF) if c == 0xE0 else (
                (0x80, 0x9F) if c == 0xED else (0x80, 0xBF))
            if i + 1 >= n:
                return (i - start, None)
            if not 0x80 <= seq[i + 1] <= 0xBF or not tight[0] <= seq[i + 1] <= tight[1]:
                return (i - start, 1)
            if i + 2 >= n:
                return (i - start, None)
            if not 0x80 <= seq[i + 2] <= 0xBF:
                return (i - start, 2)
            i += 3
            continue
        if 0xF0 <= c <= 0xF4:
            tight = (0x90, 0xBF) if c == 0xF0 else (
                (0x80, 0x8F) if c == 0xF4 else (0x80, 0xBF))
            if i + 1 >= n:
                return (i - start, None)
            if not 0x80 <= seq[i + 1] <= 0xBF or not tight[0] <= seq[i + 1] <= tight[1]:
                return (i - start, 1)
            if i + 2 >= n:
                return (i - start, None)
            if not 0x80 <= seq[i + 2] <= 0xBF:
                return (i - start, 2)
            if i + 3 >= n:
                return (i - start, None)
            if not 0x80 <= seq[i + 3] <= 0xBF:
                return (i - start, 3)
            i += 4
            continue
        return (i - start, 1)
    return (n - start, 0)


def walk_lossy(raw, flush_tail):
    """Byte-granular commit walk mirroring the native stable_prefix: commit
    every complete valid run verbatim, one U+FFFD per determined-invalid
    unit, hold a trailing incomplete tail (error_len None).  With flush_tail
    the held tail becomes a single U+FFFD (Rust-std lossy full semantics,
    identical to the python codec final flush and to HF decode)."""
    text = ""
    offset = 0
    while offset < len(raw):
        valid, err = utf8_error(raw, offset)
        text += raw[offset:offset + valid].decode("utf-8", "replace")
        offset += valid
        if err is None:
            if flush_tail:
                text += "�"
            break
        if err == 0:
            break
        text += "�"
        offset += err
    return text


def stream_text(raw, final):
    return walk_lossy(raw, final)


def load_vectors_fixture(container, name):
    for fixture in container["fixtures"]:
        if fixture["name"] == name:
            assert digest(fixture["tokenizerJson"]) == fixture["sha256"]
            return fixture
    raise AssertionError("fixture %s missing" % name)


def hf(tokenizer_json, ids, skip):
    return Tokenizer.from_str(tokenizer_json).decode(ids, skip)


def legacy_rows(tokenizer_json, skip, names):
    """Freeze decode-derived prefixes over named vocab contents.  The decode
    path does not require the ids to have come from an encode (generation
    samples arbitrary id lists), so vocab-known ids give the richest rows."""
    tokenizer = Tokenizer.from_str(tokenizer_json)
    vocab = {content: int(id_) for content, id_ in tokenizer.get_vocab(False).items()}
    ids = [vocab[name] for name in names]
    prefixes = []
    for cut in range(1, len(ids) + 1):
        prefixes.append(tokenizer.decode(ids[:cut], skip))
    return {"name": "prefixDecodes" + ("Skipped" if skip else "Retained"),
            "skip": skip, "tokens": names, "ids": ids,
            "prefixes": prefixes, "finalText": tokenizer.decode(ids, skip)}


def bytefallback_rows(container):
    fixture = load_vectors_fixture(container, "byte-fallback-special")
    tokenizer_json = fixture["tokenizerJson"]
    content = content_by_id(tokenizer_json)
    def ids_of(*names):
        tok = Tokenizer.from_str(tokenizer_json)
        return [tok.token_to_id(n) for n in names]
    rows = []
    # (name, skip, names) — every entry exercises the frozen run rule.
    spec = [
        ("openRunHoldsEvenValidPrefix", True, ["<0x61>", "<0xFF>"]),
        ("validRunCommittedOnlyAfterCloser", True, ["<0x61>", "a", "<0x62>"]),
        ("invalidRunReplacementPerByteAfterCloser", True, ["<0xFF>", "a"]),
        ("closedRunCommitsWholeCharacter", True, ["<0xE4>", "<0xBD>", "<0xA0>", "a"]),
        ("allByteOutputFlushedOnlyAtFinal", True, ["<0x61>", "<0x62>", "<0x63>"]),
        ("legalReplacementFlushOnFinal", True, ["<0xEF>", "<0xBF>", "<0xBD>"]),
        ("skippedSpecialsDoNotCloseRun", True, ["<0x61>", "<s>", "<0x62>"]),
        ("runPairThenCloserThenOpenRun", True, ["<0x61>", "<0x62>", "a", "<0x63>"]),
        ("invalidRunMidStreamThenCloser", True, ["<0xE5>", "<0x8F>", "a"]),
        ("retainedSpecialClosesRun", False, ["<0x61>", "<s>", "<0x62>"]),
        ("retainedSpecialClosesUtf8Run", False,
         ["<0xE4>", "<0xBD>", "<0xA0>", "</s>", "<0x61>"]),
    ]
    tok = Tokenizer.from_str(tokenizer_json)
    for name, skip, names in spec:
        ids = [tok.token_to_id(n) for n in names]
        effective = [i for i in ids if not (skip and is_special_content(content[i]))]
        prefixes = []
        for cut in range(1, len(ids) + 1):
            kept = [i for i in ids[:cut] if not (skip and is_special_content(content[i]))]
            run_start = len(kept)
            while run_start > 0 and is_byte_token(content[kept[run_start - 1]]):
                run_start -= 1
            closed = kept[:run_start]
            prefixes.append(tok.decode(closed, skip) if closed else "")
        final_text = tok.decode(ids, skip)
        for text in prefixes:
            if not final_text.startswith(text):
                raise AssertionError(("prefix property", name, text, final_text))
        rows.append({"name": name, "skip": skip, "tokens": names, "ids": ids,
                     "prefixes": prefixes, "finalText": final_text})
    return fixture, rows


def bytelevel_fixture_rows():
    alphabet = byte_alphabet()
    byte_of = {c: b for b, c in alphabet.items()}
    assert len(set(alphabet.values())) == 256 and alphabet[32] == "Ġ"
    vocab = {alphabet[b]: b for b in range(256)}
    vocab.update({"你": 256, "好": 257, "🙂": 258, "你好": 259,
                  "Ġ你好": 260, "[UNK]": 261})
    added = [
        {"id": 262, "content": "<s>", "single_word": False, "lstrip": False,
         "rstrip": False, "normalized": False, "special": True},
        {"id": 263, "content": "</s>", "single_word": False, "lstrip": False,
         "rstrip": False, "normalized": False, "special": True},
    ]
    base = json.loads(load_vectors_fixture(
        json.loads(open(VECTORS).read()), "byte-fallback-special")["tokenizerJson"])
    base["model"]["vocab"] = vocab
    base["model"]["merges"] = []
    base["model"]["byte_fallback"] = False
    # Full explicit field set, exactly what python 0.20.3 to_str() emits for
    # ByteLevel() — the bindings reject field-less {"type": "ByteLevel"}.
    base["decoder"] = {"type": "ByteLevel", "add_prefix_space": True,
                       "trim_offsets": True, "use_regex": True}
    base["added_tokens"] = added
    tokenizer_json = json.dumps(base, ensure_ascii=False)
    content = content_by_id(tokenizer_json)
    tokenizer = Tokenizer.from_str(tokenizer_json)
    # Sanity: the alphabet round-trips through the real HF ByteLevel decoder.
    assert tokenizer.decode([0xE5, 0xA5, 0xBD], False) == "好"
    assert tokenizer.decode([0x61], False) == "a"
    assert tokenizer.decode([256, 257, 258, 259, 260], False) == "你好🙂你好Ġ你好"
    assert tokenizer.decode([262], True) == ""
    assert tokenizer.decode([262], False) == "<s>"
    # (name, skip, ids).  Byte ids equal their alphabet-token ids; 256..261 are
    # whole raw tokens; 262/263 are the specials.
    def m(name, skip, ids):
        return {"name": name, "skip": skip, "ids": ids,
                "tokens": [repr(content[i]) for i in ids]}
    spec = [
        m("completeBoundariesAcrossTokens", True,
          [0xE5, 0xA5, 0xBD, 0x61, 0xF0, 0x9F, 0x99, 0x82, 0xE4, 0xBD, 0xA0]),
        m("truncatedLeadHeldAndFlushed", True, [0x61, 0xE5]),
        m("truncatedTwoContinuationsHeld", True, [0x61, 0xE5, 0xA5]),
        m("loneInvalidByteCommitted", True, [0x61, 0xFF]),
        m("determinedSubpartWithBoundaryBreak", True, [0xE5, 0xA5, 0x41]),
        m("leadByteBreakCommitsReplacement", True, [0xE5, 0x41, 0x62]),
        m("surrogateSequenceCommits", True, [0xED, 0xA0, 0x80]),
        m("legalReplacementCharacterKept", True, [0xEF, 0xBF, 0xBD, 0x61]),
        m("wholeTokenFallbacks", True, [256, 257, 258, 259]),
        m("mixedAlphabetWholeFallbackToken", True, [260, 0x61]),
        m("truncatedLeadThenWholeRawToken", True, [0xE5, 0xA5, 256]),
        m("specialsSkippedVanish", True, [0xE5, 0xA5, 0xBD, 262, 0x61]),
        m("specialsRetainedMapAscii", False, [0xE5, 0xA5, 0xBD, 262, 0x61]),
    ]
    rows = []
    for row in spec:
        ids = row["ids"]
        effective = [i for i in ids if not (row["skip"] and is_special_content(content[i]))]
        prefixes = []
        for cut in range(1, len(ids) + 1):
            kept = [i for i in ids[:cut]
                    if not (row["skip"] and is_special_content(content[i]))]
            raw = bytelevel_bytes(content, alphabet, kept)
            prefixes.append(stream_text(raw, False))
        final_text = tokenizer.decode(ids, row["skip"])
        full_raw = bytelevel_bytes(content, alphabet, effective)
        if stream_text(full_raw, True) != final_text:
            raise AssertionError(("HF final mismatch", row["name"],
                                  stream_text(full_raw, True), final_text))
        for text in prefixes:
            if not final_text.startswith(text):
                raise AssertionError(("prefix property", row["name"], text, final_text))
        rows.append({"name": row["name"], "skip": row["skip"], "ids": ids,
                     "tokens": row["tokens"], "prefixes": prefixes,
                     "finalText": final_text})
    return tokenizer_json, rows


def reject_fixture(container):
    fixture = load_vectors_fixture(container, "byte-fallback-special")
    base = json.loads(fixture["tokenizerJson"])
    base["decoder"] = {"type": "Fuse"}
    tokenizer_json = json.dumps(base, ensure_ascii=False)
    tokenizer = Tokenizer.from_str(tokenizer_json)
    assert tokenizer.decode([0], False) != ""
    return tokenizer_json


def main():
    if tokenizers.__version__ != "0.20.3":
        raise RuntimeError("reference requires tokenizers==0.20.3")
    container = json.loads(open(VECTORS).read())
    bf_fixture, bf_rows = bytefallback_rows(container)
    bl_json, bl_rows = bytelevel_fixture_rows()
    reject_json = reject_fixture(container)
    fixtures = [
        {"name": "byte-fallback-special", "profile": "bytefallback",
         "tokenizerJson": bf_fixture["tokenizerJson"],
         "sha256": bf_fixture["sha256"], "cases": bf_rows},
        {"name": "bytelevel", "profile": "bytelevel",
         "tokenizerJson": bl_json, "sha256": digest(bl_json), "cases": bl_rows},
        {"name": "reject-fuse", "profile": "unsupported",
         "tokenizerJson": reject_json, "sha256": digest(reject_json), "cases": []},
    ]
    for name in ("legacy-ascii", "legacy-unicode"):
        fixture = load_vectors_fixture(container, name)
        names = (["[UNK]", "token-1", "token-2", "token-3"] if name == "legacy-ascii"
                 else ["[UNK]", "你", "好", "🙂", "!", "token-1"])
        rows = [legacy_rows(fixture["tokenizerJson"], True, names),
                legacy_rows(fixture["tokenizerJson"], False, names)]
        fixtures.insert(3 if name == "legacy-ascii" else 4, {
            "name": name,
            "profile": "null" if name == "legacy-ascii" else "wordpiece",
            "tokenizerJson": fixture["tokenizerJson"],
            "sha256": fixture["sha256"], "cases": rows})
    doc = {"schema": "spec182-tokenizer-stable-vectors-v1",
           "frozenAt": "2026-09-07", "freezeOwner": "T007-B",
           "fixtures": fixtures}
    text = json.dumps(doc, ensure_ascii=False, indent=1)
    open(OUT, "w").write(text + "\n")
    print("wrote", OUT, "sha256", digest(text))
    for fixture in fixtures:
        print(fixture["name"], fixture["profile"], "sha256",
              fixture["sha256"], "cases", len(fixture["cases"]))


if __name__ == "__main__":
    main()
