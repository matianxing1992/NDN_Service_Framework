#!/usr/bin/env python3
"""Build an image-led, editable three-slide NDNSF-UAV dataset brief."""

from pathlib import Path

from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.util import Inches, Pt


ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "assets" / "multi-camera-tracking"
OUTPUT = ROOT / "multi-camera-vehicle-tracking-brief.pptx"

NAVY = RGBColor(15, 39, 71)
TEAL = RGBColor(0, 148, 160)
ORANGE = RGBColor(235, 126, 48)
INK = RGBColor(35, 43, 54)
MUTED = RGBColor(93, 105, 119)
PALE = RGBColor(244, 247, 250)
WHITE = RGBColor(255, 255, 255)
RED = RGBColor(190, 55, 55)

SLIDE_W = Inches(13.333)
SLIDE_H = Inches(7.5)


def set_bg(slide, color=WHITE):
    fill = slide.background.fill
    fill.solid()
    fill.fore_color.rgb = color


def text(slide, value, x, y, w, h, size=18, color=INK, bold=False,
         align=PP_ALIGN.LEFT, font="Aptos", valign=MSO_ANCHOR.TOP):
    box = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    frame = box.text_frame
    frame.clear()
    frame.word_wrap = True
    frame.margin_left = Inches(0.04)
    frame.margin_right = Inches(0.04)
    frame.margin_top = Inches(0.02)
    frame.margin_bottom = Inches(0.02)
    frame.vertical_anchor = valign
    p = frame.paragraphs[0]
    p.alignment = align
    run = p.add_run()
    run.text = value
    run.font.name = font
    run.font.size = Pt(size)
    run.font.bold = bold
    run.font.color.rgb = color
    return box


def title(slide, heading, kicker):
    text(slide, kicker.upper(), 0.45, 0.22, 4.0, 0.25, 10, TEAL, True)
    text(slide, heading, 0.43, 0.48, 12.45, 0.52, 26, NAVY, True)
    line = slide.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(0.45), Inches(1.05), Inches(12.42), Inches(0.025))
    line.fill.solid()
    line.fill.fore_color.rgb = TEAL
    line.line.fill.background()


def footer(slide, value, warn=False):
    color = RED if warn else MUTED
    text(slide, value, 0.47, 7.14, 12.35, 0.18, 8.5, color, False)


def add_panel(slide, x, y, w, h, fill=PALE, line_color=None, radius=False):
    shape = slide.shapes.add_shape(
        MSO_SHAPE.ROUNDED_RECTANGLE if radius else MSO_SHAPE.RECTANGLE,
        Inches(x), Inches(y), Inches(w), Inches(h),
    )
    shape.fill.solid()
    shape.fill.fore_color.rgb = fill
    shape.line.color.rgb = line_color or fill
    return shape


def add_picture_contain(slide, path, x, y, w, h, border=WHITE):
    # Keep the complete source image visible; the surrounding panel supplies a stable frame.
    panel = add_panel(slide, x, y, w, h, WHITE, border)
    slide.shapes.add_picture(str(path), Inches(x + 0.03), Inches(y + 0.03),
                             width=Inches(w - 0.06), height=Inches(h - 0.06))
    return panel


def arrow(slide, x1, y1, x2, y2, color=TEAL, width=2.2):
    line = slide.shapes.add_connector(1, Inches(x1), Inches(y1), Inches(x2), Inches(y2))
    line.line.color.rgb = color
    line.line.width = Pt(width)
    line.line.end_arrowhead = True
    return line


