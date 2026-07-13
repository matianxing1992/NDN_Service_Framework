#!/usr/bin/env python3
"""Run Spec 110 offline unittest suites and emit dependency-free JUnit XML."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import time
import traceback
import unittest
import xml.etree.ElementTree as ET


class JUnitResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.records = {}
        self.started = {}

    def startTest(self, test):
        self.started[test.id()] = time.monotonic()
        self.records[test.id()] = {"test": test, "status": "success", "message": ""}
        super().startTest(test)

    def stopTest(self, test):
        self.records[test.id()]["duration"] = time.monotonic() - self.started.pop(test.id())
        super().stopTest(test)

    def addFailure(self, test, err):
        self.records[test.id()].update(status="failure", message="".join(traceback.format_exception(*err)))
        super().addFailure(test, err)

    def addError(self, test, err):
        self.records[test.id()].update(status="error", message="".join(traceback.format_exception(*err)))
        super().addError(test, err)

    def addSkip(self, test, reason):
        self.records[test.id()].update(status="skipped", message=reason)
        super().addSkip(test, reason)


def write_junit(result: JUnitResult, path: Path, elapsed: float) -> None:
    suite = ET.Element("testsuite", {
        "name": "spec110-offline-foundation",
        "tests": str(result.testsRun),
        "failures": str(len(result.failures)),
        "errors": str(len(result.errors)),
        "skipped": str(len(result.skipped)),
        "time": f"{elapsed:.6f}",
    })
    for identity in sorted(result.records):
        record = result.records[identity]
        case = ET.SubElement(suite, "testcase", {
            "classname": record["test"].__class__.__module__ + "." + record["test"].__class__.__name__,
            "name": record["test"]._testMethodName,
            "time": f"{record.get('duration', 0.0):.6f}",
        })
        if record["status"] in {"failure", "error", "skipped"}:
            node = ET.SubElement(case, record["status"], {"message": record["status"]})
            node.text = record["message"]
    path.parent.mkdir(parents=True, exist_ok=True)
    tree = ET.ElementTree(suite)
    tree.write(path, encoding="utf-8", xml_declaration=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    unit = repo / "tests/container/itiger-qwen-live/unit"
    contract = repo / "tests/container/itiger-qwen-live/contract"
    sys.path.insert(0, str(unit))
    loader = unittest.TestLoader()
    suite = unittest.TestSuite([
        loader.discover(str(unit), pattern="test_*.py", top_level_dir=str(unit)),
        loader.discover(str(contract), pattern="test_*.py", top_level_dir=str(contract)),
    ])
    started = time.monotonic()
    runner = unittest.TextTestRunner(verbosity=2, resultclass=JUnitResult)
    result = runner.run(suite)
    elapsed = time.monotonic() - started
    write_junit(result, Path(args.output), elapsed)
    print(
        f"SPEC110_OFFLINE tests={result.testsRun} failures={len(result.failures)} "
        f"errors={len(result.errors)} skipped={len(result.skipped)} duration={elapsed:.6f}s"
    )
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
