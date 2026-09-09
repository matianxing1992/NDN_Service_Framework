#!/usr/bin/env python3
"""Build deterministic review artifacts, not a semantic correctness certificate."""
import argparse
import hashlib
import json
import math
import re
from pathlib import Path
from unicodedata import normalize

import fitz
from PIL import Image, ImageDraw


def latex_escape(text):
    text = normalize("NFKC", text.replace("ℓ", "ell"))
    mapping = {"\\": r"\textbackslash{}", "&": r"\&", "%": r"\%",
               "$": r"\$", "#": r"\#", "_": r"\_", "{": r"\{",
               "}": r"\}", "~": r"\textasciitilde{}", "^": r"\textasciicircum{}"}
    return "".join(mapping.get(c, c) for c in text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdfs", nargs="+", type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--notes", action="store_true", help="Generate notes for the first PDF")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    reports = []
    for doc_index, path in enumerate(args.pdfs):
        doc = fitz.open(path)
        label = f"{doc_index + 1}-{path.stem}"
        pages, thumbs = [], []
        notes = [r"\documentclass[11pt]{article}", r"\usepackage[margin=.8in]{geometry}",
                 r"\usepackage[T1]{fontenc}", r"\usepackage[utf8]{inputenc}",
                 r"\usepackage{lmodern}", r"\usepackage{amsmath,amssymb}",
                 r"\newcommand{\slideentry}[3]{\section*{Slide #1: #2}#3\par}",
                 r"\begin{document}", r"\section*{Presenter Reference Notes -- September 2026}",
                 "Generated from the revised deck. These notes preserve slide claims and evidence limits; they are not an independent evidence source."]
        for i, page in enumerate(doc):
            # Presentation footer metadata is not speaker-note content.
            clip = fitz.Rect(0, 0, page.rect.width, page.rect.height * .94) if args.notes and doc_index == 0 else None
            text = page.get_text(clip=clip)
            lines = [s.strip() for s in text.splitlines() if s.strip() and
                     not s.startswith("Tianxing Ma") and not re.fullmatch(r"\d+/\d+", s.strip())]
            spans = [s for b in page.get_text("dict")["blocks"] if "lines" in b
                     for l in b["lines"] for s in l["spans"] if s["text"].strip()]
            outside = [s["text"] for s in spans if s["bbox"][0] < -1 or s["bbox"][1] < -1 or
                       s["bbox"][2] > page.rect.width + 1 or s["bbox"][3] > page.rect.height + 1]
            pages.append({"page": i+1, "title": lines[0] if lines else "", "words": len(" ".join(lines).split()),
                          "text_outside_page": outside, "min_font_pt": min((s["size"] for s in spans), default=0)})
            pix = page.get_pixmap(matrix=fitz.Matrix(1.4, 1.4), alpha=False)
            im = Image.frombytes("RGB", [pix.width, pix.height], pix.samples)
            im.save(args.out / f"{label}-p{i+1:02d}.png")
            im.thumbnail((480, 355))
            tile = Image.new("RGB", (500, 385), "#e6e6e6")
            tile.paste(im, ((500-im.width)//2, 20))
            ImageDraw.Draw(tile).text((10, 365), f"{label} / {i+1}", fill="black")
            thumbs.append(tile)
            if args.notes and doc_index == 0:
                title = lines[0] if lines else f"Page {i+1}"
                body = " ".join(lines[1:])
                body = body.replace("−", "-").replace("–", "--").replace("—", "---")
                body = body.replace("→", " to ").replace("≤", " <= ").replace("≥", " >= ")
                body = body.replace("ﬁ", "fi").replace("ﬂ", "fl").replace("▶", "").replace("•", "")
                notes.append(r"\slideentry{"+str(i+1)+"}{"+latex_escape(title)+"}{\n"+latex_escape(body)+"\n}")
        for start in range(0, len(thumbs), 12):
            block = thumbs[start:start+12]
            sheet = Image.new("RGB", (2000, 385*math.ceil(len(block)/4)), "white")
            for j, tile in enumerate(block):
                sheet.paste(tile, ((j%4)*500, (j//4)*385))
            sheet.save(args.out / f"{label}-contact-{start//12+1}.png")
        reports.append({"file": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "pages": pages})
        if args.notes and doc_index == 0:
            notes.append(r"\end{document}")
            (args.out / "speaker_notes.tex").write_text("\n".join(notes)+"\n")
    (args.out / "render-audit.json").write_text(json.dumps(reports, ensure_ascii=False, indent=2)+"\n")
    print(json.dumps([{ "file": r["file"], "pages": len(r["pages"]),
        "outside_page": sum(bool(p["text_outside_page"]) for p in r["pages"]),
        "over_100_words": [p["page"] for p in r["pages"] if p["words"]>100]} for r in reports]))


if __name__ == "__main__":
    main()
