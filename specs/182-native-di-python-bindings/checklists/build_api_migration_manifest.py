#!/usr/bin/env python3
"""Build the Spec182 public-surface migration manifest without importing NDNSF.

The inventory is intentionally source based.  It records every explicit API,
SDK and root compatibility export plus names contributed by app_sdk's dynamic
``__all__``.  It does not claim that a planned native owner is implemented.
"""

from __future__ import annotations

import ast
import hashlib
import json
import re
import subprocess
from functools import lru_cache
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
PACKAGE = ROOT / "NDNSF-DistributedInference" / "ndnsf_distributed_inference"
PREFIX = "ndnsf_distributed_inference"
INVENTORY = ROOT / "specs/182-native-di-python-bindings/contracts/public-export-inventory.json"
OUTPUT = ROOT / "specs/182-native-di-python-bindings/contracts/compatibility-manifest.json"


def module_path(module: str) -> Path | None:
    if module != PREFIX and not module.startswith(PREFIX + "."):
        return None
    relative = module[len(PREFIX):].lstrip(".").replace(".", "/")
    candidate = PACKAGE / relative
    file = candidate.with_suffix(".py") if relative else None
    if file is not None and file.is_file():
        return file
    init = candidate / "__init__.py"
    return init if init.is_file() else None


def imported_module(module: str, path: Path, node: ast.ImportFrom) -> str:
    if not node.level:
        return node.module or ""
    package = module if path.name == "__init__.py" else module.rsplit(".", 1)[0]
    parts = package.split(".")
    if node.level > 1:
        parts = parts[:-(node.level - 1)]
    return ".".join(parts + ([node.module] if node.module else []))


def parse(path: Path) -> tuple[str, ast.Module]:
    source = path.read_text(encoding="utf-8")
    return source, ast.parse(source, filename=str(path))


def _parameter_descriptors(source: str, arguments: ast.arguments) -> list[dict[str, Any]]:
    """Return source-level parameter metadata without importing the package."""
    positional = arguments.posonlyargs + arguments.args
    positional_defaults = [None] * (len(positional) - len(arguments.defaults))
    positional_defaults.extend(arguments.defaults)
    result: list[dict[str, Any]] = []
    for index, argument in enumerate(positional):
        result.append({
            "name": argument.arg,
            "kind": "positional-only" if index < len(arguments.posonlyargs)
            else "positional-or-keyword",
            "annotation": ast.get_source_segment(source, argument.annotation)
            if argument.annotation else None,
            "default": ast.get_source_segment(source, positional_defaults[index])
            if positional_defaults[index] else None,
        })
    if arguments.vararg:
        result.append({
            "name": arguments.vararg.arg,
            "kind": "var-positional",
            "annotation": ast.get_source_segment(source, arguments.vararg.annotation)
            if arguments.vararg.annotation else None,
            "default": None,
        })
    for index, argument in enumerate(arguments.kwonlyargs):
        default = arguments.kw_defaults[index]
        result.append({
            "name": argument.arg,
            "kind": "keyword-only",
            "annotation": ast.get_source_segment(source, argument.annotation)
            if argument.annotation else None,
            "default": ast.get_source_segment(source, default) if default else None,
        })
    if arguments.kwarg:
        result.append({
            "name": arguments.kwarg.arg,
            "kind": "var-keyword",
            "annotation": ast.get_source_segment(source, arguments.kwarg.annotation)
            if arguments.kwarg.annotation else None,
            "default": None,
        })
    return result


def literal_all(module: str) -> list[str]:
    path = module_path(module)
    if path is None:
        return []
    _, tree = parse(path)
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == "__all__"
                for target in node.targets):
            try:
                value = ast.literal_eval(node.value)
            except (ValueError, TypeError):
                return []
            return [item for item in value if isinstance(item, str)]
    return []


