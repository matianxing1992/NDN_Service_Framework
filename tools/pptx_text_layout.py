"""Editable PowerPoint paragraph grouping and shape emission, independent of a paper project."""
from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import re
from pptx.dml.color import RGBColor
from pptx.enum.text import MSO_AUTO_SIZE, MSO_VERTICAL_ANCHOR, PP_ALIGN
from pptx.oxml.xmlchemy import OxmlElement
from pptx.util import Inches, Pt


SLIDE_W_IN = 16.0


SLIDE_H_IN = 9.0


STANDARD_FOOTER_TOP_IN = 8.55


FOOTER_BOTTOM_MARGIN_IN = 0.08


@dataclass(frozen=True)
class FontSpec:
    source_id: str
    size: float
    source_family: str
    mapped_family: str
    rgb: tuple[int, int, int]
    family_bold: bool
    family_italic: bool


@dataclass(frozen=True)
class InlineTextRun:
    text: str
    bold: bool
    italic: bool


@dataclass(frozen=True)
class TextSpan:
    span_id: int
    page: int
    page_w: float
    page_h: float
    left: float
    top: float
    width: float
    height: float
    font: FontSpec
    inline_runs: tuple[InlineTextRun, ...]

    @property
    def right(self) -> float:
        return self.left + self.width

    @property
    def bottom(self) -> float:
        return self.top + self.height

    @property
    def text(self) -> str:
        return "".join(run.text for run in self.inline_runs)


@dataclass(frozen=True)
class PdfPage:
    number: int
    width: float
    height: float
    spans: tuple[TextSpan, ...]


@dataclass(frozen=True)
class PdfDocument:
    pages: tuple[PdfPage, ...]

    @property
    def spans(self) -> tuple[TextSpan, ...]:
        return tuple(span for page in self.pages for span in page.spans)


@dataclass(frozen=True)
class TextGroup:
    group_id: int
    page: int
    spans: tuple[TextSpan, ...]
    line_start_span_ids: tuple[int, ...] = ()
    alignment: str = "left"


@dataclass(frozen=True)
class BulletMarker:
    page: int
    span: TextSpan


@dataclass(frozen=True)
class ConversionPlan:
    groups_by_page: dict[int, tuple[TextGroup, ...]]
    bullets_by_page: dict[int, tuple[BulletMarker, ...]]
    assignments: dict[int, str]
    adjacent_pairs: int
    multi_span_groups: int
    svs_citation_groups: int


def strip_subset_prefix(family: str) -> str:
    return re.sub(r"^[A-Z]{6}\+", "", family)


def is_msam_triangle(span: TextSpan) -> bool:
    family = strip_subset_prefix(span.font.source_family).upper()
    return family == "MSAM10" and span.text.strip() == "I"


def is_monospace_span(span: TextSpan) -> bool:
    """Return whether a span is source code that must keep hard line breaks."""

    family = strip_subset_prefix(span.font.source_family).lower()
    mapped = span.font.mapped_family.lower()
    return (
        "mono" in family
        or family.startswith("cmtt")
        or "mono" in mapped
        or "courier" in mapped
    )


def line_is_monospace(line: tuple[TextSpan, ...]) -> bool:
    return bool(line) and all(is_monospace_span(span) for span in line)


def vertical_overlap(first: TextSpan, second: TextSpan) -> float:
    return max(0.0, min(first.bottom, second.bottom) - max(first.top, second.top))


def same_visual_line(first: TextSpan, second: TextSpan) -> bool:
    minimum_height = min(first.height, second.height)
    if minimum_height <= 0:
        return False
    overlap_ratio = vertical_overlap(first, second) / minimum_height
    baseline_delta = abs(first.bottom - second.bottom)
    return (
        overlap_ratio >= 0.55
        and baseline_delta <= max(1.5, 0.32 * max(first.height, second.height))
    )


def adjacency_limit(first: TextSpan, second: TextSpan) -> float:
    average_font_size = (first.font.size + second.font.size) / 2.0
    return max(3.0, min(14.0, 0.80 * average_font_size))


def justified_adjacency_limit(first: TextSpan, second: TextSpan) -> float:
    average_font_size = (first.font.size + second.font.size) / 2.0
    # TeX stretches inter-word whitespace when it justifies a narrow table
    # cell.  Apply this wider limit only after a repeated column anchor; using
    # it globally would incorrectly combine independent diagram/footer labels.
    return max(4.0, min(28.0, 2.35 * average_font_size))


