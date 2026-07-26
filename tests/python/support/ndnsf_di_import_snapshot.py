"""Import a target while denying optional modules and report loaded modules."""

from __future__ import annotations

import argparse
import builtins
import importlib
import json
import sys
from collections.abc import Iterable


def import_snapshot(target: str, blocked: Iterable[str] = ()) -> dict[str, object]:
    denied = tuple(str(item) for item in blocked)
    original_import = builtins.__import__

    def guarded_import(name, globals=None, locals=None, fromlist=(), level=0):
        if any(name == prefix or name.startswith(prefix + ".") for prefix in denied):
            raise ModuleNotFoundError(f"blocked optional import: {name}", name=name)
        return original_import(name, globals, locals, fromlist, level)

    before = set(sys.modules)
    builtins.__import__ = guarded_import
    try:
        importlib.import_module(target)
    finally:
        builtins.__import__ = original_import
    loaded = sorted(set(sys.modules) - before)
    forbidden_loaded = sorted(
        name for name in sys.modules
        if any(name == prefix or name.startswith(prefix + ".") for prefix in denied)
    )
    return {
        "schema": "ndnsf-di-import-snapshot-v1",
        "target": target,
        "blocked": list(denied),
        "loaded": loaded,
        "forbiddenLoaded": forbidden_loaded,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("target")
    parser.add_argument("--blocked", action="append", default=[])
    args = parser.parse_args(argv)
    try:
        result = import_snapshot(args.target, args.blocked)
    except Exception as exc:  # emitted for an isolated subprocess caller
        print(json.dumps({"error": type(exc).__name__, "message": str(exc)}))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