def make_slide_1(prs):
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    set_bg(slide)
    title(slide, "Three synchronized UAV views of the same traffic scene", "Dataset")
    text(slide, "A small, image-led pilot for cross-camera vehicle tracking", 0.48, 1.16, 8.0, 0.3, 14, MUTED)

    files = [ASSETS / "uav1.jpg", ASSETS / "uav2.jpg", ASSETS / "uav3.jpg"]
    labels = ["UAV 1", "UAV 2", "UAV 3"]
    for i, (path, label) in enumerate(zip(files, labels)):
        x = 0.45 + i * 4.17
        add_picture_contain(slide, path, x, 1.62, 3.95, 3.12)
        add_panel(slide, x, 4.42, 1.05, 0.33, NAVY, NAVY, radius=True)
        text(slide, label, x + 0.05, 4.47, 0.95, 0.18, 10, WHITE, True, PP_ALIGN.CENTER)

    add_panel(slide, 0.45, 5.10, 12.40, 1.55, PALE, PALE, radius=True)
    text(slide, "What the release provides", 0.72, 5.32, 2.5, 0.25, 14, NAVY, True)
    text(slide, "3 synchronized one-minute UAV clips", 0.72, 5.70, 3.3, 0.28, 13, INK, True)
    text(slide, "Per-camera boxes + local tracker IDs", 4.37, 5.70, 3.3, 0.28, 13, INK, True)
    text(slide, "Cross-camera global IDs and overlap regions", 8.02, 5.70, 4.2, 0.28, 13, INK, True)
    text(slide, "Use as a synchronized tracking pilot; it is not a broad recognition or calibrated 3-D benchmark.",
         0.72, 6.16, 11.4, 0.28, 12, RED, False)
    footer(slide, "Source: Hugging Face dataset card and project repository; raw videos are not copied into this repository.", True)


def make_slide_2(prs):
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    set_bg(slide)
    title(slide, "Reference algorithm: local tracks to global ID", "Algorithm")
    text(slide, "The repository supplies an engineering baseline, not a new tracking method.", 0.48, 1.16, 8.0, 0.3, 14, MUTED)

    add_picture_contain(slide, ASSETS / "Pipeline.png", 0.50, 1.65, 8.45, 4.30, border=RGBColor(220, 226, 232))
    add_panel(slide, 9.22, 1.65, 3.62, 4.30, PALE, PALE, radius=True)
    steps = [
        ("1", "YOLO detector", "Find vehicles in each UAV frame."),
        ("2", "ByteTrack", "Maintain a camera-local track."),
        ("3", "Overlap matching", "Compare compatible tracks in overlap regions."),
        ("4", "global_id", "Export a cross-camera identity and trajectory."),
    ]
    y = 1.92
    for number, heading, detail in steps:
        add_panel(slide, 9.48, y, 0.42, 0.42, TEAL, TEAL, radius=True)
        text(slide, number, 9.48, y + 0.08, 0.42, 0.2, 12, WHITE, True, PP_ALIGN.CENTER)
        text(slide, heading, 10.06, y - 0.01, 2.45, 0.22, 13, NAVY, True)
        text(slide, detail, 10.06, y + 0.25, 2.42, 0.35, 10.5, INK)
        y += 0.93

    add_panel(slide, 0.50, 6.16, 12.34, 0.58, NAVY, NAVY, radius=True)
    text(slide, "Important boundary: global_id is an application association result, not an NDN producer identity or cryptographic proof.",
         0.78, 6.33, 11.8, 0.2, 12, WHITE, True)
    footer(slide, "Source: project README; the implementation uses Ultralytics YOLO, Supervision/ByteTrack, and overlap-region matching.")