def resolve_definition(module: str, name: str, seen: tuple[tuple[str, str], ...] = ()) -> dict[str, Any]:
    key = (module, name)
    path = module_path(module)
    if path is None or key in seen:
        return {"resolution": "external-or-unresolved", "owner": module, "symbol": name}
    source, tree = parse(path)
    for node in tree.body:
        if isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == name:
            result: dict[str, Any] = {
                "resolution": "defined",
                "owner": module,
                "symbol": name,
                "path": str(path.relative_to(ROOT)),
                "line": node.lineno,
                "sourceSha256": hashlib.sha256(source.encode("utf-8")).hexdigest(),
                "kind": type(node).__name__,
            }
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                result["parameters"] = _parameter_descriptors(source, node.args)
                result["returns"] = ast.get_source_segment(source, node.returns) \
                    if node.returns else None
            if isinstance(node, ast.ClassDef):
                result["bases"] = [ast.get_source_segment(source, base) for base in node.bases]
                result["methods"] = [
                    {
                        "name": child.name,
                        "line": child.lineno,
                        "arguments": [arg.arg for arg in child.args.posonlyargs + child.args.args],
                        "keywordOnly": [arg.arg for arg in child.args.kwonlyargs],
                        "parameters": _parameter_descriptors(source, child.args),
                        "vararg": child.args.vararg.arg if child.args.vararg else None,
                        "kwarg": child.args.kwarg.arg if child.args.kwarg else None,
                        "decorators": [ast.get_source_segment(source, dec) for dec in child.decorator_list],
                        "returns": ast.get_source_segment(source, child.returns) if child.returns else None,
                    }
                    for child in node.body
                    if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef))
                    and (not child.name.startswith("_") or child.name in {"__init__", "__post_init__"})
                ]
                result["annotatedFields"] = [
                    {
                        "name": child.target.id,
                        "annotation": ast.get_source_segment(source, child.annotation),
                        "default": ast.get_source_segment(source, child.value) if child.value else None,
                    }
                    for child in node.body
                    if isinstance(child, ast.AnnAssign) and isinstance(child.target, ast.Name)
                ]
            return result
    for node in tree.body:
        if isinstance(node, ast.ImportFrom):
            for alias in node.names:
                exported = alias.asname or alias.name
                if exported == name:
                    return resolve_definition(
                        imported_module(module, path, node), alias.name, seen + (key,))
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            if any(isinstance(target, ast.Name) and target.id == name for target in targets):
                return {
                    "resolution": "assignment",
                    "owner": module,
                    "symbol": name,
                    "path": str(path.relative_to(ROOT)),
                    "line": node.lineno,
                    "sourceSha256": hashlib.sha256(source.encode("utf-8")).hexdigest(),
                    "expression": ast.get_source_segment(source, node.value),
                }
    return {"resolution": "unresolved-static", "owner": module, "symbol": name,
            "path": str(path.relative_to(ROOT))}


def app_sdk_exports() -> dict[str, tuple[str, str]]:
    """Resolve names contributed by app_sdk/__init__.py's wildcard imports."""
    module = PREFIX + ".app_sdk"
    path = module_path(module)
    assert path is not None
    _, tree = parse(path)
    result: dict[str, tuple[str, str]] = {}
    for node in tree.body:
        if not isinstance(node, ast.ImportFrom):
            continue
        owner_module = imported_module(module, path, node)
        if any(alias.name == "*" for alias in node.names):
            for name in literal_all(owner_module):
                result.setdefault(name, (owner_module, name))
            continue
        for alias in node.names:
            result.setdefault(alias.asname or alias.name, (owner_module, alias.name))
    return result


