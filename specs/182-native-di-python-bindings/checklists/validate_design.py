#!/usr/bin/env python3
"""Read-only validation for the Spec182 document structure and phased validation contract."""
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
    "checklists/requirements.md", "contracts/symbol-design.md", "contracts/value-contracts.md", "contracts/pre-test-static-review.md",
    "contracts/execution-units.md",
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

dependencies = {}
for line in texts["tasks.md"].splitlines():
    match = re.match(r"^- \[[ xX]\] (T\d{3}).*Dependencies:(.*)", line)
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
for prefix, count in [("FR", 19), ("SC", 11), ("CD", 14), ("PO", 16)]:
    for i in range(1, count + 1):
        symbol = f"{prefix}-{i:03d}"
        require(symbol in texts["traceability.md"], f"untraced {symbol}")
        target = texts["spec.md"] if prefix in ("FR", "SC", "CD") else texts["contracts/proof-design.md"]
        require(symbol in target, f"missing definition {symbol}")
# Structural coverage is not semantic/ABI or runtime qualification.
symbol_text = texts["contracts/symbol-design.md"]
field_text = texts["contracts/value-contracts.md"]
for prefix, count in [("C", 21), ("M", 48)]:
    found = re.findall(r"^\| (" + prefix + r"\d{2})[ |]", symbol_text, re.M)
    require(found == [f"{prefix}{i:02d}" for i in range(1, count + 1)],
            f"{prefix} ledger missing, duplicate, or out of order")
coverage = json.loads((feature / "contracts/source-field-coverage.json").read_text())
field_count = 0
for record in coverage["records"]:
    require(f"## {record['id']} {record['symbol']}" in field_text, "missing source type")
    for i, field in enumerate(record["fields"], 1):
        field_count += 1
        field_id = f"{record['id']}.F{i:02d}"
        require(len(re.findall(r"^\| " + re.escape(field_id) + r" \|", field_text, re.M)) == 1,
                f"field coverage missing or duplicate: {field_id}")
        require(field["name"] in field_text, f"missing field name: {field_id}")
for task in ids:
    block = re.search(r"^## " + task + r" [^\n]+\n(.*?)(?=^## |\Z)",
                      texts["contracts/work-units.md"], re.M | re.S)
    require(block is not None, f"missing contract: {task}")
    if block:
        for key in ["Outcome", "Design", "Changes", "ForbiddenChanges", "LocalChecks", "FinalProof"]:
            require(f"**{key}**:" in block.group(1), f"{task} missing {key}")
require("FR-017" in texts["spec.md"] and "SC-009" in texts["traceability.md"],
        "missing documentation requirement/criterion")
require("maxRoles、maxNodes" not in texts["contracts/code-design.md"], "stale CandidateBudget")
require("RuntimeStatusStore" in texts["contracts/runtime-boundaries.md"], "merged security boundary absent")
# Check only the written workflow; this is not a semantic source review.
review_text = texts["contracts/pre-test-static-review.md"]
for marker in ["Static review PASS != Behavior PASS", "## One Completion Record",
               "## Spec182 Ownership", "T002--T014", "T015", "T016", "最小具名诊断"]:
    require(marker in review_text, f"missing workflow rule: {marker}")
for name in ["spec.md", "plan.md", "tasks.md", "contracts/proof-design.md"]:
    require("pre-test-static-review.md" in texts[name], f"workflow not referenced: {name}")
require("完整unit→integration→MiniNDN" in texts["tasks.md"], "final runtime stage order missing")
for name in ["contracts/work-units.md", "contracts/proof-design.md"]:
    require("T016" in texts[name], f"final runtime owner missing: {name}")
for i in range(2, 15):
    task = f"T{i:03d}"
    block = re.search(r"^## " + task + r" [^\n]+\n(.*?)(?=^## |\Z)",
                      texts["contracts/work-units.md"], re.M | re.S)
    require(block is not None and "T016" in block[1], f"runtime obligation not transferred: {task}")
for marker in ["PostTestReview**:", "StaticReview**:", "AllowedTestScope", "TestEntryChecks"]:
    require(marker not in texts["contracts/work-units.md"], f"duplicate workflow form: {marker}")
links = 0
# Validate dispatch coverage and dependency edges, not model capability or readiness.
card_text = texts["contracts/execution-units.md"]
cards = {}
for match in re.finditer(r"^### (T\d{3}-[A-Z]) [^\n]+\n(.*?)(?=^### |^## |\Z)",
                         card_text, re.M | re.S):
    card, body = match.groups()
    require(card not in cards, f"duplicate execution card: {card}")
    cards[card] = body
