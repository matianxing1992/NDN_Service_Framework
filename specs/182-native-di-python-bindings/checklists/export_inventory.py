#!/usr/bin/env python3
"""Extract explicit public exports without importing runtime dependencies."""
import ast
import argparse
import hashlib
import json
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PACKAGE = ROOT / "NDNSF-DistributedInference" / "ndnsf_distributed_inference"
PREFIX = "ndnsf_distributed_inference"


def module_path(module):
    if module != PREFIX and not module.startswith(PREFIX + "."):
        return None
    relative = module[len(PREFIX):].lstrip(".").replace(".", "/")
    base = PACKAGE / relative
    file = base.with_suffix(".py") if relative else None
    if file is not None and file.is_file():
        return file
    file = base / "__init__.py"
    return file if file.is_file() else None


def imported_module(module, path, node):
    if not node.level:
        return node.module or ""
    package = module if path.name == "__init__.py" else module.rsplit(".", 1)[0]
    parts = package.split(".")
    if node.level > 1:
        parts = parts[:-(node.level - 1)]
    return ".".join(parts + ([node.module] if node.module else []))


@lru_cache(maxsize=None)
def load_source(path):
    source = path.read_text()
    return source, ast.parse(source)


def describe(module, name, seen=()):
    key = (module, name)
    path = module_path(module)
    if key in seen or path is None:
        return {"resolution": "external-or-unresolved", "owner": module, "symbol": name}
    source, tree = load_source(path)
    # Search declarations before re-exports; do not execute module conditionals.
    for node in tree.body:
        if isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == name:
            result = {"resolution": "defined", "owner": module, "symbol": name,
                      "path": str(path.relative_to(ROOT)), "line": node.lineno,
                      "sourceSha256": hashlib.sha256(source.encode()).hexdigest(),
                      "kind": type(node).__name__}
            if isinstance(node, ast.ClassDef):
                result["bases"] = [ast.get_source_segment(source, base) for base in node.bases]
                result["methods"] = [
                    {"name": n.name, "line": n.lineno,
                     "arguments": [a.arg for a in n.args.posonlyargs + n.args.args],
                     "keywordOnly": [a.arg for a in n.args.kwonlyargs],
                     "vararg": n.args.vararg.arg if n.args.vararg else None,
                     "kwarg": n.args.kwarg.arg if n.args.kwarg else None,
                     "decorators": [ast.get_source_segment(source, d) for d in n.decorator_list],
                     "returns": ast.get_source_segment(source, n.returns) if n.returns else None}
                    for n in node.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
                    and (not n.name.startswith("_") or n.name in {"__init__", "__post_init__"})]
                result["annotatedFields"] = [
                    {"name": n.target.id,
                     "annotation": ast.get_source_segment(source, n.annotation),
                     "default": ast.get_source_segment(source, n.value) if n.value else None}
                    for n in node.body if isinstance(n, ast.AnnAssign)
                    and isinstance(n.target, ast.Name)]
            return result
    for node in tree.body:
        if isinstance(node, ast.ImportFrom):
            for alias in node.names:
                if (alias.asname or alias.name) == name:
                    return describe(imported_module(module, path, node), alias.name, seen + (key,))
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            if any(isinstance(t, ast.Name) and t.id == name for t in targets):
                return {"resolution": "assignment", "owner": module, "symbol": name,
                        "path": str(path.relative_to(ROOT)), "line": node.lineno,
                        "sourceSha256": hashlib.sha256(source.encode()).hexdigest()}
    return {"resolution": "unresolved-static", "owner": module, "symbol": name,
            "path": str(path.relative_to(ROOT))}


def explicit_exports(module):
    path = module_path(module)
    tree = ast.parse(path.read_text())
    exports = None
    imports = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "__all__"
                                               for t in node.targets):
            exports = ast.literal_eval(node.value)
        if isinstance(node, ast.ImportFrom):
            for alias in node.names:
                imports[alias.asname or alias.name] = (imported_module(module, path, node), alias.name)
    if exports is None or not all(isinstance(x, str) for x in exports):
        raise ValueError("explicit export list required: " + module)
    return [(name, *imports.get(name, (module, name))) for name in exports]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--namespace", choices=["api", "sdk", "root"])
    options = parser.parse_args()
    entries = []
    for namespace in [PREFIX + ".api", PREFIX + ".sdk"]:
        for export, owner, name in explicit_exports(namespace):
            if options.namespace and not namespace.endswith("." + options.namespace):
                continue
            entries.append({"namespace": namespace, "export": export,
                            "definition": describe(owner, name), "migration": "UNREVIEWED"})
    manifest = json.loads((PACKAGE / "compatibility/manifest.json").read_text())
    for entry in manifest["entries"]:
        if entry.get("surfaceKind") == "python-export" and options.namespace in {None, "root"}:
            name = entry["surfaceId"].split(":", 1)[1]
            owner = "py_repoclient" if name == "GenericRepoClient" else entry["currentOwner"]
            target = "RepoClient" if name == "GenericRepoClient" else name
            entries.append({"namespace": PREFIX, "export": name,
                            "definition": describe(owner, target), "migration": "UNREVIEWED"})
    entries.sort(key=lambda e: (e["namespace"], e["export"]))
    print(json.dumps({"schema": "spec182-public-export-inventory-v1",
                      "scope": "api/sdk explicit __all__ and root compatibility manifest",
                      "limits": ["app_sdk dynamic wildcard exports excluded",
                                 "inherited methods and instance assignments not enumerated",
                                 "migration dispositions require semantic review"],
                      "entries": entries}, ensure_ascii=False, separators=(",", ":")))


if __name__ == "__main__":
    main()