def should_group_adjacent(first: TextSpan, second: TextSpan) -> bool:
    if not same_visual_line(first, second):
        return False
    gap = second.left - first.right
    minimum_gap = -max(1.0, 0.12 * min(first.height, second.height))
    return minimum_gap <= gap <= adjacency_limit(first, second)


def build_visual_lines(spans: list[TextSpan]) -> list[list[TextSpan]]:
    visual_lines: list[list[TextSpan]] = []
    for span in sorted(spans, key=lambda item: (item.top, item.left)):
        candidates = [
            line
            for line in visual_lines
            if any(same_visual_line(span, member) for member in line)
        ]
        if not candidates:
            visual_lines.append([span])
            continue
        best = min(
            candidates,
            key=lambda line: min(
                abs(span.bottom - member.bottom) for member in line
            ),
        )
        best.append(span)
    return visual_lines


def repeated_left_anchors(spans: list[TextSpan]) -> tuple[float, ...]:
    """Return x positions that repeatedly start lines/columns on a page."""

    anchors: list[list[TextSpan]] = []
    for span in sorted(spans, key=lambda item: item.left):
        matching = next(
            (
                group
                for group in anchors
                if abs(group[0].left - span.left)
                <= max(1.5, 0.14 * span.font.size)
            ),
            None,
        )
        if matching is None:
            anchors.append([span])
        else:
            matching.append(span)
    return tuple(
        sum(span.left for span in group) / len(group)
        for group in anchors
        if len({round(span.top, 1) for span in group}) >= 2
    )


def starts_at_repeated_anchor(
    span: TextSpan, anchors: tuple[float, ...]
) -> bool:
    return any(
        abs(span.left - anchor) <= max(1.5, 0.14 * span.font.size)
        for anchor in anchors
    )


def is_standard_footer_span(span: TextSpan) -> bool:
    return (span.top / span.page_h) * SLIDE_H_IN >= STANDARD_FOOTER_TOP_IN


def build_line_clusters(spans: list[TextSpan]) -> tuple[list[tuple[TextSpan, ...]], int]:
    """Split each visual line into adjacent styled-span clusters."""

    clusters: list[tuple[TextSpan, ...]] = []
    adjacent_pairs = 0
    anchors = repeated_left_anchors(spans)
    for visual_line in build_visual_lines(spans):
        ordered = sorted(visual_line, key=lambda item: item.left)
        if not ordered:
            continue
        cluster: list[TextSpan] = [ordered[0]]
        for span in ordered[1:]:
            gap = span.left - cluster[-1].right
            minimum_gap = -max(1.0, 0.12 * min(cluster[-1].height, span.height))
            justified_word = (
                same_visual_line(cluster[-1], span)
                and not is_standard_footer_span(cluster[0])
                and starts_at_repeated_anchor(cluster[0], anchors)
                and not starts_at_repeated_anchor(span, anchors)
                and minimum_gap <= gap <= justified_adjacency_limit(cluster[-1], span)
            )
            crosses_column_boundary = (
                starts_at_repeated_anchor(cluster[0], anchors)
                and starts_at_repeated_anchor(span, anchors)
                and span.left - cluster[0].left
                > max(14.0, 1.30 * span.font.size)
            )
            ordinary_word = should_group_adjacent(cluster[-1], span)
            if ordinary_word or (justified_word and not crosses_column_boundary):
                cluster.append(span)
                adjacent_pairs += 1
                continue
            clusters.append(tuple(cluster))
            cluster = [span]
        clusters.append(tuple(cluster))
    clusters.sort(
        key=lambda line: (
            min(span.top for span in line),
            min(span.left for span in line),
        )
    )
    return clusters, adjacent_pairs


def line_left(line: tuple[TextSpan, ...]) -> float:
    return min(span.left for span in line)


def line_right(line: tuple[TextSpan, ...]) -> float:
    return max(span.right for span in line)


def line_top(line: tuple[TextSpan, ...]) -> float:
    return min(span.top for span in line)


def line_height(line: tuple[TextSpan, ...]) -> float:
    return max(span.height for span in line)


