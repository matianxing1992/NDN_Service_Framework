"""Fail-closed build-boundary checks for the Spec 170 application SIF."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shlex
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


class Spec170BuildBoundaryError(ValueError):
    pass


def _fail(code: str, detail: object = "") -> None:
    suffix = f":{detail}" if detail != "" else ""
    raise Spec170BuildBoundaryError(code + suffix)


@dataclass(frozen=True)
class _Stage:
    name: str
    bootstrap: str
    source: str
    sections: dict[str, str]


def _parse_stages(text: str) -> list[_Stage]:
    blocks: list[list[str]] = []
    current: list[str] = []
    for raw in text.splitlines():
        if re.match(r"^\s*Bootstrap\s*:", raw, re.IGNORECASE) and current:
            blocks.append(current)
            current = []
        current.append(raw)
    if current:
        blocks.append(current)

    stages: list[_Stage] = []
    for block in blocks:
        headers: dict[str, str] = {}
        sections: dict[str, list[str]] = {}
        active = ""
        for raw in block:
            header = re.match(r"^\s*(Bootstrap|From|Stage)\s*:\s*(.*?)\s*$", raw,
                              re.IGNORECASE)
            if header and not active:
                headers[header.group(1).lower()] = header.group(2)
                continue
            section = re.match(r"^\s*%(\S+(?:\s+from\s+\S+)?)\s*$", raw,
                               re.IGNORECASE)
            if section:
                active = section.group(1).lower()
                sections.setdefault(active, [])
                continue
            if active:
                sections[active].append(raw)
        stages.append(_Stage(
            name=headers.get("stage", ""),
            bootstrap=headers.get("bootstrap", "").lower(),
            source=headers.get("from", ""),
            sections={key: "\n".join(value) for key, value in sections.items()},
        ))
    return stages


def _file_entries(section: str) -> Iterable[tuple[str, str]]:
    for raw in section.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            tokens = shlex.split(line)
        except ValueError:
            _fail("WRONG_BUILD_BOUNDARY_FILES_SYNTAX", line)
        if tokens:
            yield tokens[0], tokens[1] if len(tokens) > 1 else ""


def _contains_compiled_payload(path: Path) -> bool:
    if path.is_file():
        name = path.name
        return name.endswith((".so", ".a", ".o")) or name in {
            "di-native-provider", "App_ServiceController"
        }
    if path.is_dir():
        return any(
            candidate.is_file() and
            (candidate.name.endswith((".so", ".a", ".o")) or
             candidate.name in {"di-native-provider", "App_ServiceController"})
            for candidate in path.rglob("*")
        )
    return False


def _looks_like_compiled_host_input(source: str, destination: str,
                                    candidate: Path) -> bool:
    """Reject compiled runtime inputs even when an old source path is gone."""
    source_name = Path(source).name
    destination_name = Path(destination).name if destination else ""
    compiled_name = re.compile(r"(?:\.so(?:\.|$)|\.(?:a|o)$)")
    runtime_binary_names = {"di-native-provider", "App_ServiceController"}

    if source_name in runtime_binary_names or destination_name in runtime_binary_names:
        return True
    if compiled_name.search(source_name) or compiled_name.search(destination_name):
        return True

    normalized_destination = destination.replace("\\", "/")
    if (source_name == "ndnsf" and
            "/site-packages/ndnsf" in normalized_destination):
        return True
    return _contains_compiled_payload(candidate)


def validate_definition(path: Path | str) -> dict[str, object]:
    definition = Path(path).resolve()
    try:
        text = definition.read_text(encoding="utf-8")
    except OSError as exc:
        _fail("WRONG_BUILD_BOUNDARY_DEFINITION_READ_FAILED", exc)

    stages = _parse_stages(text)
    # Scan plain %files before validating the stage shape.  This gives old
    # single-stage definitions (including r13) the precise provenance failure
    # instead of hiding it behind a generic multi-stage error.
    host_binary_inputs: list[str] = []
    for stage in stages:
        for key, body in stage.sections.items():
            if not key.startswith("files") or " from " in key:
                continue
            for source, destination in _file_entries(body):
                candidate = Path(source)
                if not candidate.is_absolute():
                    candidate = definition.parent / candidate
                if _looks_like_compiled_host_input(
                        source, destination, candidate):
                    host_binary_inputs.append(
                        f"{candidate.resolve()}->{destination or '<implicit>'}")
    if host_binary_inputs:
        _fail("WRONG_BUILD_BOUNDARY_HOST_BINARY_INPUT", host_binary_inputs[0])

    by_name = {stage.name: stage for stage in stages}
    if len(stages) != 2 or set(by_name) != {"builder", "final"}:
        _fail("WRONG_BUILD_BOUNDARY_MULTISTAGE_REQUIRED")
    builder = by_name["builder"]
    final = by_name["final"]
    if builder.bootstrap != "localimage" or final.bootstrap != "localimage":
        _fail("WRONG_BUILD_BOUNDARY_LOCALIMAGE_REQUIRED")
    if not builder.source or builder.source != final.source:
        _fail("WRONG_BUILD_BOUNDARY_BASE_IDENTITY_MISMATCH")

    builder_post = builder.sections.get("post", "")
    required_builder_markers = {
        "NDNSF_CONTAINER_BUILD=1": "CONTAINER_BUILD_MARKER_MISSING",
        "di-native-provider": "PROVIDER_BUILD_MISSING",
        "pythonWrapper": "PYTHON_EXTENSION_SOURCE_MISSING",
        "pip": "PYTHON_EXTENSION_BUILD_MISSING",
        "container-configure-closure.json": "CONFIGURE_CLOSURE_MISSING",
        "container-native-build.json": "BUILD_MANIFEST_MISSING",
    }
    for marker, code in required_builder_markers.items():
        if marker not in builder_post:
            _fail(f"WRONG_BUILD_BOUNDARY_{code}")
    if re.search(r"(?:^|[\s'\"])/home/", builder_post):
        _fail("WRONG_BUILD_BOUNDARY_HOST_HOME_REFERENCE")

    transfer = final.sections.get("files from builder", "")
    required_transfers = {
        "/opt/ndnsf-stage/bin/di-native-provider": "PROVIDER_TRANSFER_MISSING",
        "/opt/ndnsf-stage/lib/libndn-service-framework": "FRAMEWORK_TRANSFER_MISSING",
        "/opt/ndnsf-stage/python": "PYTHON_TRANSFER_MISSING",
        "/opt/ndnsf-stage/manifest/container-native-build.json":
            "BUILD_MANIFEST_TRANSFER_MISSING",
    }
    for marker, code in required_transfers.items():
        if marker not in transfer:
            _fail(f"WRONG_BUILD_BOUNDARY_{code}")

    # The experiment-only fault provider is optional for a normal runtime, but
    # if a candidate builds it, it must cross the same sealed builder boundary
    # and be installed from that exact stage.  This prevents a test job from
    # silently falling back to a host or stale base executable.
    if "di-native-fault-provider" in builder_post:
        if "/opt/ndnsf-stage/bin/di-native-fault-provider" not in transfer:
            _fail("WRONG_BUILD_BOUNDARY_FAULT_PROVIDER_TRANSFER_MISSING")

    # A qualified dependency base may contain an older application build.
    # Require the final stage to remove that build before installing the one
    # builder-stage output set, and to prove that only one extension remains.
    final_post = final.sections.get("post", "")
    required_replacement_markers = {
        "NDNSF_REPLACE_STALE_NATIVE=1": "STALE_REPLACEMENT_MARKER_MISSING",
        "rm -f /opt/ndnsf-di/current/bin/di-native-provider":
            "STALE_PROVIDER_REMOVAL_MISSING",
        "rm -f /opt/ndnsf-di/current/lib/libndn-service-framework.so":
            "STALE_FRAMEWORK_REMOVAL_MISSING",
        "rm -f /opt/venv/lib/python3.10/site-packages/ndnsf/_ndnsf":
            "STALE_EXTENSION_REMOVAL_MISSING",
        "find /opt/venv/lib/python3.10/site-packages/ndnsf":
            "ACTIVE_EXTENSION_CENSUS_MISSING",
        "_ndnsf*.so": "ACTIVE_EXTENSION_PATTERN_MISSING",
        "sha256sum": "FINAL_HASH_CHECK_MISSING",
        "container-native-build.json": "FINAL_MANIFEST_CHECK_MISSING",
    }
    for marker, code in required_replacement_markers.items():
        if marker not in final_post:
            _fail(f"WRONG_BUILD_BOUNDARY_{code}")
    if "di-native-fault-provider" in builder_post:
        if "rm -f /opt/ndnsf-di/current/bin/di-native-fault-provider" not in final_post:
            _fail("WRONG_BUILD_BOUNDARY_FAULT_PROVIDER_STALE_REMOVAL_MISSING")
        if "install -m 0755 /opt/ndnsf-candidate/bin/di-native-fault-provider" not in final_post:
            _fail("WRONG_BUILD_BOUNDARY_FAULT_PROVIDER_INSTALL_MISSING")

    final_labels = final.sections.get("labels", "")
    if "org.ndnsf.di.build-boundary container-runtime-in-sif" not in final_labels:
        _fail("WRONG_BUILD_BOUNDARY_LABEL_MISSING")
    if ("org.ndnsf.di.native-build-manifest "
            "/opt/ndnsf-di/current/manifest/container-native-build.json"
            not in final_labels):
        _fail("WRONG_BUILD_BOUNDARY_MANIFEST_LABEL_MISSING")

    return {
        "schemaVersion": "spec170-sif-build-boundary-v2",
        "status": "PASS",
        "definition": str(definition),
        "definitionSha256": "sha256:" + hashlib.sha256(text.encode()).hexdigest(),
        "builderStage": "builder",
        "finalStage": "final",
        "baseImage": builder.source,
        "hostBinaryInputs": [],
        "containerNativeBuild": True,
        "staleBaseArtifactsReplaced": True,
        "manifestPath": "/opt/ndnsf-di/current/manifest/container-native-build.json",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--definition", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = validate_definition(args.definition)
    except Spec170BuildBoundaryError as exc:
        print(str(exc), file=sys.stderr)
        return 4
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
