#!/usr/bin/env python3
"""Count production callers of the compatibility modules moved by Spec 111."""

from __future__ import annotations

import ast
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCAN_ROOTS = (
    ROOT / "examples/python/NDNSF-DistributedInference",
    ROOT / "Experiments",
    ROOT / "packaging",
)
LEGACY = {
    "ndnsf_distributed_inference",
    "ndnsf_distributed_inference.app",
    "ndnsf_distributed_inference.controller",
    "ndnsf_distributed_inference.gui",
    "ndnsf_distributed_inference.onnx_graph",
    "ndnsf_distributed_inference.onnx_executor",
    "ndnsf_distributed_inference.qwen_pilot",
    "ndnsf_distributed_inference.llm_runtime",
    "ndnsf_distributed_inference.planner_registry",
    "ndnsf_distributed_inference.split_planner",
}

def main() -> int:
    matches = []
    scanned = 0
    for root in SCAN_ROOTS:
        for path in root.rglob("*.py"):
            if any(part in {"build", "dist", "__pycache__"} for part in path.parts):
                continue
            scanned += 1
            try:
                tree = ast.parse(path.read_text(encoding="utf-8"))
            except SyntaxError as error:
                matches.append({"path": str(path.relative_to(ROOT)),
                                "line": error.lineno, "module": "SYNTAX_ERROR"})
                continue
            for node in ast.walk(tree):
                if isinstance(node, ast.ImportFrom) and node.module in LEGACY:
                    matches.append({"path": str(path.relative_to(ROOT)),
                                    "line": node.lineno, "module": node.module})
                elif isinstance(node, ast.Import):
                    for alias in node.names:
                        if alias.name in LEGACY:
                            matches.append({"path": str(path.relative_to(ROOT)),
                                            "line": node.lineno, "module": alias.name})
    payload = {
        "schema": "ndnsf-di-spec111-compatibility-callers-v1",
        "scannerDigest": "sha256:" + hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "roots": [str(path.relative_to(ROOT)) for path in SCAN_ROOTS],
        "scannedPythonFiles": scanned,
        "productionCallerCount": len(matches),
        "matches": matches,
        "verdict": "PASS" if not matches else "FAIL",
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if not matches else 1

if __name__ == "__main__":
    raise SystemExit(main())