def line_font_size(line: tuple[TextSpan, ...]) -> float:
    return max(span.font.size for span in line)


def line_color(line: tuple[TextSpan, ...]) -> tuple[int, int, int]:
    return max(line, key=lambda span: len(span.text.strip())).font.rgb


def expected_line_text(line: tuple[TextSpan, ...]) -> str:
    parts: list[str] = []
    for index, span in enumerate(line):
        if index:
            parts.append(spacer_text(line[index - 1], span))
        parts.append(span.text)
    return "".join(parts)


def line_starts_with_bullet(
    line: tuple[TextSpan, ...], bullet_spans: list[TextSpan]
) -> bool:
    left = line_left(line)
    size = line_font_size(line)
    for bullet in bullet_spans:
        if not any(same_visual_line(bullet, span) for span in line):
            continue
        gap = left - bullet.right
        if -1.0 <= gap <= max(24.0, 2.4 * size):
            return True
    return False


LIST_MARKER_RE = re.compile(r"(?:\d+|[A-Za-z]|[ivxlcdmIVXLCDM]+)[.)]")


INLINE_LABEL_RE = re.compile(r"[A-Za-z]{1,4}\d+")


def paragraph_lane_left(line: tuple[TextSpan, ...]) -> float:
    """Return the body-column anchor for a labeled paragraph line.

    Description-style entries such as ``RQ1`` contain a short label followed
    by the paragraph body.  PDF extraction reports the first line's left edge
    at the label, while wrapped lines start at the body column.  Treating the
    body column as the paragraph anchor lets the resulting PPTX use one
    wrapping textbox instead of one textbox per visual line.
    """

    ordered = sorted(line, key=lambda span: span.left)
    if len(ordered) >= 2 and INLINE_LABEL_RE.fullmatch(ordered[0].text.strip()):
        return ordered[1].left
    return line_left(line)


def line_starts_with_list_marker(line: tuple[TextSpan, ...]) -> bool:
    """Return true for an explicit standalone list marker such as ``1.``."""

    first = min(line, key=lambda span: span.left)
    return LIST_MARKER_RE.fullmatch(first.text.strip()) is not None


def line_has_separate_sibling(
    line: tuple[TextSpan, ...], all_lines: list[tuple[TextSpan, ...]]
) -> bool:
    """Detect a table/column row that has another non-adjacent cell."""

    for sibling in all_lines:
        if sibling is line:
            continue
        if not any(
            same_visual_line(first, second)
            for first in line
            for second in sibling
        ):
            continue
        if line_right(sibling) < line_left(line):
            gap = line_left(line) - line_right(sibling)
        elif line_right(line) < line_left(sibling):
            gap = line_left(sibling) - line_right(line)
        else:
            continue
        if gap <= max(80.0, 7.0 * max(line_font_size(line), line_font_size(sibling))):
            return True
    return False


def paragraph_join_text(previous: TextSpan, following: TextSpan) -> str:
    previous_text = previous.text.rstrip()
    following_text = following.text.lstrip()
    if not previous_text or not following_text:
        return ""
    if previous_text.endswith(("-", "–", "/")):
        return ""
    if previous.text[-1:].isspace() or following.text[:1].isspace():
        return ""
    return " "


def can_merge_paragraph_line(
    paragraph_lines: list[tuple[TextSpan, ...]],
    following: tuple[TextSpan, ...],
    all_lines: list[tuple[TextSpan, ...]],
    bullet_spans: list[TextSpan],
) -> bool:
    previous = paragraph_lines[-1]
    # Verbatim/code blocks use indentation and explicit newlines as syntax.
    # Keeping each visual code line in its own non-wrapping textbox prevents
    # LibreOffice and Google Slides from replacing those newlines with spaces
    # or reflowing indentation after font substitution.
    if line_is_monospace(previous) or line_is_monospace(following):
        return False
    if line_starts_with_bullet(following, bullet_spans) or line_starts_with_list_marker(
        following
    ):
        return False

    top_delta = line_top(following) - line_top(previous)
    maximum_height = max(line_height(previous), line_height(following))
    if top_delta <= 0.20 * maximum_height or top_delta > 1.75 * maximum_height:
        return False

    previous_size = line_font_size(previous)
    following_size = line_font_size(following)
    if abs(previous_size - following_size) > max(
        1.0, 0.14 * max(previous_size, following_size)
    ):
        return False
    if line_color(previous) != line_color(following):
        return False

    previous_text = expected_line_text(previous).rstrip()
    hyphen_continuation = previous_text.endswith(("-", "–", "/"))
    if line_has_separate_sibling(following, all_lines) and not hyphen_continuation:
        return False

    anchor = paragraph_lines[0]
    anchor_lane_left = paragraph_lane_left(anchor)
    left_tolerance = max(3.0, 0.34 * max(previous_size, following_size))
    left_aligned = abs(line_left(following) - line_left(previous)) <= left_tolerance
    hanging_indent = (
        line_left(following) >= anchor_lane_left
        and line_left(following) - anchor_lane_left
        <= max(18.0, 1.7 * following_size)
    )
    previous_center = (line_left(previous) + line_right(previous)) / 2.0
    following_center = (line_left(following) + line_right(following)) / 2.0
    centered = abs(previous_center - following_center) <= max(
        5.0, 0.55 * following_size
    )
    return left_aligned or hanging_indent or centered


