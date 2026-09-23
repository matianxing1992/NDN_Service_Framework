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


def _validate_installed_runtime(definition, text, builder, final):
    """Current complete SIF: an unchanged dependency base and one install tree.

    Historical APP definitions retain the legacy validation below. This is a
    structural preflight only; build/import/ABI verification is still required.
    """
    post = builder.sections.get("post", "")
    final_post = final.sections.get("post", "")
    required = (
        "NDNSF_RUNTIME_LAYOUT=installed-v1", "NDNSF_CONTAINER_BUILD=1",
        "NDNSF_SKIP_DEV_PIP_INSTALL=1", "NDNSF_WAF_INSTALL_PAYLOAD=1",
        "./waf install -j4 -v --destdir=/opt/ndnsf-waf-dest",
        "--with-tests", "--install-experiment-fixtures",
        "--libexecdir=/opt/ndnsf-di/current/libexec", "di-native-assembly-worker",
        "cp -a /opt/ndnsf-waf-dest/opt/ndnsf-di/current/. /opt/ndnsf-stage/waf-install/",
        "cp -a /opt/ndnsf-stage/waf-install/. /opt/ndnsf-stage/",
        "export NDNSF_NAC_ABE_PREFIX=/opt/ndn-base",
        "unset NDNSF_NDN_SVS_SOURCE_TREE NDNSF_NDN_SVS_BUILD_TREE",
        "NDNSF_LIBRARY_DIR=/opt/ndnsf-stage/lib",
        "--no-index --no-deps --no-build-isolation",
        "BASE_DEPENDENCY_IDENTITY_MISMATCH", "BASE_NATIVE_INPUT_IDENTITY_MISMATCH",
        "BASE_CONTAINS_NDNSF", "CONTAINER_PYTHON_HEADERS_MISSING",
        "EXTENSION_SOABI", "LOADED_LIBRARY_MISMATCH", "LINKED_LIBRARY_ORIGIN_MISMATCH",
        "NATIVE_DEPENDENCY_MISSING", "UNDECLARED_HOST_LIBRARY",
        "WAF_INSTALL_HASH_MISMATCH", "FINAL_WAF_INSTALL_RECEIPT_CHANGED",
        "FINAL_ARTIFACT_OR_CLOSURE_MISMATCH",
        "container-configure-closure.json", "container-native-build.json",
        "verify-native.py builder",
    )
    if any(marker not in post for marker in required):
        _fail("WRONG_BUILD_BOUNDARY_WAF_INSTALL_CONTRACT_MISSING")
    if list(_file_entries(final.sections.get("files from builder", ""))) != [
            ("/opt/ndnsf-stage", "/opt/ndnsf-candidate")]:
        _fail("WRONG_BUILD_BOUNDARY_WAF_INSTALL_TRANSFER_MISSING")
    for marker in (
            "BASE_CONTAINS_NDNSF", "BASE_RUNTIME_PACKAGE_CHANGED",
            "cp -a /opt/ndnsf-candidate/. /opt/ndnsf-di/current/",
            "cp -a /opt/ndnsf-di/current/python/. /opt/venv/lib/python3.10/site-packages/",
            "dependency-sdk.py verify", "verify-native.py final",
            "sha256sum", "extension_count", "repo_extension_count"):
        if marker not in final_post:
            _fail("WRONG_BUILD_BOUNDARY_WAF_INSTALL_FINAL_COPY_MISSING", marker)
    for value in re.findall(r"\bNDNSF_LIBRARY_DIR=([^\s]+)", post):
        if value.strip("\"'") != "/opt/ndnsf-stage/lib":
            _fail("WRONG_BUILD_BOUNDARY_PYTHON_STAGE_LIBRARY_CLOSURE_MISMATCH")
    forbidden = ("--ndn-svs-source-tree=", "--ndn-svs-build-tree=",
                 "install -m 0755 build/", "install -m 0644 build/",
                 "apt-get", "cp -a /opt/ndn-base/include/",
                 "install -m 0755 \"/opt/ndn-base/lib/",
                 "install -d /opt/ndnsf-app", "/usr/local/lib/libndnsd")
    if any(marker in post + final_post for marker in forbidden):
        _fail("WRONG_BUILD_BOUNDARY_DUPLICATE_INSTALL_OR_BASE_MUTATION")
    if re.search(r"(?:^|[\s'\"])/home/", post + final_post):
        _fail("WRONG_BUILD_BOUNDARY_HOST_HOME_REFERENCE")
    labels = final.sections.get("labels", "")
    for marker in (
            "org.ndnsf.di.runtime-layout installed-v1",
            "org.ndnsf.di.build-boundary container-runtime-in-sif",
            "org.ndnsf.di.native-build-manifest /opt/ndnsf-di/current/manifest/container-native-build.json"):
        if marker not in labels:
            _fail("WRONG_BUILD_BOUNDARY_LABEL_MISSING")
    return {
        "schemaVersion": "spec170-sif-build-boundary-v2", "status": "PASS",
        "definition": str(definition),
        "definitionSha256": "sha256:" + hashlib.sha256(text.encode()).hexdigest(),
        "builderStage": "builder", "finalStage": "final", "baseImage": builder.source,
        "hostBinaryInputs": [], "containerNativeBuild": True,
        "runtimeLayout": "installed-v1", "cleanDependencyBaseRequired": True,
        "staleBaseArtifactsReplaced": False,
        "manifestPath": "/opt/ndnsf-di/current/manifest/container-native-build.json",
    }


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
    if "NDNSF_RUNTIME_LAYOUT" in builder_post or "org.ndnsf.di.runtime-layout" in text:
        return _validate_installed_runtime(definition, text, builder, final)
    required_builder_markers = {
        "NDNSF_CONTAINER_BUILD=1": "CONTAINER_BUILD_MARKER_MISSING",
        "di-native-provider": "PROVIDER_BUILD_MISSING",
        "pythonWrapper": "PYTHON_EXTENSION_SOURCE_MISSING",
        "pip": "PYTHON_EXTENSION_BUILD_MISSING",
        "container-configure-closure.json": "CONFIGURE_CLOSURE_MISSING",
        "container-native-build.json": "BUILD_MANIFEST_MISSING",
        "export NDNSF_NAC_ABE_PREFIX=/opt/ndnsf-stage":
            "PYTHON_NAC_STAGE_PREFIX_MISSING",
        "NDNSF_LIBRARY_DIR=/opt/ndnsf-stage/lib":
            "PYTHON_STAGE_LIBRARY_CLOSURE_MISSING",
        "NDNSF_RUNTIME_RPATH='$ORIGIN/../../lib:/opt/ndn-base/lib'":
            "PYTHON_RUNTIME_RPATH_MISSING",
        "APP_ELF_RUNTIME_CHECK=ndnsf-app-v2":
            "APP_ELF_RUNTIME_CHECK_MISSING",
        "/usr/bin/readelf -d \"$native_path\"":
            "APP_ELF_READelf_CHECK_MISSING",
        "LINKED_LIBRARY_ORIGIN_MISMATCH":
            "NATIVE_LIBRARY_ORIGIN_CHECK_MISSING",
        "UNDECLARED_HOST_LIBRARY":
            "NATIVE_HOST_LIBRARY_CHECK_MISSING",
    }
    for marker, code in required_builder_markers.items():
        if marker not in builder_post:
            _fail(f"WRONG_BUILD_BOUNDARY_{code}")
    # This variable selects repository libraries, not general dependency paths.
    # Repo's binding requires Core in every explicit directory; base dependencies
    # are supplied separately by pkg-config and their pinned prefixes.
    library_dirs = re.findall(r'\bNDNSF_LIBRARY_DIR=([^\s\\]+)', builder_post)
    if any(value.strip("\"'") != "/opt/ndnsf-stage/lib" for value in library_dirs):
        _fail("WRONG_BUILD_BOUNDARY_PYTHON_STAGE_LIBRARY_CLOSURE_MISMATCH")
    if re.search(r"(?:^|[\s'\"])/home/", builder_post):
        _fail("WRONG_BUILD_BOUNDARY_HOST_HOME_REFERENCE")

    transfer = final.sections.get("files from builder", "")
    required_transfers = {
        "/opt/ndnsf-stage/bin/di-native-provider": "PROVIDER_TRANSFER_MISSING",
        "/opt/ndnsf-stage/lib/libndn-service-framework": "FRAMEWORK_TRANSFER_MISSING",
        "/opt/ndnsf-stage/lib/libndnsf-distributed-inference.so":
            "DI_LIBRARY_TRANSFER_MISSING",
        "/opt/ndnsf-stage/lib/libndn-svs.so.0.1.0":
            "SVS_LIBRARY_TRANSFER_MISSING",
        "/opt/ndnsf-stage/lib/libnac-abe.so":
            "NAC_ABE_LIBRARY_TRANSFER_MISSING",
        "/opt/ndnsf-stage/lib/libndnsd.so.0.1.0":
            "NDNSD_LIBRARY_TRANSFER_MISSING",
        "/opt/ndnsf-stage/lib/libopenabe.so":
            "OPENABE_LIBRARY_TRANSFER_MISSING",
        "/opt/ndnsf-stage/lib/librelic.so":
            "RELIC_LIBRARY_TRANSFER_MISSING",
        "/opt/ndnsf-stage/lib/librelic_ec.so":
            "RELIC_EC_LIBRARY_TRANSFER_MISSING",
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
        "rm -f /opt/ndnsf-di/current/lib/libndnsf-distributed-inference.so":
            "STALE_DI_LIBRARY_REMOVAL_MISSING",
        "rm -f /opt/venv/lib/python3.10/site-packages/ndnsf/_ndnsf":
            "STALE_EXTENSION_REMOVAL_MISSING",
        "find /opt/venv/lib/python3.10/site-packages/ndnsf":
            "ACTIVE_EXTENSION_CENSUS_MISSING",
        "_ndnsf*.so": "ACTIVE_EXTENSION_PATTERN_MISSING",
        "sha256sum": "FINAL_HASH_CHECK_MISSING",
        "container-native-build.json": "FINAL_MANIFEST_CHECK_MISSING",
        # Spec175 deployment is ONNX Runtime-only.  A base image can retain
        # functorch even when torch itself is absent, so the final stage must
        # remove it explicitly; otherwise the runtime preflight will reject
        # every rebuilt candidate for the same residue.
        "/opt/venv/lib/python3.10/site-packages/functorch*":
            "FUNCTORCH_REMOVAL_MISSING",
        "test ! -e /opt/ndnsf-app":
            "APP_ROOT_PREEXISTENCE_GUARD_MISSING",
        "test ! -L /opt/ndnsf-app":
            "APP_ROOT_SYMLINK_GUARD_MISSING",
        "install -d /opt/ndnsf-app/bin /opt/ndnsf-app/lib /opt/ndnsf-app/python":
            "APP_LAYOUT_DIRECTORIES_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/lib/libndnsf-distributed-inference.so":
            "APP_DI_LIBRARY_INSTALL_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/lib/libndn-service-framework.so.0.1.0":
            "APP_FRAMEWORK_INSTALL_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/lib/libndn-svs.so.0.1.0":
            "APP_SVS_INSTALL_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/lib/libnac-abe.so":
            "APP_NAC_ABE_INSTALL_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/lib/libndnsd.so.0.1.0":
            "APP_NDNSD_INSTALL_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/lib/libopenabe.so":
            "APP_OPENABE_INSTALL_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/lib/librelic.so":
            "APP_RELIC_INSTALL_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/lib/librelic_ec.so":
            "APP_RELIC_EC_INSTALL_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/bin/di-native-provider /opt/ndnsf-app/bin/di-native-provider":
            "APP_PROVIDER_INSTALL_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/bin/di-native-fault-provider /opt/ndnsf-app/bin/di-native-fault-provider":
            "APP_FAULT_PROVIDER_INSTALL_MISSING",
        "install -m 0755 /opt/ndnsf-candidate/bin/App_ServiceController /opt/ndnsf-app/bin/App_ServiceController":
            "APP_CONTROLLER_INSTALL_MISSING",
        "cp -aL /opt/ndnsf-candidate/python/. /opt/ndnsf-app/python/":
            "APP_PYTHON_COPY_MISSING",
        "cp -aL /opt/ndnsf-candidate/replay/. /opt/ndnsf-app/replay/":
            "APP_REPLAY_COPY_MISSING",
        "cp -aL /opt/ndnsf-candidate/replay/source-seal.json /opt/ndnsf-app/manifest/source-seal.json":
            "APP_SOURCE_SEAL_COPY_MISSING",
        "manifest/source-seal.json": "APP_SOURCE_SEAL_MISSING",
        "manifest/app-runtime.lock.json": "APP_RUNTIME_LOCK_MISSING",
        "APP_LAYOUT_VERIFY=ndnsf-app-v2": "APP_LAYOUT_VERIFY_MISSING",
        "APP_NATIVE_LIBRARY_CLOSURE_MISMATCH": "APP_NATIVE_LIBRARY_CLOSURE_CHECK_MISSING",
    }
    for marker, code in required_replacement_markers.items():
        if marker not in final_post:
            _fail(f"WRONG_BUILD_BOUNDARY_{code}")
    if "cp -aL /opt/ndnsf-candidate/lib/. /opt/ndnsf-app/lib/" in final_post:
        _fail("WRONG_BUILD_BOUNDARY_APP_COPIES_BASE_LIBRARIES")
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
