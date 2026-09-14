#!/usr/bin/env python3
"""Make a non-destructive, word-marked comparison of two exact proposal PDFs.

Requires PyMuPDF. Text matching is not a semantic or figure-change audit.
The cover is compiled separately from comparison-guide.tex with pdflatex.
"""
import argparse
from collections import defaultdict
from difflib import SequenceMatcher
import hashlib
import json
from pathlib import Path
import re
import unicodedata

import fitz


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract(doc):
    """Keep word locations; normalize ligatures and join line-end hyphenation."""
    first_chapter = next(p - 1 for level, title, p in doc.get_toc()
                         if level == 1 and title == "Introduction")
    toc_start = next(i for i, p in enumerate(doc)
                     if p.get_text().lstrip().startswith("Table of Contents"))
    excluded = set(range(toc_start, first_chapter)) | {0}
    words = []
    for i, page in enumerate(doc):
        if i in excluded:
            continue
        page_words = page.get_text("words", sort=True)
        for word in page_words:
            x0, y0, x1, y1, value, block, line, _ = word
            if y0 > page.rect.height - 65 and re.fullmatch(r"[0-9ivxlcdm]+", value):
                continue
            value = unicodedata.normalize("NFKC", value).replace("\u00ad", "")
            rect = (x0, y0, x1, y1)
            if (words and words[-1]["page"] == i
                    and words[-1]["text"].endswith("-")
                    and words[-1]["line"] != (block, line)
                    and value and value[0].islower()):
                words[-1]["text"] = words[-1]["text"][:-1] + value
                words[-1]["rects"].append(rect)
                words[-1]["line"] = (block, line)
            else:
                words.append({"text": value, "page": i, "line": (block, line),
                              "rects": [rect]})
    return words, sorted(p + 1 for p in excluded)


def changed_indices(old, new):
    matcher = SequenceMatcher(None, [w["text"] for w in old],
                              [w["text"] for w in new], autojunk=False)
    removed, added, operations = set(), set(), []
    for tag, a, b, c, d in matcher.get_opcodes():
        if tag == "equal":
            continue
        removed.update(range(a, b))
        added.update(range(c, d))
        operations.append({"kind": tag, "original_words": [a, b],
                           "current_words": [c, d],
                           "original_pdf_pages": sorted({w["page"] + 1 for w in old[a:b]}),
                           "current_pdf_pages": sorted({w["page"] + 1 for w in new[c:d]})})
    # Chapter reordering otherwise makes an unchanged long passage look entirely
    # new. Recover verbatim runs of >=12 words without assuming document order.
    # Use each occurrence at most once; repeated copies are still additions.
    width = 12
    left = [w["text"] for w in old]
    right = [w["text"] for w in new]
    index = defaultdict(list)
    for j in range(len(right) - width + 1):
        if all(k in added for k in range(j, j + width)):
            index[tuple(right[j:j + width])].append(j)
    runs = []
    for i in range(len(left) - width + 1):
        if not all(k in removed for k in range(i, i + width)):
            continue
        for j in index.get(tuple(left[i:i + width]), []):
            if i and j and i - 1 in removed and j - 1 in added and left[i - 1] == right[j - 1]:
                continue
            n = width
            while (i + n in removed and j + n in added and left[i + n] == right[j + n]):
                n += 1
            runs.append((n, i, j))
    recovered = []
    for n, i, j in sorted(runs, key=lambda item: (-item[0], item[1], item[2])):
        if all(k in removed for k in range(i, i + n)) and all(k in added for k in range(j, j + n)):
            removed.difference_update(range(i, i + n))
            added.difference_update(range(j, j + n))
            recovered.append({"original_start": i, "current_start": j, "word_count": n})
    # Operations describe the initial alignment; recovery is explicitly separate.
    operations.append({"verbatim_runs_recovered_independent_of_order": recovered})
    return removed, added, operations