def shares_paragraph_lane(
    paragraph_lines: list[tuple[TextSpan, ...]],
    following: tuple[TextSpan, ...],
) -> bool:
    """Whether two lines occupy the same likely textbox column.

    This deliberately ignores row-start and punctuation rules.  The caller
    first finds the nearest line in the same lane and only then decides if it
    may merge.  That prevents a rejected new table row from attaching to an
    older row farther above.
    """

    previous = paragraph_lines[-1]
    top_delta = line_top(following) - line_top(previous)
    maximum_height = max(line_height(previous), line_height(following))
    if top_delta <= 0.20 * maximum_height or top_delta > 1.75 * maximum_height:
        return False

    previous_size = line_font_size(previous)
    following_size = line_font_size(following)
    if abs(previous_size - following_size) > max(
        1.0, 0.14 * max(previous_size, following_size)
    ):
        return False

    anchor = paragraph_lines[0]
    anchor_lane_left = paragraph_lane_left(anchor)
    left_tolerance = max(3.0, 0.34 * max(previous_size, following_size))
    left_aligned = abs(line_left(following) - line_left(previous)) <= left_tolerance
    hanging_indent = (
        line_left(following) >= anchor_lane_left
        and line_left(following) - anchor_lane_left
        <= max(18.0, 1.7 * following_size)
    )
    previous_center = (line_left(previous) + line_right(previous)) / 2.0
    following_center = (line_left(following) + line_right(following)) / 2.0
    centered = abs(previous_center - following_center) <= max(
        5.0, 0.55 * following_size
    )
    return left_aligned or hanging_indent or centered


def merge_line_clusters(
    lines: list[tuple[TextSpan, ...]], bullet_spans: list[TextSpan]
) -> list[list[tuple[TextSpan, ...]]]:
    """Merge PDF visual lines into paragraph-sized editable blocks."""

    paragraphs: list[list[tuple[TextSpan, ...]]] = []
    for line in lines:
        lane_candidates = [
            paragraph
            for paragraph in paragraphs
            if shares_paragraph_lane(paragraph, line)
        ]
        if not lane_candidates:
            paragraphs.append([line])
            continue
        nearest = min(
            lane_candidates,
            key=lambda paragraph: line_top(line) - line_top(paragraph[-1]),
        )
        if can_merge_paragraph_line(nearest, line, lines, bullet_spans):
            nearest.append(line)
        else:
            paragraphs.append([line])
    paragraphs.sort(
        key=lambda paragraph: (
            line_top(paragraph[0]),
            line_left(paragraph[0]),
        )
    )
    return paragraphs


