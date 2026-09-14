#!/usr/bin/env python3
"""Readable, topic-grouped side-by-side comparison of frozen proposal PDFs.

Whole source pages are embedded at 1:1 scale; no text difference overlays.
Boundary pages repeat deliberately because topics do not end at page breaks.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

import fitz
from PIL import Image, ImageChops


def seq(first, last):
    return list(range(first, last + 1))


GROUPS = [
    ("Title", [1], [1]),
    ("Abstract", [2, 3], [2]),
    ("Contents", [4, 5], [3, 4]),
    ("Motivation, research gap and scope", [6, 11, 12, 15, 16], seq(5, 8)),
    ("Research questions and expected contributions", [13, 14], [9, 10]),
    ("NDN background and naming", seq(7, 11), [11, 12]),
    ("Related work and design alternatives", seq(17, 25), seq(12, 16)),
    ("Architecture, responsibilities and API", seq(26, 30), seq(17, 19)),
    ("Named messages and invocation workflow", seq(30, 32), [20, 21]),
    ("Authorization, confidentiality and trust", seq(32, 34), [19] + seq(21, 26)),
    ("Invocation modes, streams and large objects", seq(37, 39), seq(26, 29)),
    ("Collaboration plan, dependencies and execution state", [29, 36, 37], seq(29, 31)),
    ("UAV application and validation", seq(40, 46), seq(32, 35)),
    ("Distributed inference application and validation", seq(47, 52), seq(35, 40)),
    ("Existing evidence and its interpretation", seq(34, 36), [41, 42]),
    ("Proposed evaluation and metrics", [45, 46, 51, 52, 54, 55], seq(43, 45)),
    ("Research plan, timeline and risks", seq(53, 56), [46, 47]),
    ("Conclusion and expected outcomes", [57], [48]),
    ("References", seq(57, 59), seq(48, 50)),
]
W, H = 1272, 880
PANELS = [fitz.Rect(12, 64, 624, 856), fitz.Rect(648, 64, 1260, 856)]
HASHES = ["3e1bd2e9ae323ad919917d64c0d6a003646ddc27e7a4ca137c7e9618c9bc6277",
          "f3b6d952ed723fa0b81b6b0600434f0c37d5230a92b2b5c60a13c6f3ca90fcae"]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def text(page, rect, value, size=11, color=(.15, .20, .27), bold=False):
    box = fitz.Rect(rect)
    name = "hebo" if bold else "helv"
    font = fitz.Font(fontname=name)
    lines = value.split("\n")
    baseline = box.y0 + font.ascender * size
    assert baseline + (len(lines) - 1) * size * 1.4 - font.descender * size <= box.y1, value
    for i, line in enumerate(lines):
        assert fitz.get_text_length(line, fontname=name, fontsize=size) <= box.width, line
        page.insert_text((box.x0, baseline + i * size * 1.4), line,
                         fontsize=size, fontname=name, color=color)


def page_range(values):
    runs = []
    start = end = values[0]
    for n in values[1:]:
        if n == end + 1:
            end = n
        else:
            runs.append(str(start) if start == end else f"{start}-{end}")
            start = end = n
    runs.append(str(start) if start == end else f"{start}-{end}")
    return ", ".join(runs)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--original", type=Path, required=True)
    ap.add_argument("--current", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    args = ap.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    target = args.out_dir / "main_comparison.pdf"
    assert not target.exists(), "Use a fresh run directory."
    paths = [args.original, args.current]
    assert [sha(p) for p in paths] == HASHES, "Source changed: review topic mapping first."
    docs = [fitz.open(p) for p in paths]
    assert [len(d) for d in docs] == [59, 50]
    assert all(p.rect == fitz.Rect(0, 0, 612, 792) for d in docs for p in d)
    out = fitz.open()
    guide = out.new_page(width=W, height=H)
    text(guide, (40, 30, 1230, 65), "NDNSF Proposal | Original and Current, Side by Side", 23, bold=True)
    text(guide, (40, 77, 1230, 140),
         "LEFT: Complete Origin proposal (59 pages).  RIGHT: Current main.pdf (50 pages).\n"
         "Clean pages at original size. No red/blue text markings. September 14, 2026.", 13)
    text(guide, (40, 142, 1230, 202),
         "Start with a topic below or use the PDF bookmarks. Pages are grouped by topic, not aligned sentence by sentence.\n"
         "Boundary pages may repeat across topics. A blank panel means that version has no further page in this group;\n"
         "it does not mean that the topic is absent. The original and current manuscripts are unchanged.", 12)
    starts, toc, records = [], [[1, "Guide and topic index", 1]], []
    seen = [Counter(), Counter()]
    for group_id, (title, old_pages, new_pages) in enumerate(GROUPS, 1):
        starts.append(len(out) + 1)
        toc.append([1, f"{group_id:02d}. {title}", len(out) + 1])
        total = max(len(old_pages), len(new_pages))
        for i in range(total):
            page = out.new_page(width=W, height=H)
            page.draw_rect((0, 0, W, 58), fill=(.95, .96, .97), color=None)
            text(page, (16, 8, 1190, 29), f"{group_id:02d}  {title}", 14, bold=True)
            text(page, (1190, 8, 1260, 29), f"{i+1} / {total}", 11)
            record = {"comparison_page": len(out), "topic": title, "panels": []}
            for side, source_pages in enumerate([old_pages, new_pages]):
                panel = PANELS[side]
                name = "ORIGINAL" if side == 0 else "CURRENT"
                source_page = source_pages[i] if i < len(source_pages) else None
                label = name
                if source_page is not None:
                    label += f" | Source PDF p. {source_page} / {len(docs[side])}"
                    if seen[side][source_page]:
                        label += " | Repeated for this topic"
                    seen[side][source_page] += 1
                    page.show_pdf_page(panel, docs[side], source_page - 1)
                else:
                    text(page, (panel.x0 + 55, 350, panel.x1 - 55, 435),
                         "No further page in this topic group.\n\n"
                         "Continue reading the other version,\nor use the next topic bookmark.", 14)
                text(page, (panel.x0 + 4, 35, panel.x1, 57), label, 11, bold=True)
                record["panels"].append({"version": name, "source_page": source_page})
            page.draw_line((636, 32), (636, 857), color=(.68, .71, .74), width=.7)
            text(page, (16, 862, 1180, 879),
                 "Source page numbers refer to PDF pages, not printed chapter page numbers. Topic pairing is not sentence alignment.", 8)
            text(page, (1190, 862, 1260, 879), str(len(out)), 8)
            records.append(record)
    # Fetch the guide again: adding pages invalidates earlier Page handles.
    guide = out[0]
    cols = [40, 680, 905, 1125]
    for x, label in zip(cols, ["Topic", "Original PDF pages", "Current PDF pages", "Open at"]):
        text(guide, (x, 220, x + 620 if x == 40 else min(x + 210, 1240), 243), label, 12, bold=True)
    guide.draw_line((40, 247), (1230, 247), color=(.55, .60, .65), width=.8)
    for row, ((title, a, b), start) in enumerate(zip(GROUPS, starts)):
        y = 256 + row * 26
        values = [f"{row+1:02d}  {title}", page_range(a), page_range(b), str(start)]
        for col, value in enumerate(values):
            width = [625, 215, 210, 100][col]
            text(guide, (cols[col], y, cols[col] + width, y + 22), value, 11)
        guide.insert_link({"kind": fitz.LINK_GOTO, "from": fitz.Rect(40, y, 1230, y + 22), "page": start - 1})
        guide.draw_line((40, y + 23), (1230, y + 23), color=(.84, .86, .88), width=.4)
    text(guide, (40, 768, 1230, 836),
         "Baseline: reference-pdfs/Tianxing_Dissertation_Proposal_Origin.pdf, not the 11-page annotated proposal 2.pdf.\n"
         "Current document checkpoint: 3b3f29f5. Full source hashes and page coverage are in main_comparison-report.json.\n"
         "This copy supports visual comparison only; it does not classify semantic changes or certify academic claims.", 11)
    out.set_toc(toc)
    out.set_metadata({"title": "NDNSF Proposal - Original / Current Side-by-Side Comparison",
                      "author": "Tianxing Ma", "subject": "Unmarked, topic-grouped full-page comparison"})
    out.save(target, deflate=True, garbage=4)
    check = fitz.open(target)
    verified, raster_deltas = [], []
    for record in records:
        page = check[record["comparison_page"] - 1]
        for side, item in enumerate(record["panels"]):
            n = item["source_page"]
            if n is None:
                continue
            source = docs[side][n - 1]
            assert page.get_text(clip=PANELS[side]) == source.get_text(), (record, side)
            # Embedding remains vector-based at 1:1 scale. MuPDF can rasterize
            # a few glyph-edge pixels differently after object relocation.
            a = source.get_pixmap(alpha=False)
            b = page.get_pixmap(clip=PANELS[side], alpha=False)
            assert (a.width, a.height) == (b.width, b.height)
            if a.samples != b.samples:
                diff = ImageChops.difference(
                    Image.frombytes("RGB", (a.width, a.height), a.samples),
                    Image.frombytes("RGB", (b.width, b.height), b.samples))
                hist = diff.histogram()
                fraction = (sum(hist) - hist[0] - hist[256] - hist[512]) / len(a.samples)
                mae = sum((i % 256) * count for i, count in enumerate(hist)) / len(a.samples)
                significant = sum(count for i, count in enumerate(hist) if i % 256 > 8) / len(a.samples)
                # Count material deltas, not widespread one-level RGB rounding.
                assert significant <= .001 and mae <= 1, ("Raster difference exceeds tolerance", record, side, significant, mae)
                raster_deltas.append({"comparison_page": record["comparison_page"], "side": side,
                                      "source_page": n, "changed_channel_fraction": fraction,
                                      "channel_fraction_with_error_over_8": significant,
                                      "mean_absolute_channel_error": mae, "bbox": diff.getbbox()})
            verified.append([record["comparison_page"], side, n])
    for side, source in enumerate(docs):
        assert set(seen[side]) == set(range(1, len(source) + 1)), "Missing source page"
    assert [sha(p) for p in paths] == HASHES
    report = {"status": "DOCUMENT_PASS", "method": "Topic-grouped full-page side-by-side; no difference overlays",
              "comparison_pages": len(check), "source_pages": [59, 50],
              "source_sha256": dict(zip([str(p) for p in paths], HASHES)),
              "output_sha256": sha(target), "page_size_points": [W, H], "source_scale": 1,
              "all_source_pages_covered": True, "text_identical_and_raster_checked_panels": len(verified),
              "raster_tolerance": {"channel_fraction_with_error_over_8_max": .001, "mean_channel_error_max": 1},
              "raster_deltas": raster_deltas,
              "groups": [{"topic": g[0], "original": g[1], "current": g[2], "starts_at": start}
                         for g, start in zip(GROUPS, starts)], "pages": records,
              "limits": ["Topic grouping is not sentence alignment.", "Boundary pages deliberately repeat.",
                         "Embedded pages are unchanged; internal source-PDF links are not copied."]}
    (args.out_dir / "main_comparison-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ["status", "comparison_pages", "all_source_pages_covered", "text_identical_and_raster_checked_panels"]}))


if __name__ == "__main__":
    main()
