from __future__ import annotations

import importlib.util
from pathlib import Path
import sys


REPO = Path(__file__).resolve().parents[4]
TOOLS = REPO / "tools" / "ndnsf-di"
FIXTURES = REPO / "tests" / "container" / "itiger-qwen-live" / "fixtures"
DIGEST_A = "sha256:" + "a" * 64


def load_tool(name: str):
    key = "spec110_test_" + name
    if key in sys.modules:
        return sys.modules[key]
    spec = importlib.util.spec_from_file_location(key, TOOLS / f"{name}.py")
    if spec is None or spec.loader is None:
        raise ImportError(name)
    module = importlib.util.module_from_spec(spec)
    sys.modules[key] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop(key, None)
        raise
    return module
