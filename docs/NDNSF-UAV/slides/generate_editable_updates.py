#!/usr/bin/env python3
"""Export the UAV update PDF using the proposal's editable exporter.

Text stays native PowerPoint content; diagrams/photos are independent pictures.
This wrapper changes presentation representation, not scientific slide content.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import tempfile

from pptx import Presentation

HERE = Path(__file__).resolve().parent
PROPOSAL = HERE.parents[1] / "PAPER/proposal-defense/slides"
sys.path.insert(0, str(PROPOSAL))
import generate_hybrid_editable_pptx as exporter

REPAIRS = {
    "as-signed": "assigned",
    "calibra-tion": "calibration",
    "align-ment": "alignment",
    "uncer-tainty": "uncertainty",
    "cali-bration": "calibration",
}
LINKS = {
    "MAVSDK control/telemetry": "https://mavsdk.mavlink.io/main/en/cpp/index.html",
    "OpenCV calibration/geometry": "https://docs.opencv.org/4.13.0/d9/d0c/group__calib3d.html",
    "SIYI official store": "https://shop.siyi.biz/products/siyi-a8-mini-gimbal-camera",
    "Gremsy official site": "https://gremsy.com/products/pixy-u",
    "MAVLink capture metadata": "https://mavlink.io/en/messages/common.html#CAMERA_IMAGE_CAPTURED",
}


def repaired(text):
    for before, after in REPAIRS.items():
        text = text.replace(before, after)
    return text


def add_link(paragraph, label, url):
    """Split one source run while preserving its formatting and text order."""
    for run in list(paragraph.runs):
        if label not in run.text:
            continue
        before, after = run.text.split(label, 1)
        parent = run._r.getparent()
        position = parent.index(run._r)
        for text, linked in ((before, False), (label, True), (after, False)):
            if not text:
                continue
            new = paragraph.add_run()
            new.text = text
            if run._r.rPr is not None:
                new._r.insert(0, deepcopy(run._r.rPr))
            if linked:
                new.hyperlink.address = url
            parent.remove(new._r)
            parent.insert(position, new._r)
            position += 1
        parent.remove(run._r)
        return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    # The shared exporter owns and clears only a marked, dedicated build dir.
    if args.build_dir:
        build = args.build_dir.resolve()
    else:
        build = Path(tempfile.mkdtemp(prefix="ndnsf-uav-editable-")) / "ndnsf-export"
    pdf = HERE / "UPDATES_UAV.pdf"
    output = HERE / "UPDATES_UAV.pptx"
    exporter.SLIDE_VISUAL_REGION_SPECS = (
        exporter.SlideVisualRegionSpec(
            ("What Is Near This Map Pin?",),
            (exporter.VisualRegion("UAV_SCENE", 1.32, 2.10, 13.38, 5.10),)),
        exporter.SlideVisualRegionSpec(
            ("Complementary Evidence Supports a Joint Judgment",),
            (exporter.VisualRegion("PROTOCOL_FLOW", 1.45, 1.15, 12.95, 1.78),)),
        exporter.SlideVisualRegionSpec(
            ("SIYI A8 mini", "Gremsy Pixy U"),
            (exporter.VisualRegion("SIYI_PHOTO", 3.02, 2.27, 2.22, 2.22),
             exporter.VisualRegion("GREMSY_PHOTO", 10.52, 2.27, 2.25, 2.87))),
    )
    sys.argv = [str(Path(exporter.__file__)), "--pdf", str(pdf),
                "--output", str(output), "--build-dir", str(build), "--no-notes"]
    exporter.main()
    prs = Presentation(output)
    found_links = set()
    corrections = 0
    for slide in prs.slides:
        for shape in slide.shapes:
            if not shape.has_text_frame:
                continue
            # Wrapped table cells use multiple equal-style PDF runs. Collapse
            # only these repaired cells, not mixed-style prose or labels.
            if repaired(shape.text) != shape.text:
                paragraph = shape.text_frame.paragraphs[0]
                runs = list(paragraph.runs)
                text = repaired(shape.text)
                runs[0].text = text
                for run in runs[1:]:
                    paragraph._p.remove(run._r)
                corrections += 1
            for paragraph in shape.text_frame.paragraphs:
                for label, url in LINKS.items():
                    if add_link(paragraph, label, url):
                        found_links.add(label)
    assert found_links == set(LINKS), "A source hyperlink was not restored"
    prs.save(output)
    saved = Presentation(output)
    manifest = json.loads((build / exporter.MANIFEST_NAME).read_text())
    pages = []
    for slide, original in zip(saved.slides, manifest["pages"]):
        text_shapes = {s.name: s for s in slide.shapes
                       if s.has_text_frame and s.text.strip()}
        for group in original["text_groups"]:
            actual = text_shapes[f'PDF_TEXT_GROUP_{group["group_id"]}'].text
            assert " ".join(actual.split()) == " ".join(repaired(group["text"]).split())
        for shape in slide.shapes:
            assert shape.left >= 0 and shape.top >= 0
            assert shape.left + shape.width <= saved.slide_width + 2
            assert shape.top + shape.height <= saved.slide_height + 2
        pages.append({"page": original["page"], "editable_textboxes": len(text_shapes),
                      "independent_figures": len(original["visual_regions"])})
    assert len(saved.slides) == len(manifest["pages"]) == 5
    report = {
        "status": "PASS", "scope": "object-model and source-text preservation",
        "source_pdf_sha256": hashlib.sha256(pdf.read_bytes()).hexdigest(),
        "pptx_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "source_spans": sum(len(page["spans"]) for page in manifest["pages"]),
        "pages": pages,
        "repaired_pdf_discretionary_hyphen_cells": corrections,
        "restored_hyperlinks": len(found_links),
        "visual_review": "Separate renderer review required",
    }
    (build / "uav-editable-validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    print(f"Build evidence: {build}")


if __name__ == "__main__":
    main()