@lru_cache(maxsize=1)
def source_symbol_index() -> dict[str, tuple[str, ...]]:
    """Tokenize tracked source once so the 300-entry inventory stays bounded."""
    try:
        paths = subprocess.check_output(
            ["git", "ls-files", "--",
             "NDNSF-DistributedInference/**/*.py",
             "NDNSF-DistributedInference/**/*.cpp",
             "NDNSF-DistributedInference/**/*.hpp",
             "NDNSF-DistributedInference/**/*.h",
             "pythonWrapper/**/*.py", "pythonWrapper/**/*.cpp",
             "pythonWrapper/**/*.hpp", "pythonWrapper/**/*.h",
             "tests/**/*.py", "tests/**/*.cpp", "tests/**/*.hpp",
             "tests/**/*.h", "examples/**/*.py", "examples/**/*.cpp",
             "examples/**/*.hpp", "examples/**/*.h",
             "Experiments/**/*.py", "Experiments/**/*.cpp",
             "Experiments/**/*.hpp", "Experiments/**/*.h"],
            cwd=ROOT, text=True).splitlines()
    except (OSError, subprocess.CalledProcessError):
        paths = []
    result: dict[str, list[str]] = {}
    for raw in paths:
        try:
            text = (ROOT / raw).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for symbol in set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", text)):
            result.setdefault(symbol, []).append(raw)
    return {symbol: tuple(paths) for symbol, paths in result.items()}


def source_callers(symbol: str, definition_path: str | None) -> list[str]:
    """Find repository source files mentioning a public symbol.

    This is a conservative caller inventory: it reports token mentions and
    leaves semantic call classification to the owner task.
    """
    return [path for path in source_symbol_index().get(symbol, ())
            if path != definition_path]


NATIVE_TYPES = {
    "InferenceApplication": ("NativeInferenceClient / NativeConversationCoordinator", "T012-B"),
    "InferenceClient": ("NativeInferenceClient", "T012-B"),
    "InferenceProvider": ("NativeInferenceProvider", "T012-B"),
    "InferenceRequestHandle": ("NativeInferenceHandle", "T010/T012"),
    "InferenceResult": ("NativeInferenceResult", "T010/T012"),
    "ModelRef": ("NativeModelRef", "T003/T012"),
    "GenerationInput": ("NativeApplicationInput", "T008/T012"),
    "GenerationConfig": ("Native generation contract", "T007/T012"),
    "ArtifactReference": ("NativeArtifactBinding", "T008/T012"),
    "DeploymentDefinition": ("Native deployment definition port", "T010/T012"),
    "ProviderDeploymentOffer": ("NativeProviderPlanningView", "T008/T012"),
    "ProviderDeploymentOffers": ("NativeProviderPlanningView", "T008/T012"),
    "PlacementPlanCoreV3": ("NativePlacementPlanCore", "T004"),
    "PlacementProposalV3": ("NativePlacementProposal", "T003/T004"),
    "PlanSealerV3": ("NativePlanSealer", "T004"),
    "ModelPlacementStrategy": ("NativePlacementStrategy", "T003"),
    "PartitionPlanner": ("NativeModelSplitStrategy", "T003"),
    "ExecutionDisposition": ("Native request terminal state", "T010"),
    "ObserverRegistry": ("NativeInferenceHandle observer boundary", "T010/T012"),
}