def infer_text_alignment(
    page: PdfPage,
    paragraph_lines: list[tuple[TextSpan, ...]],
    bullet_spans: list[TextSpan],
) -> str:
    """Infer semantic paragraph alignment from the PDF line geometry."""

    if not paragraph_lines:
        return "left"
    first = paragraph_lines[0]
    if line_starts_with_bullet(first, bullet_spans) or line_starts_with_list_marker(first):
        return "left"
    if all(is_standard_footer_span(span) for line in paragraph_lines for span in line):
        return "left"

    # The title page is a deliberately centered composition.  Unlike normal
    # frame titles it has no left-aligned header/footer structure from which a
    # single-line alignment could be inferred.
    if page.number == 1:
        return "center"

    sizes = [line_font_size(line) for line in paragraph_lines]
    tolerance = max(2.0, 0.22 * max(sizes))
    centers = [(line_left(line) + line_right(line)) / 2.0 for line in paragraph_lines]
    lefts = [line_left(line) for line in paragraph_lines]
    if len(paragraph_lines) >= 2:
        center_spread = max(centers) - min(centers)
        left_spread = max(lefts) - min(lefts)
        if center_spread <= tolerance and left_spread > tolerance:
            return "center"
        return "left"

    line_width = line_right(first) - line_left(first)
    near_page_center = abs(centers[0] - page.width / 2.0) <= tolerance
    if (
        near_page_center
        and line_width <= 0.65 * page.width
        and line_top(first) / page.height * SLIDE_H_IN >= 1.0
    ):
        return "center"
    return "left"


def plan_conversion(document: PdfDocument) -> ConversionPlan:
    groups_by_page: dict[int, tuple[TextGroup, ...]] = {}
    bullets_by_page: dict[int, tuple[BulletMarker, ...]] = {}
    assignments: dict[int, str] = {}
    assignment_events: list[int] = []
    next_group_id = 1
    adjacent_pairs = 0
    multi_span_groups = 0
    svs_citation_groups = 0

    for page in document.pages:
        bullet_spans = [span for span in page.spans if is_msam_triangle(span)]
        text_spans = [span for span in page.spans if not is_msam_triangle(span)]
        bullets = tuple(BulletMarker(page.number, span) for span in bullet_spans)
        for bullet in bullets:
            assignments[bullet.span.span_id] = f"bullet-{bullet.span.span_id}"
            assignment_events.append(bullet.span.span_id)

        line_clusters, page_adjacent_pairs = build_line_clusters(text_spans)
        adjacent_pairs += page_adjacent_pairs
        paragraph_blocks = merge_line_clusters(line_clusters, bullet_spans)

        groups: list[TextGroup] = []
        for paragraph_lines in paragraph_blocks:
            ordered_spans = tuple(
                span
                for line in paragraph_lines
                for span in sorted(line, key=lambda item: item.left)
            )
            line_starts = tuple(
                min(line, key=lambda item: item.left).span_id
                for line in paragraph_lines[1:]
            )
            group = TextGroup(
                next_group_id,
                page.number,
                ordered_spans,
                line_starts,
                infer_text_alignment(page, paragraph_lines, bullet_spans),
            )
            groups.append(group)
            adjacent_pairs += max(0, len(paragraph_lines) - 1)
            next_group_id += 1

        groups.sort(key=lambda group: (group.spans[0].top, group.spans[0].left))
        for group in groups:
            assignment_name = f"text-group-{group.group_id}"
            for span in group.spans:
                if span.span_id in assignments:
                    raise RuntimeError(
                        f"Text span {span.span_id} was assigned more than once"
                    )
                assignments[span.span_id] = assignment_name
                assignment_events.append(span.span_id)
            if len(group.spans) > 1:
                multi_span_groups += 1
            texts = [span.text.strip() for span in group.spans]
            if any("SVS" in text for text in texts) and any(
                text.startswith("[3]") for text in texts
            ):
                svs_citation_groups += 1

        groups_by_page[page.number] = tuple(groups)
        bullets_by_page[page.number] = bullets

    counts = Counter(assignment_events)
    source_ids = {span.span_id for span in document.spans}
    assigned_ids = set(counts)
    missing = sorted(source_ids - assigned_ids)
    unexpected = sorted(assigned_ids - source_ids)
    duplicated = sorted(span_id for span_id, count in counts.items() if count != 1)
    if missing or unexpected or duplicated:
        raise RuntimeError(
            "Invalid source-span assignment: "
            f"missing={missing[:10]}, unexpected={unexpected[:10]}, "
            f"duplicated={duplicated[:10]}"
        )

    return ConversionPlan(
        groups_by_page=groups_by_page,
        bullets_by_page=bullets_by_page,
        assignments=assignments,
        adjacent_pairs=adjacent_pairs,
        multi_span_groups=multi_span_groups,
        svs_citation_groups=svs_citation_groups,
    )


