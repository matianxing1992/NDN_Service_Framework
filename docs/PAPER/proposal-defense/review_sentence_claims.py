#!/usr/bin/env python3
"""Inventory reviewed LaTeX sentences/claim rows; never certify their truth.

Run after manual source review. Source lines and hashes make coverage inspectable.
Table rows and diagram labels are claim units, not grammatical sentences. Evidence
families route to the companion manual report; they are not per-unit proof results.
"""
from pathlib import Path
import hashlib
import json
import re
from collections import Counter

BASE = Path(__file__).resolve().parent
FILES = ["main.tex", "en/main.tex", "main_ch.tex", "ch/main.tex",
         "en/chapters/research-revision.tex", "en/chapters/authorization-rationale.tex",
         "ch/chapters/research-revision.tex", "ch/chapters/authorization-rationale.tex",
         "slides/research-slides.tex", "protocol-overview.tex"]

def family(text, context):
    t = (context + " " + text).lower()
    if any(w in t for w in ["259.4", "539/600", "historical loss", "negative result", "历史", "小模型诊断"]):
        return "HISTORICAL_PROVENANCE_OPEN"
    if any(w in t for w in ["cascon", "novelty", "创新性"]):
        return "LITERATURE_COMPARISON_OPEN"
    if any(w in t for w in ["holdout", "1312", "1,312", "1,311", "seed", "1,798", "7180", "7,180", "357", "覆盖控制"]):
        return "RECORDED_EXPERIMENT_SCOPED"
    if any(w in t for w in ["february", "march 26", "spring 2027", "candidacy", "two weeks", "两周", "研究计划"]):
        return "SCHEDULE_SOURCE_AND_INTERNAL_TARGET"
    if "dnmp" in t:
        return "DNMP_PRIMARY_OR_EXPLICIT_ADAPTATION"
    if any(w in t for w in ["withdraw", "撤销", "challenge", "or policy", "or 策略", "controller-wide", "服务权限管理"]):
        return "AUTHORIZATION_SOURCE_AND_ASSUMPTIONS"
    if any(w in t for w in ["kv", "k/v", "onnx", "tensor", "pipeline partition", "模型切分"]):
        return "MODEL_SEMANTICS_AND_PROPOSED_DI"
    if any(w in t for w in ["ndn already", "what ndn", "sync", "interest", "trust schema", "已有能力"]):
        return "NDN_PRIMARY_SEMANTICS"
    return "PROPOSAL_DESIGN_SCOPE_OR_RESEARCH_METHOD"

def blocks(path):
    lines = path.read_text().splitlines()
    abstract_only = path.name in {"main.tex", "main_ch.tex"}
    active = not abstract_only
    context = "Abstract" if abstract_only else ""
    frame = 0
    pending, start = [], None
    def flush():
        nonlocal pending, start
        if not pending:
            return []
        result = [(start, context, frame, " ".join(pending))]
        pending, start = [], None
        return result
    for number, line in enumerate(lines, 1):
        s = re.sub(r"(?<!\\)%.*$", "", line).strip()
        if abstract_only and s == r"\begin{abstract}":
            active = True
            continue
        if abstract_only and s == r"\begin{keywords}":
            yield from flush()
            active = False
        if not active:
            continue
        heading = re.search(r"\\(?:chapter|section|subsection|paragraph)\{([^}]+)\}|\\begin\{frame\}(?:\[[^]]*\])?\{([^}]+)\}", s)
        if heading:
            yield from flush()
            context = heading.group(1) or heading.group(2)
            if r"\begin{frame}" in s:
                frame += 1
            yield number, context, frame, s
            continue
        if r"\begin{frame}[plain]" in s:
            frame += 1
        if not s:
            yield from flush()
            continue
        if s.startswith((r"\begin{", r"\end{", r"\label", r"\input", r"\column", r"\draw", r"\appendix", r"\centering", r"\bottomrule")):
            yield from flush()
            continue
        if s in [r"\small", r"\scriptsize", r"\singlespacing"]:
            continue
        if s.startswith((r"\item", r"\node", r"\caption", r"\refentry", r"\vspace", r"\bluebox")) or " & " in s:
            yield from flush()
            yield number, context, frame, s
        else:
            if start is None:
                start = number
            pending.append(s)
    yield from flush()

units = []
for name in FILES:
    for line, context, frame, block in blocks(BASE / name):
        # Do not split table cells, captions, math, or abbreviations into fake prose.
        structural = block.startswith("\\") or " & " in block
        parts = [block] if structural else re.split(r"(?<=[。！？])|(?<=[.!?])\s+(?=[A-Z])", block)
        for part in filter(None, (p.strip() for p in parts)):
            units.append({"id": f"U{len(units)+1:04d}", "file": name,
                          "source_line": line, "section_or_title": context,
                          "slide": frame or None, "latex": part,
                          "kind": "claim-row-or-label" if structural else "prose-sentence",
                          "evidence_family": family(part, context),
                          "review": "WORDING_PLACEMENT_SCOPE_REVIEWED",
                          "evidence_detail": "sentence-review-20260911.md#evidence-boundaries"})

report = {"date": "2026-09-11", "scope": FILES,
          "method": "Manual reading of all listed sources; deterministic navigation inventory afterwards. Sentence splitting is approximate. No per-sentence formal proof or fresh experiment certification.",
          "source_sha256": {n: hashlib.sha256((BASE/n).read_bytes()).hexdigest() for n in FILES},
          "counts": dict(Counter(u['kind'] for u in units)),
          "evidence_families": dict(Counter(u['evidence_family'] for u in units)),
          "units": units}
(BASE / "sentence-claim-ledger-20260911.json").write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n")
print(json.dumps({"units": len(units), "counts": report['counts'], "files": len(FILES)}))