# These are deliberately conservative.  A PARTIAL_EXISTING_TYPE entry means
# that a native type or facade already exists, while field/method/error parity
# is still an O-004/T012 obligation.  Entries absent from this table remain
# UNREVIEWED instead of receiving a guessed native owner.
API_MAPPING_OVERRIDES: dict[str, dict[str, str]] = {
    "ArtifactReference": {
        "mappingStatus": "PARTIAL_EXISTING_TYPE",
        "nativeOwner": "NativeArtifactBinding",
        "mappingReference": "contracts/runtime-boundaries.md#cd-013-preparation-and-offer-admission",
        "ownerTask": "T008/T012",
    },
    "GenerationInput": {
        "mappingStatus": "PARTIAL_EXISTING_TYPE",
        "nativeOwner": "NativeApplicationInput",
        "mappingReference": "contracts/value-contracts.md",
        "ownerTask": "T008/T012",
    },
    "GenerationConfig": {
        "mappingStatus": "PLANNED_TYPE",
        "nativeOwner": "NativeGenerationOptions (planned)",
        "mappingReference": "contracts/native-generation-design.md",
        "ownerTask": "T007/T012",
    },
    "InferenceClient": {
        "mappingStatus": "PARTIAL_EXISTING_TYPE",
        "nativeOwner": "NativeInferenceClient",
        "mappingReference": "contracts/code-design.md#cd-001-public-api",
        "ownerTask": "T010/T012",
    },
    "InferenceProvider": {
        "mappingStatus": "PARTIAL_EXISTING_TYPE",
        "nativeOwner": "NativeInferenceProvider",
        "mappingReference": "contracts/runtime-boundaries.md#cd-014-provider-host-and-binding",
        "ownerTask": "T009/T012",
    },
    "InferenceRequestHandle": {
        "mappingStatus": "PARTIAL_EXISTING_TYPE",
        "nativeOwner": "NativeInferenceHandle",
        "mappingReference": "contracts/code-design.md#cd-001-public-api",
        "ownerTask": "T010/T012",
    },
    "InferenceResult": {
        "mappingStatus": "PARTIAL_EXISTING_TYPE",
        "nativeOwner": "NativeInferenceResult",
        "mappingReference": "contracts/value-contracts.md",
        "ownerTask": "T010/T012",
    },
    "InferenceOptions": {
        "mappingStatus": "PARTIAL_EXISTING_TYPE",
        "nativeOwner": "NativeRequestOptions",
        "mappingReference": "contracts/code-design.md#cd-001-public-api",
        "ownerTask": "T010/T012",
    },
    "ModelRef": {
        "mappingStatus": "PARTIAL_EXISTING_TYPE",
        "nativeOwner": "NativeModelRef",
        "mappingReference": "contracts/value-contracts.md",
        "ownerTask": "T003/T012",
    },
}


def disposition(namespace: str, export: str, owner: str) -> tuple[str, str, str, bool]:
    if namespace == PREFIX:
        return ("RETAINED_COMPATIBILITY", "T013-B", "retained until native caller migration", True)
    if export in NATIVE_TYPES or namespace.endswith(".api"):
        native_owner, task = NATIVE_TYPES.get(
            export, ("native compatibility owner to be finalized", "T012-B"))
        return ("NATIVE_REQUIRED", task, native_owner, True)
    if namespace.endswith(".sdk"):
        if any(part in owner for part in ("planner", "core.ports", "sdk.placement")):
            return ("BINDING_ONLY", "T012-B", "native strategy/value boundary", True)
        return ("OFFLINE_REFERENCE", "T013-B", "Python policy/reference owner retained", True)
    if namespace.endswith(".app_sdk"):
        if export in {"APPClient", "APPProvider", "InferenceClient", "InferenceProvider", "InferenceApplication"}:
            return ("NATIVE_REQUIRED", "T012-B", "native facade owner", True)
        return ("OFFLINE_REFERENCE", "T013-B", "Python compatibility owner retained", True)
    return ("OFFLINE_REFERENCE", "T013-B", owner, True)


def mapping_metadata(namespace: str, export: str) -> dict[str, str]:
    if namespace.endswith(".api"):
        return API_MAPPING_OVERRIDES.get(export, {
            "mappingStatus": "UNREVIEWED",
            "nativeOwner": "UNRESOLVED_O004",
            "mappingReference": "contracts/public-api-migration-review.md",
            "ownerTask": "T001-C/O-004",
        })
    if namespace.endswith(".app_sdk"):
        return {
            "mappingStatus": "DYNAMIC_OR_COMPATIBILITY_REVIEW",
            "mappingReference": "contracts/public-api-migration-review.md",
        }
    return {"mappingStatus": "INVENTORY_ONLY"}