def x_inches(value: float, page: PdfPage) -> float:
    return value / page.width * SLIDE_W_IN


def y_inches(value: float, page: PdfPage) -> float:
    return value / page.height * SLIDE_H_IN


def font_size_points(span: TextSpan) -> float:
    return span.font.size / span.page_w * SLIDE_W_IN * 72.0


def remove_shape_line(shape) -> None:
    line = shape._element.spPr.get_or_add_ln()
    for child in list(line):
        line.remove(child)
    line.append(OxmlElement("a:noFill"))


def add_powerpoint_run(paragraph, span: TextSpan, inline: InlineTextRun) -> None:
    run = paragraph.add_run()
    run.text = inline.text
    run.font.name = span.font.mapped_family
    run.font.size = Pt(font_size_points(span))
    run.font.bold = span.font.family_bold or inline.bold
    run.font.italic = span.font.family_italic or inline.italic
    run.font.color.rgb = RGBColor(*span.font.rgb)


def spacer_text(first: TextSpan, second: TextSpan) -> str:
    if first.text[-1:].isspace() or second.text[:1].isspace():
        return ""
    gap = second.left - first.right
    if gap <= max(0.4, 0.04 * first.font.size):
        return ""
    # A logical paragraph must use semantic whitespace.  Reproducing the PDF's
    # justified x-gap with several literal spaces makes Google Slides and
    # LibreOffice wrap at different positions after font substitution.
    return " "


def expected_group_text(group: TextGroup) -> str:
    parts: list[str] = []
    line_starts = set(group.line_start_span_ids)
    preserve_centered_lines = has_centered_visual_line_geometry(group)
    for index, span in enumerate(group.spans):
        if index:
            previous = group.spans[index - 1]
            parts.append(
                (
                    "\v"
                    if preserve_centered_lines
                    else paragraph_join_text(previous, span)
                )
                if span.span_id in line_starts
                else spacer_text(previous, span)
            )
        parts.append(span.text)
    return "".join(parts)


def has_centered_visual_line_geometry(group: TextGroup) -> bool:
    """Whether a centered group contains intentionally centered PDF lines.

    Center alignment alone is insufficient: the title-page fallback and some
    narrow table cells can be centered even when their PDF lines are ordinary
    wrapped prose.  Preserve hard line breaks only when the source-line
    bounding boxes themselves share a common center.
    """

    if group.alignment != "center" or not group.line_start_span_ids:
        return False

    line_starts = set(group.line_start_span_ids)
    lines: list[list[TextSpan]] = [[]]
    for index, span in enumerate(group.spans):
        if index and span.span_id in line_starts:
            lines.append([])
        lines[-1].append(span)

    centers = [
        (min(span.left for span in line) + max(span.right for span in line)) / 2.0
        for line in lines
    ]
    largest_font = max(font_size_points(span) for span in group.spans)
    return max(centers) - min(centers) <= max(3.0, 0.30 * largest_font)


def is_standard_footer_group(page: PdfPage, group: TextGroup) -> bool:
    source_top = min(span.top for span in group.spans)
    return y_inches(source_top, page) >= STANDARD_FOOTER_TOP_IN