def make_slide_3(prs):
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    set_bg(slide)
    title(slide, "NDNSF-UAV mapping: each selected stream is a Provider", "Integration")
    text(slide, "Keep the video payload out of Request and Selection; publish bounded Named Data after Selection.",
         0.48, 1.16, 11.3, 0.3, 14, MUTED)

    # Image-led provider row.
    providers = [("/uav/A", ASSETS / "uav1.jpg"), ("/uav/B", ASSETS / "uav2.jpg"), ("/uav/C", ASSETS / "uav3.jpg")]
    for i, (name, path) in enumerate(providers):
        x = 0.52 + i * 2.62
        add_picture_contain(slide, path, x, 1.60, 2.18, 1.32, border=RGBColor(220, 226, 232))
        text(slide, name, x, 2.99, 2.18, 0.22, 12, NAVY, True, PP_ALIGN.CENTER)

    add_panel(slide, 8.30, 1.60, 2.18, 1.32, RGBColor(226, 241, 242), TEAL, radius=True)
    text(slide, "Tracking\nProvider", 8.43, 1.92, 1.92, 0.48, 15, NAVY, True,
         PP_ALIGN.CENTER, valign=MSO_ANCHOR.MIDDLE)
    add_panel(slide, 10.92, 1.60, 1.88, 1.32, RGBColor(239, 235, 247), RGBColor(135, 107, 171), radius=True)
    text(slide, "User", 11.10, 2.05, 1.52, 0.28, 16, NAVY, True,
         PP_ALIGN.CENTER, valign=MSO_ANCHOR.MIDDLE)
    for x in [2.70, 5.32, 7.94]:
        arrow(slide, x, 2.25, x + 0.56, 2.25)
    arrow(slide, 10.48, 2.25, 10.90, 2.25, ORANGE)

    # Editable protocol strip.
    stages = [
        ("Request", "protected descriptor"),
        ("ACK", "positive / negative"),
        ("Selection", "role assignment"),
        ("View Data", "signed named object"),
        ("Result", "tracking output"),
    ]
    x = 0.48
    for i, (heading, detail) in enumerate(stages):
        width = 2.38 if i < 4 else 2.10
        add_panel(slide, x, 3.60, width, 0.83, PALE, RGBColor(210, 218, 226), radius=True)
        text(slide, heading, x + 0.08, 3.73, width - 0.16, 0.20, 12.5, NAVY, True, PP_ALIGN.CENTER)
        text(slide, detail, x + 0.08, 4.01, width - 0.16, 0.18, 9.8, MUTED, False, PP_ALIGN.CENTER)
        if i < len(stages) - 1:
            arrow(slide, x + width + 0.04, 4.01, x + width + 0.24, 4.01, ORANGE, 1.8)
        x += width + 0.22

    add_panel(slide, 0.48, 4.85, 12.34, 1.42, NAVY, NAVY, radius=True)
    text(slide, "Protocol tests enabled by this dataset", 0.76, 5.06, 4.1, 0.25, 14, WHITE, True)
    text(slide, "wrong producer", 0.76, 5.48, 1.7, 0.22, 12, RGBColor(255, 207, 138), True)
    text(slide, "undeclared dependency", 3.20, 5.48, 2.25, 0.22, 12, RGBColor(255, 207, 138), True)
    text(slide, "missing or late stream", 6.20, 5.48, 2.15, 0.22, 12, RGBColor(255, 207, 138), True)
    text(slide, "stale attempt / cache replay", 9.05, 5.48, 2.90, 0.22, 12, RGBColor(255, 207, 138), True)
    text(slide, "The Tracking Provider accepts View Data only after producer, dependency, attempt, and cache checks.",
         0.76, 5.87, 11.4, 0.22, 12, WHITE, False)
    footer(slide, "NDNSF mapping is the proposed project integration; it is not a claim made by the dataset authors.", True)


def main():
    for required in [ASSETS / "Pipeline.png", ASSETS / "uav1.jpg", ASSETS / "uav2.jpg", ASSETS / "uav3.jpg"]:
        if not required.is_file():
            raise SystemExit(f"missing asset: {required}")

    prs = Presentation()
    prs.slide_width = SLIDE_W
    prs.slide_height = SLIDE_H
    # Remove the default empty slide if a template ever supplies one.
    while prs.slides:
        rel = prs.slides._sldIdLst[0].rId
        prs.part.drop_rel(rel)
        del prs.slides._sldIdLst[0]

    make_slide_1(prs)
    make_slide_2(prs)
    make_slide_3(prs)
    prs.save(OUTPUT)
    print(f"wrote {OUTPUT} ({len(prs.slides)} slides)")


if __name__ == "__main__":
    main()
