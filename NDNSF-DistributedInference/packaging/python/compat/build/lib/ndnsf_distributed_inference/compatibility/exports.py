"""Manifest-driven compatibility resolution for the aggregate wheel."""

from importlib import import_module
import json
from pathlib import Path
import warnings

def _targets():
    payload = json.loads(Path(__file__).with_name("manifest.json").read_text())
    result = {}
    for entry in payload.get("entries", []):
        if entry.get("surfaceKind") == "python-export":
            name = str(entry["surfaceId"]).split(":", 1)[1]
            result[name] = "py_repoclient" if name == "GenericRepoClient" else str(entry["currentOwner"])
    return result

_TARGETS = _targets()

def legacy_names(): return tuple(sorted(_TARGETS))

def resolve_legacy_export(name):
    owner = _TARGETS.get(name)
    if owner is None: raise AttributeError(name)
    warnings.warn(f"root export {name} is a compatibility adapter", DeprecationWarning, stacklevel=2)
    module = import_module(owner)
    return getattr(module, "RepoClient" if name == "GenericRepoClient" else name)
