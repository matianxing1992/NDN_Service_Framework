#!/usr/bin/env python3
"""Check STRICT_SERIAL document ordering, not implementation completion."""
import argparse
import re
from pathlib import Path


def check(text):
    errors = []
    task_id = r"T\d{3}(?:-[A-Za-z0-9]+)*"
    ids = re.findall(r"^- \[[ xX]\] (" + task_id + r")\b", text, re.M)
    if any("-" in task for task in ids):
        errors.append("executable child IDs unsupported: use ordered internal steps in one serial task")
    rows = []
    for line in text.splitlines():
        if re.match(r"\| \[T\d{3}\b", line):
            cells = line.split("|")
            rows.append((re.search(task_id, cells[1])[0],
                         re.findall(task_id, cells[3])))
    if not re.search(r"^\*\*Execution mode\*\*:\s*STRICT_SERIAL\b", text, re.M):
        errors.append("missing explicit STRICT_SERIAL Execution mode declaration")
    if not ids or len(ids) != len(set(ids)):
        errors.append("missing or duplicate task IDs")
    if [row[0] for row in rows] != ids:
        errors.append("progress order/coverage differs from task details")
    if re.search(r"^- \[[ xX]\] T\d{3} \[P\]", text, re.M):
        errors.append("parallel task in STRICT_SERIAL")
    seen = set()
    previous = None
    for task, deps in rows:
        if set(deps) - seen:
            errors.append(f"{task}: forward, self or unknown dependency")
        if previous and previous not in deps:
            errors.append(f"{task}: missing previous-task completion dependency")
        seen.add(task)
        previous = task
    return errors


def self_test():
    good = ("**Execution mode**: STRICT_SERIAL\n"
            "| [T001 First](#first) | NOT_STARTED | — | pending | now |\n"
            "| [T002 Second](#second) | NOT_STARTED | T001 | pending | now |\n"
            "- [ ] T001 First\n- [ ] T002 Second\n")
    assert not check(good)
    bad = [good.replace("| — |", "| T002 |"),
           good.replace("| T001 |", "| T099 |"),
           good.replace("| T001 |", "| — |"),
           good.replace("- [ ] T002 Second", "- [ ] T001 Second"),
           good.replace("- [ ] T002 Second", "- [ ] T002 [P] Second"),
           good.replace("STRICT_SERIAL", "BATCH"),
           good.replace("- [ ] T001 First\n- [ ] T002 Second",
                        "- [ ] T002 Second\n- [ ] T001 First"),
           good.replace("| T001 |", "| T001-Z |"),
           good.replace("T002", "T001-A"),
           good.replace("**Execution mode**: STRICT_SERIAL", "Documentation mentions STRICT_SERIAL")]
    assert all(check(case) for case in bad)
    print("PASS: valid chain + 10 invalid order/dependency fixtures")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tasks", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if args.tasks:
        findings = check(args.tasks.read_text(encoding="utf-8"))
        print("\n".join(findings) if findings else "PASS: serial task order and completion dependency chain")
        raise SystemExit(bool(findings))
    if not args.self_test:
        parser.error("provide tasks.md or --self-test")
