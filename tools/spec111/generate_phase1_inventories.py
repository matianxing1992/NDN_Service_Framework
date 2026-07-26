#!/usr/bin/env python3
"""Generate deterministic Spec 111 Phase-1 inventories without executing code."""

from __future__ import annotations

import ast
import hashlib
import json
import re
import subprocess
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Sequence, Set


REPO = Path(__file__).resolve().parents[2]
FEATURE = REPO / "specs/111-ndnsf-di-core-app-separation"
EVIDENCE = FEATURE / "evidence"
PACKAGE = REPO / "NDNSF-DistributedInference/ndnsf_distributed_inference"
ROLLBACK_RELEASE = "pre-spec111-worktree"


def run(*args: str) -> str:
    result = subprocess.run(
        list(args), cwd=str(REPO), text=True, capture_output=True, check=False
    )
    if result.returncode != 0:
        raise RuntimeError("command failed: {}\n{}".format(" ".join(args), result.stderr))
    return result.stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def tracked_files() -> List[Path]:
    values: List[Path] = []
    for text in run("git", "ls-files").splitlines():
        path = REPO / text
        if path.is_file():
            values.append(path)
    return values


def rel(path: Path) -> str:
    return path.relative_to(REPO).as_posix()


def parse_python(path: Path) -> ast.AST:
    return ast.parse(path.read_text(encoding="utf-8"), filename=str(path))


def public_symbols(path: Path) -> List[Dict[str, str]]:
    tree = parse_python(path)
    result: List[Dict[str, str]] = []
    for node in getattr(tree, "body", []):
        if isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            if not node.name.startswith("_"):
                result.append({"name": node.name, "kind": type(node).__name__})
        elif isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            for target in targets:
                if isinstance(target, ast.Name) and re.fullmatch(r"[A-Z][A-Z0-9_]+", target.id):
                    result.append({"name": target.id, "kind": "constant"})
    return sorted(result, key=lambda item: (item["name"], item["kind"]))


def package_owner(module: str) -> str:
    leaf = module.rsplit(".", 1)[-1]
    if leaf in {"app", "gui", "controller"}:
        return "app_sdk"
    if leaf in {"policy", "planner_registry", "split_planner", "splitter", "llm_stub_planner"}:
        return "planner"
    if leaf.startswith("onnx"):
        return "adapter-onnx"
    if leaf.startswith("qwen"):
        return "adapter-qwen"
    if leaf.startswith("llm") or leaf == "backends":
        return "adapter-llama"
    if leaf in {"operations", "release_gate", "runtime_v1_evidence"}:
        return "ops"
    if leaf == "__init__":
        return "compatibility"
    return "core"


def import_callers(files: Sequence[Path]) -> Mapping[str, List[str]]:
    callers: Dict[str, Set[str]] = {}
    prefix = "ndnsf_distributed_inference"
    for path in files:
        if path.suffix != ".py" or "third_party/" in rel(path):
            continue
        try:
            tree = parse_python(path)
        except (SyntaxError, UnicodeDecodeError):
            continue
        imported: Set[str] = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported.update(alias.name for alias in node.names if alias.name.startswith(prefix))
            elif isinstance(node, ast.ImportFrom):
                module = node.module or ""
                if module.startswith(prefix):
                    imported.add(module)
                elif node.level and rel(path).startswith("NDNSF-DistributedInference/ndnsf_distributed_inference/"):
                    imported.add(prefix + ("." + module if module else ""))
        for module in imported:
            callers.setdefault(module, set()).add(rel(path))
    return {key: sorted(value) for key, value in sorted(callers.items())}


def root_exports() -> List[str]:
    tree = parse_python(PACKAGE / "__init__.py")
    for node in getattr(tree, "body", []):
        if isinstance(node, ast.Assign):
            if any(isinstance(target, ast.Name) and target.id == "__all__" for target in node.targets):
                try:
                    value = ast.literal_eval(node.value)
                except (ValueError, TypeError):
                    return []
                return sorted(str(item) for item in value)
    return []