def main() -> None:
    inventory = json.loads(INVENTORY.read_text(encoding="utf-8"))
    entries: dict[tuple[str, str], dict[str, Any]] = {}
    for item in inventory["entries"]:
        entries[(item["namespace"], item["export"])] = item
    explicit_count = len(entries)

    for export, (owner, symbol) in app_sdk_exports().items():
        key = (PREFIX + ".app_sdk", export)
        if key not in entries:
            entries[key] = {
                "namespace": key[0],
                "export": export,
                "definition": resolve_definition(owner, symbol),
                "migration": "UNREVIEWED",
            }

    manifest_entries: list[dict[str, Any]] = []
    for (namespace, export), item in sorted(entries.items()):
        definition = item.get("definition", {})
        # The checked-in export inventory predates this manifest and does not
        # retain assignment expressions or full parameter annotations.  For
        # package-owned modules, refresh the definition from the same AST
        # source so aliases and signatures cannot silently disappear.
        if namespace.startswith(PREFIX + "."):
            refreshed = resolve_definition(namespace, export)
            if refreshed.get("resolution") not in {"unresolved-static", "external-or-unresolved"}:
                definition = refreshed
        owner = str(definition.get("owner", namespace))
        path = definition.get("path")
        disp, task, native_owner, default_reachable = disposition(namespace, export, owner)
        mapping = mapping_metadata(namespace, export)
        task = mapping.get("ownerTask", task)
        native_owner = mapping.get("nativeOwner", native_owner)
        callers = source_callers(export, path)
        method_names = [method["name"] for method in definition.get("methods", [])]
        field_names = [field["name"] for field in definition.get("annotatedFields", [])]
        manifest_entries.append({
            "surfaceId": f"{namespace}:{export}",
            "surfaceKind": "python-export",
            "namespace": namespace,
            "export": export,
            "source": definition,
            "callers": callers,
            "externalUseStatus": "repository_callers_found" if callers else "external_use_unknown",
            "disposition": disp,
            "nativeOwner": native_owner,
            "ownerTask": task,
            "defaultReachable": default_reachable,
            "parameterContract": {
                "publicMethods": method_names,
                "annotatedFields": field_names,
                "methodSignatures": [
                    {
                        "name": method["name"],
                        "parameters": method.get("parameters", []),
                        "returns": method.get("returns"),
                    }
                    for method in definition.get("methods", [])
                ],
                "nestedTypes": sorted(set(re.findall(
                    r"[A-Z][A-Za-z0-9_]+", " ".join(
                        str(field.get("annotation", ""))
                        for field in definition.get("annotatedFields", []))))),
            },
            "mappingStatus": mapping["mappingStatus"],
            "mappingReference": mapping.get("mappingReference"),
            "verificationSelectors": sorted({
                path for path in callers
                if path.startswith("tests/") or path.startswith("examples/")
            }),
            "status": "RETAINED_UNTIL_MIGRATION" if disp == "RETAINED_COMPATIBILITY"
            else "PLANNED_NATIVE" if disp == "NATIVE_REQUIRED" else "RETAINED_OR_OFFLINE",
            "removalEligible": False,
            "removalCondition": "owner task acceptance plus two zero-caller snapshots and rollback evidence",
            "rollbackRelease": "last-verified-pre-spec182-release",
        })

    payload = {
        "schema": "spec182-compatibility-manifest-v1",
        "sourceInventory": "contracts/public-export-inventory.json",
        "sourceCommit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "coverage": {
            "explicitApiSdkRoot": explicit_count,
            "dynamicAppSdkExports": sum(
                item["namespace"] == PREFIX + ".app_sdk" for item in manifest_entries),
            "total": len(manifest_entries),
            "semanticCallerScan": "token-mention inventory; owner task performs call classification",
        },
        "entries": manifest_entries,
    }
    OUTPUT.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"path": str(OUTPUT.relative_to(ROOT)), "entries": len(manifest_entries),
                      "dynamicAppSdk": payload["coverage"]["dynamicAppSdkExports"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