card_dependencies = {}
for card, body in cards.items():
    parent = re.search(r"\*\*Parent\*\*: (T\d{3})", body)
    require(parent is not None and parent[1] == card[:4] and parent[1] in ids,
            f"invalid card parent: {card}")
    for key in ["Depends", "Reviewer", "Read", "Write", "Steps", "Verify"]:
        require(f"**{key}**:" in body, f"{card} missing {key}")
    dep = re.search(r"\*\*Depends\*\*: ([^;\n]+)", body)
    card_dependencies[card] = re.findall(r"T\d{3}-[A-Z]", dep[1] if dep else "")
    for other in card_dependencies[card]:
        require(other in cards, f"unknown card dependency {card}: {other}")
    write = re.search(r"\*\*Write\*\*: ([^\n]+)", body)
    require(write is not None and '*' not in write[1] and '/' in write[1],
            f"missing exact write paths: {card}")
require(all(any(card.startswith(task + '-') for card in cards) for task in ids),
        "parent task missing execution cards")
card_visiting, card_visited = set(), set()
def visit_card(card):
    if card in card_visiting:
        errors.append(f"execution card dependency cycle: {card}")
        return
    if card in card_visited:
        return
    card_visiting.add(card)
    for other in card_dependencies.get(card, []):
        if other in cards:
            visit_card(other)
    card_visiting.remove(card)
    card_visited.add(card)
for card in cards:
    visit_card(card)
for name in ["tasks.md", "plan.md", "contracts/work-units.md"]:
    require("execution-units.md" in texts[name], f"dispatch entry missing: {name}")
# The current registry is complete and consistent with the executable inventory.
progress_block = re.search(r"^## Execution Progress\n(.*?)(?=^## |\Z)",
                           texts["tasks.md"], re.M | re.S)
require(progress_block is not None, "missing Execution Progress")
progress = {}
for line in (progress_block[1] if progress_block else "").splitlines():
    cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
    if not cells or not re.match(r"\[T\d{3}-[A-Z] ", cells[0]):
        continue
    card = re.match(r"\[(T\d{3}-[A-Z]) ", cells[0])[1]
    require(card not in progress, f"duplicate progress row: {card}")
    require(len(cells) == 5, f"invalid progress columns: {card}")
    if len(cells) != 5:
        continue
    _, state, deps, evidence, updated = cells
    require(state in {"NOT_STARTED", "READY", "IN_PROGRESS", "PARTIAL", "BLOCKED", "DONE"},
            f"invalid progress status: {card}")
    require(re.findall(r"T\d{3}-[A-Z]", deps) == card_dependencies.get(card),
            f"progress dependencies differ: {card}")
    require(re.fullmatch(r"\d{4}-\d{2}-\d{2}", updated) is not None,
            f"missing progress date: {card}")
    require("execution-units.md#" + card.lower() + "-" in cells[0],
            f"wrong progress detail link: {card}")
    if state == "DONE":
        require(bool(re.search(r"\[[^]]+\]\([^)]*evidence/[^)]+\)", evidence))
                and "task-progress-registry-20260907.md" not in evidence,
                f"DONE requires actual unit acceptance evidence: {card}")
    progress[card] = state
require(set(progress) == set(cards), "progress/card coverage differs")
for card, state in progress.items():
    if state in {"READY", "IN_PROGRESS", "DONE"}:
        require(all(progress.get(dep) == "DONE" for dep in card_dependencies.get(card, [])),
                f"progress dependencies not DONE: {card}")
for state, parent in task_rows:
    if state.lower() == "x":
        require(all(progress.get(card) == "DONE" for card in cards if card.startswith(parent + "-")),
                f"parent complete before children: {parent}")
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
        require(destination.is_file() or destination.is_dir(),
                f"missing link {path.name}: {target}")
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
report = {"schema": "spec182-document-validation-v2",
          "ok": not errors, "tasks": len(ids),
          "tasks_complete": sum(state.lower() == "x" for state, _ in task_rows),
          "execution_cards": len(cards), "progress_units": len(progress),
          "progress_status_counts": {state: list(progress.values()).count(state) for state in sorted(set(progress.values()))}, "card_dependencies": card_dependencies,
          "fr": 19, "sc": 11, "cd": 14, "po": 16,
          "source_types": len(coverage["records"]), "source_fields": field_count,
          "classes_or_modules": 21, "methods": 48, "design_readiness": "DRAFT",
          "local_links_checked": links, "dependency_graph": dependencies,
          "errors": errors, "runtime_tests": "NOT_RUN", "product_static_review": "NOT_RUN"}
print(json.dumps(report, ensure_ascii=False, indent=2))
sys.exit(0 if not errors else 1)