def historical_baseline() -> Dict[str, Any]:
    files: List[Dict[str, Any]] = []
    for pattern in ("107-*", "109-*", "110-*"):
        for directory in sorted((REPO / "specs").glob(pattern)):
            for path in sorted(directory.rglob("*")):
                if not path.is_file():
                    continue
                files.append({"path": rel(path), "sha256": sha256(path), "bytes": path.stat().st_size})
    return {
        "schemaVersion": 1,
        "sourceCommit": run("git", "rev-parse", "HEAD"),
        "scope": ["specs/107-*", "specs/109-*", "specs/110-*"],
        "files": files,
    }


def python_inventory(files: Sequence[Path]) -> Dict[str, Any]:
    callers = import_callers(files)
    modules: List[Dict[str, Any]] = []
    for path in sorted(PACKAGE.rglob("*.py")):
        if "__pycache__" in path.parts:
            continue
        module = "ndnsf_distributed_inference." + path.relative_to(PACKAGE).with_suffix("").as_posix().replace("/", ".")
        if module.endswith(".__init__"):
            module = module[: -len(".__init__")]
        related = set(callers.get(module, []))
        for imported, imported_callers in callers.items():
            if imported.startswith(module + "."):
                related.update(imported_callers)
        modules.append(
            {
                "module": module,
                "path": rel(path),
                "currentOwner": "mixed-package",
                "proposedCanonicalOwner": package_owner(module),
                "publicSymbols": public_symbols(path),
                "callers": sorted(related),
            }
        )
    return {
        "schemaVersion": 1,
        "sourceCommit": run("git", "rev-parse", "HEAD"),
        "rootExports": root_exports(),
        "modules": modules,
    }


def string_arguments(path: Path, calls: Set[str]) -> Set[str]:
    values: Set[str] = set()
    try:
        tree = parse_python(path)
    except (SyntaxError, UnicodeDecodeError):
        return values
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        name = ""
        if isinstance(node.func, ast.Attribute):
            name = node.func.attr
        elif isinstance(node.func, ast.Name):
            name = node.func.id
        if name not in calls:
            continue
        for arg in node.args:
            if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
                values.add(arg.value)
    return values


def command_config_inventory(files: Sequence[Path]) -> Dict[str, Any]:
    setup_text = (REPO / "NDNSF-DistributedInference/setup.py").read_text(encoding="utf-8")
    commands = sorted(re.findall(r'"([A-Za-z0-9_-]+)=([^" ]+)"', setup_text))
    options: Set[str] = set()
    environment: Set[str] = set()
    config_literals: Set[str] = set()
    scanned: List[str] = []
    for path in files:
        path_text = rel(path)
        if path.suffix != ".py":
            continue
        if not (
            path_text.startswith("NDNSF-DistributedInference/")
            or path_text.startswith("packaging/ndnsf-di-container/")
            or path_text.startswith("tools/ndnsf-di/")
        ):
            continue
        scanned.append(path_text)
        options.update(value for value in string_arguments(path, {"add_argument"}) if value.startswith("-"))
        try:
            tree = parse_python(path)
        except (SyntaxError, UnicodeDecodeError):
            continue
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
                if node.func.attr in {"get", "getenv"} and node.args:
                    arg = node.args[0]
                    if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
                        if isinstance(node.func.value, ast.Attribute) and node.func.value.attr == "environ":
                            environment.add(arg.value)
                        elif isinstance(node.func.value, ast.Name) and node.func.value.id == "os":
                            environment.add(arg.value)
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                if re.fullmatch(r"[a-z][A-Za-z0-9_]{2,}", node.value):
                    config_literals.add(node.value)
    return {
        "schemaVersion": 1,
        "sourceCommit": run("git", "rev-parse", "HEAD"),
        "consoleScripts": [{"name": name, "target": target} for name, target in commands],
        "cliOptions": sorted(options),
        "environmentKeys": sorted(environment),
        "configKeyCandidates": sorted(config_literals),
        "scannedFiles": sorted(scanned),
    }