def add_text_group(slide, page: PdfPage, group: TextGroup) -> int:
    left = min(span.left for span in group.spans)
    top = min(span.top for span in group.spans)
    right = max(span.right for span in group.spans)
    bottom = max(span.bottom for span in group.spans)
    x = x_inches(left, page)
    y = y_inches(top, page)
    source_width = x_inches(right - left, page)
    source_height = y_inches(bottom - top, page)
    largest_font_height = max(font_size_points(span) for span in group.spans) / 72.0
    is_multiline = bool(group.line_start_span_ids)
    is_monospace = all(is_monospace_span(span) for span in group.spans)
    width_padding = (
        0.14
        if is_multiline
        else max(0.12, 0.60 * largest_font_height, 0.08 * source_width)
    )
    if is_monospace:
        width_padding = max(width_padding, 0.22)
    height_padding = largest_font_height * (0.52 if is_multiline else 0.30)
    if group.alignment == "center":
        x = max(0.0, x - width_padding / 2.0)
    width = min(SLIDE_W_IN - x, max(0.03, source_width + width_padding))
    is_frame_title = y < 1.0 and max(
        font_size_points(span) for span in group.spans
    ) >= 20.0
    title_left_inset = 0.0
    if is_frame_title:
        # A title's PDF glyph bounds are not its layout bounds.  Give it the
        # full content width so substituted fonts do not wrap a nominally
        # single-line title above the slide in Google Slides/LibreOffice.
        if group.alignment == "center":
            x = 0.30
            width = SLIDE_W_IN - 0.60
        else:
            # Give the glyphs a small safety area to the left of the visible
            # title anchor. LibreOffice can otherwise clip the negative left
            # bearing of the first glyph after import/save (notably the "P" in
            # "Preliminary"). The matching text-frame inset keeps the visible
            # text at the source PDF position.
            title_left_inset = min(0.10, x)
            x -= title_left_inset
            # Use nearly all remaining slide width. LibreOffice can otherwise
            # rewrap a title after save even when the first import rendered it
            # on one line.
            width = max(width, SLIDE_W_IN - x - 0.05)
    desired_height = max(
        0.04,
        source_height + height_padding,
        largest_font_height * (1.42 if is_multiline else 1.28),
    )
    if is_standard_footer_group(page, group):
        height = min(
            SLIDE_H_IN - FOOTER_BOTTOM_MARGIN_IN, desired_height
        )
        y = min(
            y,
            SLIDE_H_IN - FOOTER_BOTTOM_MARGIN_IN - height,
        )
    else:
        height = min(SLIDE_H_IN - y, desired_height)
    textbox = slide.shapes.add_textbox(
        Inches(x), Inches(y), Inches(width), Inches(height)
    )
    textbox.name = f"PDF_TEXT_GROUP_{group.group_id}"
    textbox.fill.background()
    remove_shape_line(textbox)
    frame = textbox.text_frame
    frame.clear()
    frame.auto_size = MSO_AUTO_SIZE.NONE
    # Frame titles are semantically one line.  Explicitly disabling wrapping
    # survives the Google Slides/LibreOffice import-save path better than
    # relying on a font-specific glyph-width fit. Body paragraphs still wrap.
    frame.word_wrap = not (is_frame_title or is_monospace)
    frame.vertical_anchor = MSO_VERTICAL_ANCHOR.TOP
    frame.margin_left = Inches(title_left_inset)
    frame.margin_right = 0
    frame.margin_top = 0
    frame.margin_bottom = 0
    paragraph = frame.paragraphs[0]
    paragraph.alignment = (
        PP_ALIGN.CENTER if group.alignment == "center" else PP_ALIGN.LEFT
    )
    paragraph.space_before = Pt(0)
    paragraph.space_after = Pt(0)
    paragraph.line_spacing = 1.0
    if line_starts_with_list_marker((group.spans[0],)) and len(group.spans) >= 2:
        marker = group.spans[0]
        body = group.spans[1]
        if body.span_id not in set(group.line_start_span_ids):
            hanging_indent = x_inches(max(0.0, body.left - marker.left), page)
            paragraph_properties = paragraph._p.get_or_add_pPr()
            hanging_indent_emu = int(Inches(hanging_indent))
            paragraph_properties.set("marL", str(hanging_indent_emu))
            paragraph_properties.set("indent", str(-hanging_indent_emu))

    run_count = 0
    line_starts = set(group.line_start_span_ids)
    preserve_centered_lines = has_centered_visual_line_geometry(group)
    for index, span in enumerate(group.spans):
        prefix = ""
        if index:
            previous = group.spans[index - 1]
            if span.span_id in line_starts and preserve_centered_lines:
                # Centered diagram labels and cover-page blocks use their PDF
                # line boundaries as intentional layout, not prose wrapping.
                paragraph.add_line_break()
            else:
                prefix = (
                    paragraph_join_text(previous, span)
                    if span.span_id in line_starts
                    else spacer_text(previous, span)
                )
        for inline_index, inline in enumerate(span.inline_runs):
            # LibreOffice and Google Slides may discard a whitespace-only run
            # between two styled spans.  Prefix semantic whitespace to the
            # following visible run so the saved PPTX retains word boundaries.
            visible_inline = (
                InlineTextRun(prefix + inline.text, inline.bold, inline.italic)
                if prefix and inline_index == 0
                else inline
            )
            add_powerpoint_run(paragraph, span, visible_inline)
            run_count += 1
    return run_count
