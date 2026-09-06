#!/usr/bin/env python3
"""Render/check the documentary evidence inventory; never qualify a runtime."""

import argparse
import hashlib
from pathlib import Path
import re
import sys


FEATURES = (
    "180-ack-driven-cross-model-qualification",
    "181-ndnsf-di-protected-grant-qualification",
)
REPORT = Path("specs") / FEATURES[1] / "evidence/t007-evidence-inventory-20260905.md"


def field(lines, name):
    pattern = re.compile(r"(?:\*\*(?:" + name + r")\*\*|^(?:" + name + r"))\s*:\s*(.*)", re.I)
    for line in lines[:15]:
        match = pattern.search(line)
        if match:
            return match.group(1).strip()
    return "MISSING_HEADER"


def cell(value):
    return value.replace("|", "&#124;").replace("\n", " ")


def render(root):
    records = []
    for feature in FEATURES:
        directory = Path("specs") / feature
        paths = {directory / "audit.md"}
        paths.update(p.relative_to(root) for p in (root / directory / "evidence").rglob("*")
                     if p.is_file())
        if feature == FEATURES[1]:
            paths.add(REPORT)
        for path in sorted(paths):
            active = feature == FEATURES[1]
            if path == REPORT:
                digest, layer, status = "SELF", "executed (document inventory)", "PASS (inventory only)"
                marker = "SELF"
            else:
                data = (root / path).read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                lines = data.decode("utf-8").splitlines()
                layer = field(lines, r"(?:Evidence\s+)?Layer")
                status = field(lines, "Status")
                if layer == "MISSING_HEADER" and not active:
                    section_layers = [line for line in lines[:15] if re.match(
                        r"^#+.*\((?:proposed|implemented|wired|executed|measured)\)", line, re.I)]
                    if section_layers:
                        layer = "SECTION_ONLY: " + "; ".join(section_layers)
                    elif re.match(r"`?IMPLEMENTED`? boundary only", status, re.I):
                        layer = "implemented only (explicit Status boundary)"
                marker = "PRESENT" if re.search(
                    r"INVALIDATED|DO NOT REUSE|SUPERSEDED|FROZEN",
                    "\n".join(lines[:15]), re.I) else "NOT_DECLARED"
                if path.suffix != ".md":
                    layer, status = "NON_MARKDOWN", "SEE_PAYLOAD"
                if active and layer == "MISSING_HEADER":
                    raise ValueError("ACTIVE_EVIDENCE_LAYER_MISSING:" + str(path))
            use = "SCOPED_RECORD" if active else "HISTORICAL_ONLY"
            if path == REPORT:
                use = "INVENTORY_ONLY"
            records.append((path, digest, layer, status, marker, use))

    old = sum(str(p).startswith("specs/" + FEATURES[0] + "/") for p, *_ in records)
    new = len(records) - old
    lines = [
        "# Complete Evidence Inventory",
        "",
        "**Status**: PASS (inventory coverage only)",
        "**Evidence layer**: executed (document inventory; no runtime qualification)",
        "",
        "## Scope and Interpretation",
        "",
        f"完整输入为 Spec180 的 {old} 个文件和 Spec181 的 {new} 个文件：",
        "各 feature 的全部 evidence 文件及根级 audit.md；没有省略中间行。",
        "本报告也列入清单，SELF 行不计算自身哈希以避免循环；其头部层声明显式给出。",
        "",
        "HISTORICAL_ONLY：保留冻结原文，所有 PASS 仅保留原修订/源身份下的限定含义，",
        "不得直接用作 Spec181 候选、本地矩阵、SIF 或 Tiger 资格。既有失效横幅仍有效。",
        "头部没有层声明的历史文件保留 MISSING_HEADER，不根据 PASS 字样推断 executed",
        "或 measured；本清单对其只声明 historical document evidence。",
        "层/状态按前 15 行的显式字段提取；SECTION_ONLY 只标记已声明的段落层，",
        "不据此推断整份文档的层级。显式 implemented-only 状态保留原限制。",
        "",
        "SCOPED_RECORD：按记录自己的源身份、测试层和限制使用；旧 partial/OPEN 是",
        "当时检查点，不能覆盖当前 [tasks.md](../tasks.md)、[audit.md](../audit.md) 和",
        "[T002 acceptance](t002-acceptance-20260905.md)。其中 PASS 不自动晋升为正式资格。",
        "",
        "Spec180 的 JSON 是历史契约检查输出；其 readinessScope 为",
        "DOCUMENT_AND_TRUST_ROOT_CONTRACT、qualificationReady=false，不能解释为运行通过。",
        "当前替代权威为 Spec181 spec/plan/tasks/audit；T001--T006 的验收映射见",
        "[traceability.md](../traceability.md)。正式资格必须由 T005/T008--T012 各自证据证明；",
        "本清单不作任务完成裁决。Qwen 模型资格仍在范围外。逐文件 disposition 由下表承接冻结文件适用范围。",
        "",
        "## Reproduction",
        "",
        "`python3 scripts/spec181_evidence_inventory.py` 输出完整清单；",
        "`python3 scripts/spec181_evidence_inventory.py --check` 比较当前文件集合、",
        "SHA-256、头部声明和本报告。只检查文档覆盖与漂移，不验证底层实验真伪。",
        "任一输入文档变化后须重新生成；检查通过不等于 T007 或运行资格 PASS。",
        "",
        "## Per-file Inventory",
        "",
        "| File | SHA-256 | Declared header layer | Declared header status | Retirement marker in header | Current use |",
        "|---|---|---|---|---|---|",
    ]
    for path, digest, layer, status, marker, use in records:
        relative = "../../../" + path.as_posix()
        lines.append("| " + " | ".join([
            f"[{path.as_posix()}]({relative})", f"`{digest}`", cell(layer),
            cell(status), marker, use]) + " |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    expected = render(root)
    if args.check:
        if not (root / REPORT).is_file() or (root / REPORT).read_text() != expected:
            raise ValueError("EVIDENCE_INVENTORY_STALE")
        print("SPEC181_EVIDENCE_INVENTORY_OK")
    else:
        sys.stdout.write(expected)


if __name__ == "__main__":
    main()