def cpp_owner(path: str) -> str:
    name = Path(path).name.lower()
    if "qwen" in name:
        return "adapter-qwen"
    if "onnx" in name:
        return "adapter-onnx"
    return "core"


def cpp_inventory(files: Sequence[Path]) -> Dict[str, Any]:
    sources = []
    headers = []
    for path in files:
        path_text = rel(path)
        if "NDNSF-DistributedInference/cpp/" not in path_text:
            continue
        if path.suffix in {".cpp", ".cc", ".cxx"}:
            sources.append({"path": path_text, "proposedCanonicalOwner": cpp_owner(path_text)})
        elif path.suffix in {".hpp", ".h"}:
            headers.append({"path": path_text, "proposedCanonicalOwner": cpp_owner(path_text)})
    targets: List[Dict[str, Any]] = []
    for path in files:
        if path.name != "wscript":
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r"(?:target|name)\s*=\s*['\"]([^'\"]+)['\"]", text):
            target = match.group(1)
            if "di" in target.lower() or "inference" in target.lower():
                targets.append({"target": target, "wscript": rel(path)})
    include_callers: Dict[str, Set[str]] = {}
    include_pattern = re.compile(r'#\s*include\s*[<\"]([^>\"]*(?:ndnsf-di|DistributedInference)[^>\"]*)[>\"]')
    for path in files:
        if path.suffix not in {".cpp", ".cc", ".cxx", ".hpp", ".h"}:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for header in include_pattern.findall(text):
            include_callers.setdefault(header, set()).add(rel(path))
    return {
        "schemaVersion": 1,
        "sourceCommit": run("git", "rev-parse", "HEAD"),
        "sources": sorted(sources, key=lambda item: item["path"]),
        "headers": sorted(headers, key=lambda item: item["path"]),
        "targets": sorted(targets, key=lambda item: (item["target"], item["wscript"])),
        "includeCallers": {key: sorted(value) for key, value in sorted(include_callers.items())},
    }


def deployment_inventory(files: Sequence[Path]) -> Dict[str, Any]:
    categories = {
        "container": re.compile(r"docker|container|oci", re.I),
        "systemd": re.compile(r"systemd|\.service\b", re.I),
        "slurm": re.compile(r"slurm|sbatch|srun", re.I),
        "apptainer": re.compile(r"apptainer|singularity", re.I),
        "minindn": re.compile(r"minindn", re.I),
        "distributed-inference": re.compile(r"ndnsf[_-](?:distributed[_-])?inference|ndnsf-di", re.I),
    }
    entries: List[Dict[str, Any]] = []
    for path in files:
        path_text = rel(path)
        if path_text.startswith("Experiments/gRPC/") or path_text.startswith("third_party/"):
            continue
        if not (
            path_text.startswith("examples/")
            or path_text.startswith("Experiments/")
            or path_text.startswith("packaging/")
            or path_text.startswith("tools/")
            or path_text.endswith(".service")
        ):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        matched = sorted(name for name, pattern in categories.items() if pattern.search(path_text) or pattern.search(text))
        if matched and ("distributed-inference" in matched or set(matched) & {"container", "slurm", "apptainer", "minindn"}):
            entries.append({"path": path_text, "categories": matched})
    return {
        "schemaVersion": 1,
        "sourceCommit": run("git", "rev-parse", "HEAD"),
        "entries": sorted(entries, key=lambda item: item["path"]),
    }