def mark(doc, offset, words, selected, old=False):
    lines = defaultdict(list)
    for i in sorted(selected):
        word = words[i]
        for rect in word["rects"]:
            # Actual vertical coordinates, not block IDs: joined words may span lines.
            lines[(word["page"], round(rect[1], 1), round(rect[3], 1))].append(fitz.Rect(rect))
    counts = defaultdict(int)
    for (page_num, _, _), rects in sorted(lines.items()):
        rects.sort(key=lambda r: r.x0)
        groups = []
        for rect in rects:
            if groups and rect.x0 - groups[-1].x1 < 4:
                groups[-1] |= rect
            else:
                groups.append(fitz.Rect(rect))
        page = doc[offset + page_num]
        for rect in groups:
            assert page.rect.contains(rect), (page_num, rect)
            color = (1, .60, .60) if old else (.30, .65, 1)
            # Vector overlays survive viewers that suppress PDF annotations.
            page.draw_rect(rect, color=None, fill=color, fill_opacity=.23, overlay=True)
            if old:
                y = rect.y0 + .53 * rect.height
                page.draw_line((rect.x0, y), (rect.x1, y), color=(.72, .12, .12), width=.55)
            else:
                page.draw_line((rect.x0, rect.y1 - 1), (rect.x1, rect.y1 - 1),
                               color=(.05, .28, .68), width=.55)
            counts[page_num + 1] += 1
    return dict(counts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--guide", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    inputs = [args.original.resolve(), args.current.resolve(), args.guide.resolve()]
    assert args.output.resolve() not in inputs
    assert not args.output.exists(), "Use a fresh output path; never replace an earlier run."
    hashes = {str(p): sha(p) for p in inputs}
    old, new, guide = [fitz.open(p) for p in inputs]
    assert len(guide) == 1, "The comparison guide must fit on one page."
    old_words, old_excluded = extract(old)
    new_words, new_excluded = extract(new)
    removed, added, operations = changed_indices(old_words, new_words)
    out = fitz.open()
    out.insert_pdf(guide)
    new_offset = len(out)
    out.insert_pdf(new)
    old_offset = len(out)
    out.insert_pdf(old)
    counts_new = mark(out, new_offset, new_words, added)
    counts_old = mark(out, old_offset, old_words, removed, old=True)
    toc = [[1, "Comparison guide", 1], [1, "CURRENT - additions / rewritten wording", new_offset + 1]]
    for level, title, page in new.get_toc():
        toc.append([level + 1, title, page + new_offset])
    toc.append([1, "ORIGINAL - removed / rewritten wording", old_offset + 1])
    for level, title, page in old.get_toc():
        toc.append([level + 1, title, page + old_offset])
    out.set_toc(toc)
    label_rects = []
    for source, offset, label in [(new, new_offset, "CURRENT"), (old, old_offset, "ORIGINAL")]:
        for i, source_page in enumerate(source):
            page = out[offset + i]
            label_box = fitz.Rect(36, 14, page.rect.width - 36, 29)
            original_words = source_page.get_text("words")
            assert not any(label_box.intersects(fitz.Rect(w[:4])) for w in original_words)
            label = label.split(" | ")[0]
            page.insert_text((36, 24), f"{label} | Source PDF page {i + 1} of {len(source)}",
                             fontname="helv", fontsize=8, color=(.27, .31, .38))
            label_rects.append(label_box)
    out.set_metadata({"title": "NDNSF Dissertation Proposal - Original / Current Text Comparison",
                      "author": "Tianxing Ma", "subject": "Review copy; exact PDF sources preserved"})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    out.save(args.output, garbage=4, deflate=True)
    check = fitz.open(args.output)
    preserved = []
    for source, offset, label in [(new, new_offset, "CURRENT"), (old, old_offset, "ORIGINAL")]:
        for i, page in enumerate(source):
            rendered = check[offset + i]
            header = f"{label} | Source PDF page {i + 1} of {len(source)}\n"
            assert rendered.get_text().endswith(header)
            assert rendered.get_text()[:-len(header)] == page.get_text(), (label, i)
            assert rendered.rect == page.rect
            assert len(rendered.get_images()) == len(page.get_images())
            assert all(-.1 <= x <= rendered.rect.width + .1 and -.1 <= y <= rendered.rect.height + .1
                       for w in rendered.get_text("words") for x, y in [(w[0], w[1]), (w[2], w[3])])
            preserved.append({"version": label, "source_page": i + 1, "output_page": offset + i + 1})
    assert all(sha(p) == hashes[str(p)] for p in inputs)
    report = {"status": "DOCUMENT_PASS", "method": "PDF word alignment; no semantic or image diff",
              "input_sha256": hashes, "output_sha256": sha(args.output),
              "original_pages": len(old), "current_pages": len(new), "comparison_pages": len(check),
              "current_starts_at": new_offset + 1, "original_starts_at": old_offset + 1,
              "excluded_from_text_matching": {"original": old_excluded, "current": new_excluded},
              "original_word_count": len(old_words), "current_word_count": len(new_words),
              "marked_original_words": len(removed), "marked_current_words": len(added),
              "original_page_mark_groups": counts_old, "current_page_mark_groups": counts_new,
              "source_pages_preserved": preserved, "operations": operations,
              "limitations": ["Reordered text can appear as removed and added.",
                              "Figures and formatting preserved but not automatically compared.",
                              "Title, contents and page-number-only changes not highlighted.",
                              "PDF extraction / word alignment is not semantic change classification."]}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ["status", "comparison_pages", "original_pages",
                     "current_pages", "marked_original_words", "marked_current_words"]}))


if __name__ == "__main__":
    main()
