#!/usr/bin/env python3
"""Read-only validation for the Spec182 revision 2 document contract."""
from pathlib import Path
import json
import re
import sys
from urllib.parse import unquote

feature = Path(__file__).resolve().parents[1]
normative = [
    "spec.md", "plan.md", "tasks.md", "traceability.md", "audit.md",
    "contracts/code-design.md", "contracts/proof-design.md",
    "contracts/work-units.md", "contracts/runtime-boundaries.md",
    "checklists/requirements.md",
]
texts = {name: (feature / name).read_text() for name in normative}
errors = []
def require(condition, message):
    if not condition:
        errors.append(message)

def slug(title):
    title = re.sub(r"[^\w\s-]", "", title.strip().lower())
    return title.replace(" ", "-")

task_rows = re.findall(
    r"^- \[([ xX])\] (T\d{3}) \[US\d+\].*$",
    texts["tasks.md"], re.M)
ids = [item[1] for item in task_rows]
require(ids == [f"T{i:03d}" for i in range(1, 18)], "expected ordered T001--017")
require(all(state == " " for state, _ in task_rows), "implementation tasks must remain unchecked")
dependencies = {}
for line in texts["tasks.md"].splitlines():
    match = re.match(r"^- \[ \] (T\d{3}).*Dependencies:(.*)", line)
    if match:
        dependencies[match[1]] = re.findall(r"T\d{3}", match[2])
for task in ids:
    require(task in dependencies, f"missing dependencies: {task}")
    require(f"## {task} " in texts["contracts/work-units.md"], f"missing work unit: {task}")
    for dependency in dependencies.get(task, []):
        require(dependency in ids, f"unknown dependency: {dependency}")
visiting, visited = set(), set()
def visit(task):
    if task in visiting:
        errors.append(f"dependency cycle at {task}")
        return
    if task in visited:
        return
    visiting.add(task)
    for dependency in dependencies.get(task, []):
        visit(dependency)
    visiting.remove(task)
    visited.add(task)
for task in ids:
    visit(task)
require(dependencies.get("T015") == ["T014"], "audit must follow harness")
require(dependencies.get("T016") == ["T015"], "qualification must follow audit")
require({"T008", "T009"}.issubset(dependencies.get("T010", [])), "requester missing preparation/host")
require("G2 / T010--012" in texts["plan.md"], "requester/binding gate range differs from tasks")
require("G3 / T013--014" in texts["plan.md"], "migration/harness gate range differs from tasks")
for name, content in texts.items():
    for start, end in re.findall(r"T(\d{3})--(?:T)?(\d{3})", content):
        require(int(start) <= int(end) <= 17, f"invalid task range in {name}")
    for block in re.split(r"\n\s*\n", content):
        if block.startswith("|"):
            require(bool(re.search(r"^\|[ :|\-]+\|$", block, re.M)),
                    f"orphan Markdown table rows in {name}")
for prefix, count in [("FR", 16), ("SC", 8), ("CD", 14), ("PO", 14)]:
    for i in range(1, count + 1):
        symbol = f"{prefix}-{i:03d}"
        require(symbol in texts["traceability.md"], f"untraced {symbol}")
        target = texts["spec.md"] if prefix in ("FR", "SC", "CD") else texts["contracts/proof-design.md"]
        require(symbol in target, f"missing definition {symbol}")
links = 0
for path in list(feature.rglob("*.md")):
    content = path.read_text()
    for label, raw_target in re.findall(r"\[([^\]]+)\]\(([^)]+)\)", content):
        target = raw_target.strip().strip("<>")
        if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target):
            continue
        filename, _, anchor = target.partition("#")
        destination = (path.parent / unquote(filename)).resolve() if filename else path
        links += 1
        require(destination.is_file(), f"missing link {path.name}: {target}")
        if destination.is_file() and anchor and destination.suffix == ".md":
            headings = re.findall(r"^#{1,6} (.+)$", destination.read_text(), re.M)
            require(unquote(anchor) in {slug(h) for h in headings},
                    f"missing anchor {path.name}: {target}")
# Validate each JSON evidence file without interpreting historical snapshots as current.
for path in (feature / "evidence").glob("*.json"):
    try:
        json.loads(path.read_text())
    except ValueError as exc:
        errors.append(f"invalid JSON {path.name}: {exc}")
report = {"schema": "spec182-revision2-document-validation-v1",
          "ok": not errors, "tasks": len(ids),
          "tasks_complete": sum(state.lower() == "x" for state, _ in task_rows),
          "fr": 16, "sc": 8, "cd": 14, "po": 14,
          "local_links_checked": links, "dependency_graph": dependencies,
          "errors": errors, "runtime_tests": "NOT_RUN"}
print(json.dumps(report, ensure_ascii=False, indent=2))
sys.exit(0 if not errors else 1)