def compatibility_manifest(
    python: Mapping[str, Any], command: Mapping[str, Any], cpp: Mapping[str, Any], deployment: Mapping[str, Any]
) -> Dict[str, Any]:
    entries_by_id: Dict[str, Dict[str, Any]] = {}

    def add(surface_id: str, kind: str, current: str, callers: Iterable[str], owner: str) -> None:
        existing = entries_by_id.get(surface_id)
        if existing is not None:
            if existing["surfaceKind"] != kind or existing["proposedCanonicalOwner"] != owner:
                raise ValueError("conflicting compatibility surface: " + surface_id)
            existing["callers"] = sorted(set(existing["callers"]) | set(callers))
            return
        entries_by_id[surface_id] = {
            "surfaceId": surface_id,
            "surfaceKind": kind,
            "currentOwner": current,
            "callers": sorted(set(callers)),
            "rollbackRelease": ROLLBACK_RELEASE,
            "proposedCanonicalOwner": owner,
            "status": "inventoried",
        }

    module_by_symbol: Dict[str, str] = {}
    callers_by_module: Dict[str, List[str]] = {}
    for module in python["modules"]:
        callers_by_module[module["module"]] = module["callers"]
        for symbol in module["publicSymbols"]:
            module_by_symbol.setdefault(symbol["name"], module["module"])
    for name in python["rootExports"]:
        module = module_by_symbol.get(name, "ndnsf_distributed_inference")
        add("python-export:" + name, "python-export", module, callers_by_module.get(module, []), package_owner(module))
    for item in command["consoleScripts"]:
        add("console-script:" + item["name"], "console-script", item["target"], [], "ops")
    for item in cpp["headers"]:
        add("cpp-header:" + item["path"], "cpp-header", item["path"], [], item["proposedCanonicalOwner"])
    for item in cpp["targets"]:
        add("cpp-target:" + item["target"], "cpp-target", item["wscript"], [], cpp_owner(item["target"]))
    for item in deployment["entries"]:
        owner = "experiments" if "minindn" in item["categories"] else "operations"
        add("deployment-caller:" + item["path"], "deployment-caller", item["path"], [item["path"]], owner)
    return {
        "schemaVersion": 1,
        "sourceCommit": run("git", "rev-parse", "HEAD"),
        "entries": sorted(entries_by_id.values(), key=lambda item: item["surfaceId"]),
    }


def main() -> None:
    files = tracked_files()
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    history = historical_baseline()
    python = python_inventory(files)
    command = command_config_inventory(files)
    cpp = cpp_inventory(files)
    deployment = deployment_inventory(files)

    write_json(EVIDENCE / "historical-evidence-baseline.json", history)
    write_json(EVIDENCE / "python-surface-inventory.json", python)
    write_json(EVIDENCE / "command-config-inventory.json", command)
    write_json(EVIDENCE / "cpp-surface-inventory.json", cpp)
    write_json(EVIDENCE / "deployment-caller-inventory.json", deployment)

    manifest = compatibility_manifest(python, command, cpp, deployment)
    manifest_path = PACKAGE / "compatibility/manifest.json"
    write_json(manifest_path, manifest)

    fixture = {
        "schemaVersion": 1,
        "baselinePath": rel(EVIDENCE / "historical-evidence-baseline.json"),
        "baselineSha256": sha256(EVIDENCE / "historical-evidence-baseline.json"),
        "frozenFileCount": len(history["files"]),
        "sourceCommit": history["sourceCommit"],
    }
    write_json(
        REPO / "tests/fixtures/ndnsf-di-core-app-separation/historical-evidence-baseline.json",
        fixture,
    )

    summary = {
        "historicalFiles": len(history["files"]),
        "pythonModules": len(python["modules"]),
        "pythonRootExports": len(python["rootExports"]),
        "consoleScripts": len(command["consoleScripts"]),
        "cppSources": len(cpp["sources"]),
        "cppHeaders": len(cpp["headers"]),
        "cppTargets": len(cpp["targets"]),
        "deploymentCallers": len(deployment["entries"]),
        "compatibilityEntries": len(manifest["entries"]),
    }
    print(json.dumps(summary, sort_keys=True))


if __name__ == "__main__":
    main()
