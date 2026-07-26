"""Manifest-driven compatibility resolution with observable usage signals."""

from __future__ import annotations

from importlib import import_module
import json
import logging
from pathlib import Path
import warnings
from typing import Any


_LOG = logging.getLogger("ndnsf.di.compatibility")


def _targets() -> dict[str, str]:
    manifest = Path(__file__).with_name("manifest.json")
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    result: dict[str, str] = {}
    for entry in payload.get("entries", []):
        if entry.get("surfaceKind") != "python-export":
            continue
        name = str(entry["surfaceId"]).split(":", 1)[1]
        owner = str(entry["currentOwner"])
        result[name] = "py_repoclient" if name == "GenericRepoClient" else owner
    return result


_TARGETS = _targets()


def legacy_names() -> tuple[str, ...]:
    return tuple(sorted(_TARGETS))


def resolve_legacy_export(name: str) -> Any:
    owner = _TARGETS.get(name)
    if owner is None:
        raise AttributeError(name)
    warnings.warn(
        f"root export {name} is a Spec 111 compatibility adapter; import its canonical owner",
        DeprecationWarning,
        stacklevel=2,
    )
    _LOG.info("NDNSF_DI_COMPAT_EXPORT name=%s owner=%s", name, owner)
    try:
        module = import_module(owner)
        return getattr(module, "RepoClient" if name == "GenericRepoClient" else name)
    except ImportError as error:
        if owner.endswith(".onnx_executor"):
            if name == "OnnxExecutionResult":
                return type("OnnxExecutionResult", (), {})

            def missing(*args: Any, **kwargs: Any) -> Any:
                del args, kwargs
                raise ImportError(
                    f"{name} requires optional ONNX runtime dependencies") from error
            missing.__name__ = name
            return missing
        if name == "GenericRepoClient":
            return None
        raise
