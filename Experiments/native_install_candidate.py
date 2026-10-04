"""Checks for a source-matched, installed native experiment candidate."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sysconfig
from typing import Any


RUN_ID_RE = re.compile(r"^[a-f0-9]{32}$")


def resolve_installed_python_package(requested: str | Path | None = None) -> Path:
    """Resolve the package directory Python will actually import."""
    if requested:
        package = Path(requested).expanduser()
    else:
        spec = importlib.util.find_spec("ndnsf")
        locations = list(spec.submodule_search_locations or []) if spec else []
        if not locations:
            raise RuntimeError("CANDIDATE_PYTHON_PACKAGE_NOT_IMPORTABLE")
        package = Path(locations[0])
    try:
        resolved = package.resolve(strict=True)
    except OSError as error:
        raise RuntimeError(f"CANDIDATE_PYTHON_PACKAGE_MISSING:{package}") from error
    if not resolved.is_dir():
        raise RuntimeError(f"CANDIDATE_PYTHON_PACKAGE_MISSING:{resolved}")
    return resolved


def apply_yolo_repo_operation_policy(policy_path: Path,
                                    template_path: Path) -> dict[str, Any]:
    """Add repo grants and independent probe grants to splitter output."""
    try:
        import yaml  # type: ignore
    except ImportError as error:
        raise RuntimeError("CANDIDATE_PYYAML_REQUIRED") from error
    try:
        config = yaml.safe_load(policy_path.read_text(encoding="utf-8")) or {}
        template = yaml.safe_load(template_path.read_text(encoding="utf-8")) or {}
    except (OSError, ValueError) as error:
        raise RuntimeError("CANDIDATE_REPO_POLICY_UNREADABLE") from error
    if config.get("controller") != template.get("controller"):
        raise RuntimeError("CANDIDATE_REPO_TEMPLATE_CONTROLLER_MISMATCH")
    if config.get("runtime", {}).get("user_identity") != \
            template.get("runtime", {}).get("user_identity"):
        raise RuntimeError("CANDIDATE_REPO_TEMPLATE_USER_MISMATCH")

    prefix = "/NDNSF/DistributedRepo/Object/v1/"
    repo_services = [
        service for service in template.get("services", [])
        if isinstance(service, dict)
        and str(service.get("name", "")).startswith(prefix)
    ]
    if not repo_services:
        raise RuntimeError("CANDIDATE_REPO_OPERATION_POLICY_MISSING")
    services = list(config.get("services", []))
    probe_name = "/NDNSF/Network/Probe/v1"
    probe_template = next((service for service in template.get("services", [])
                           if isinstance(service, dict)
                           and service.get("name") == probe_name), None)
    model_service = next((service for service in services
                          if isinstance(service, dict)
                          and str(service.get("name", "")).startswith("/AI/YOLO/")), None)
    if probe_template is None or model_service is None:
        raise RuntimeError("CANDIDATE_NETWORK_PROBE_POLICY_MISSING")
    # The generated model roster is authoritative. Do not grant the static
    # template's Provider identities when a run uses a different mapping.
    probe_service = dict(probe_template)
    probe_service["users"] = list(model_service.get("users", []))
    probe_service["providers"] = [
        {"identity": provider["identity"], "roles": []}
        for provider in model_service.get("providers", [])
        if isinstance(provider, dict) and provider.get("identity")
    ]
    repo_services.append(probe_service)
    by_name = {service.get("name"): index
               for index, service in enumerate(services)
               if isinstance(service, dict)}
    for service in repo_services:
        name = service["name"]
        if name in by_name:
            services[by_name[name]] = service
        else:
            by_name[name] = len(services)
            services.append(service)
    config["services"] = services

    controller = str(config["controller"])
    users_by_service = {
        service.get("name"): set(service.get("users", []))
        for service in services if isinstance(service, dict)
    }
    for operation in ("STATUS", "INSERT"):
        if controller not in users_by_service.get(prefix + operation, set()):
            raise RuntimeError(f"CANDIDATE_REPO_CONTROLLER_GRANT_MISSING:{operation}")

    users: dict[str, set[str]] = {}
    providers: dict[str, list[dict[str, Any]]] = {}
    for service in services:
        if not isinstance(service, dict):
            continue
        name = str(service.get("name", ""))
        for identity in service.get("users", []):
            users.setdefault(str(identity), set()).add(name)
        for provider in service.get("providers", []):
            if isinstance(provider, dict) and provider.get("identity"):
                providers.setdefault(str(provider["identity"]), []).append({
                    "service": name,
                    "roles": provider.get("roles", []),
                })
    config["authorization_summary"] = {
        "users": [{"identity": identity, "services": sorted(names)}
                  for identity, names in sorted(users.items())],
        "providers": [{"identity": identity, "services": rows}
                      for identity, rows in sorted(providers.items())],
    }
    policy_path.write_text(yaml.safe_dump(config, sort_keys=False),
                           encoding="utf-8")
    return config


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def file_identity(path: Path) -> dict[str, Any]:
    resolved = path.expanduser().resolve(strict=True)
    if not resolved.is_file():
        raise RuntimeError(f"CANDIDATE_INPUT_NOT_FILE:{resolved}")
    return {"path": str(resolved), "bytes": resolved.stat().st_size,
            "sha256": sha256_file(resolved)}


def binary_check(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {"path": str(path), "exists": path.is_file(),
                              "executable": os.access(path, os.X_OK)}
    if not result["exists"] or not result["executable"]:
        return {**result, "status": "FAIL", "reason": "MISSING_OR_NOT_EXECUTABLE"}
    result["sha256"] = sha256_file(path)
    readelf = subprocess.run(["/usr/bin/readelf", "-d", str(path)],
                             text=True, capture_output=True, check=False)
    ldd = subprocess.run(["/usr/bin/ldd", "-r", str(path)],
                         text=True, capture_output=True, check=False)
    dynamic = readelf.stdout + readelf.stderr
    ldd_output = ldd.stdout + ldd.stderr
    unresolved = [line.strip() for line in ldd_output.splitlines()
                  if "not found" in line or "undefined symbol" in line]
    python_runtime_symbols: list[str] = []
    extension_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    is_ndnsf_extension = bool(extension_suffix) and path.name == f"_ndnsf{extension_suffix}"
    if is_ndnsf_extension:
        remaining = []
        for line in unresolved:
            match = re.search(r"undefined symbol:\s*([^\s(]+)", line)
            symbol = match.group(1) if match else ""
            if symbol.startswith(("Py", "_Py")):
                # CPython resolves its C API symbols when it loads an extension.
                python_runtime_symbols.append(line)
            else:
                remaining.append(line)
        unresolved = remaining
    libraries: dict[tuple[str, str], dict[str, str]] = {}
    for line in ldd_output.splitlines():
        dependency = line.strip()
        if "=>" in dependency:
            name, target = (part.strip() for part in dependency.split("=>", 1))
            if not target.startswith("/"):
                continue
            library_path = target.split(" (", 1)[0].strip()
        elif dependency.startswith("/"):
            library_path = dependency.split(" (", 1)[0].strip()
            name = Path(library_path).name
        else:
            continue
        try:
            resolved = Path(library_path).resolve(strict=True)
            if not resolved.is_file():
                raise OSError("not a regular file")
            libraries[(name, str(resolved))] = {
                "name": name, "path": str(resolved), "sha256": sha256_file(resolved)}
        except (OSError, RuntimeError) as error:
            unresolved.append(f"{name} => {library_path}: {error}")
    result.update({
        "readelfExit": readelf.returncode,
        "lddExit": ldd.returncode,
        "runpaths": [line.strip() for line in dynamic.splitlines()
                     if "RPATH" in line or "RUNPATH" in line],
        "unresolved": unresolved,
        "pythonRuntimeSymbols": python_runtime_symbols,
        "sharedLibraries": sorted(libraries.values(),
                                   key=lambda item: (item["name"], item["path"])),
        "status": "PASS" if readelf.returncode == 0 and ldd.returncode == 0
        and not unresolved else "FAIL",
    })
    if result["status"] == "FAIL":
        result["reason"] = "ABI_OR_LIBRARY_CLOSURE"
    return result


def _inventory_files(paths: list[Path]) -> list[dict[str, Any]]:
    unique = sorted({path.expanduser().resolve(strict=True) for path in paths})
    return [file_identity(path) for path in unique]


def _package_files(package: Path) -> dict[str, str]:
    files = {}
    for path in sorted(package.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(package)
        if "__pycache__" in relative.parts or path.suffix == ".pyc":
            continue
        files[str(relative)] = sha256_file(path)
    return files


def _package_sources(package: Path) -> dict[str, str]:
    return {str(path.relative_to(package)): sha256_file(path)
            for path in sorted(package.rglob("*.py")) if path.is_file()}


def _service_permission(policy_path: Path, service_name: str,
                        requester_identity: str) -> dict[str, Any]:
    try:
        import yaml  # type: ignore
    except ImportError as error:
        raise RuntimeError("CANDIDATE_PYYAML_REQUIRED") from error
    try:
        policy = yaml.safe_load(policy_path.read_text(encoding="utf-8")) or {}
    except (OSError, ValueError) as error:
        raise RuntimeError(f"CANDIDATE_POLICY_UNREADABLE:{policy_path}") from error
    service = next((entry for entry in policy.get("services", [])
                    if isinstance(entry, dict) and
                    entry.get("name") == service_name), None)
    if service is None:
        raise RuntimeError(f"CANDIDATE_MODEL_SERVICE_MISSING:{service_name}")
    users = service.get("users", [])
    roles = service.get("roles", [])
    providers = service.get("providers", [])
    if requester_identity not in users:
        raise RuntimeError(
            f"CANDIDATE_TARGETED_REQUESTER_NOT_AUTHORIZED:{requester_identity}")
    if not roles or not providers:
        raise RuntimeError(f"CANDIDATE_MODEL_SERVICE_INCOMPLETE:{service_name}")
    return {"service": service_name, "requester": requester_identity,
            "authorizedByExistingServiceGrant": True,
            "roles": list(roles), "providers": providers}


def build_yolo_native_candidate(
        *, run_id: str, components: dict[str, tuple[Path, Path]],
        extension: tuple[Path, Path], source_package: Path,
        installed_package: Path, source_app_package: Path,
        topology: Path, policy: Path,
        model_directory: Path, model_input: str, input_size: int,
        layout: str, service_name: str, requester_identity: str,
        provider_nodes: list[str], profile: dict[str, Any],
        runtime_inputs: list[Path] | None = None) -> dict[str, Any]:
    """Validate and identify the exact installed binaries and YOLO inputs."""
    if not RUN_ID_RE.fullmatch(run_id):
        raise RuntimeError("CANDIDATE_RUN_ID_INVALID")

    installed_components: dict[str, Any] = {}
    for name, (installed_path, build_path) in sorted(components.items()):
        installed = binary_check(installed_path)
        built = binary_check(build_path)
        if installed["status"] != "PASS" or built["status"] != "PASS":
            raise RuntimeError(f"CANDIDATE_COMPONENT_CLOSURE_FAILED:{name}")
        if installed["sha256"] != built["sha256"]:
            raise RuntimeError(f"CANDIDATE_COMPONENT_STALE:{name}")
        installed_libraries = sorted(
            (item["name"], item["sha256"])
            for item in installed.get("sharedLibraries", []))
        build_libraries = sorted(
            (item["name"], item["sha256"])
            for item in built.get("sharedLibraries", []))
        if installed_libraries != build_libraries:
            raise RuntimeError(f"CANDIDATE_LIBRARY_CLOSURE_STALE:{name}")
        installed_components[name] = {"installed": installed, "build": built}

    extension_path, built_extension_path = extension
    installed_ext = binary_check(extension_path)
    built_ext = binary_check(built_extension_path)
    if installed_ext["status"] != "PASS" or built_ext["status"] != "PASS":
        raise RuntimeError("CANDIDATE_PYTHON_EXTENSION_CLOSURE_FAILED")
    if installed_ext["sha256"] != built_ext["sha256"]:
        raise RuntimeError("CANDIDATE_PYTHON_EXTENSION_STALE")
    if sorted((item["name"], item["sha256"])
              for item in installed_ext.get("sharedLibraries", [])) != sorted(
              (item["name"], item["sha256"])
              for item in built_ext.get("sharedLibraries", [])):
        raise RuntimeError("CANDIDATE_PYTHON_LIBRARY_CLOSURE_STALE")
    if _package_sources(source_package) != _package_sources(installed_package):
        raise RuntimeError("CANDIDATE_PYTHON_PACKAGE_STALE")
    source_app_package = source_app_package.expanduser().resolve(strict=True)
    if not source_app_package.is_dir():
        raise RuntimeError("CANDIDATE_PYTHON_APPLICATION_PACKAGE_MISSING")

    authorization = _service_permission(policy, service_name, requester_identity)
    model_files = [path for path in model_directory.rglob("*") if path.is_file()]
    if not model_files:
        raise RuntimeError(f"CANDIDATE_MODEL_ARTIFACTS_MISSING:{model_directory}")
    input_files = _inventory_files([topology, policy, *model_files])
    runtime_assets = _inventory_files(runtime_inputs or [])
    record: dict[str, Any] = {
        "schema": "ndnsf-di-spec196-yolo-candidate-v1",
        "runId": run_id,
        "profile": profile,
        "model": {"input": model_input, "inputSize": input_size,
                  "artifacts": _inventory_files(model_files)},
        "authorization": authorization,
        "components": installed_components,
        "pythonRequester": {
            "source": str(source_package.resolve()),
            "installed": str(installed_package.resolve()),
            "extension": {"installed": installed_ext, "build": built_ext},
            "pythonSources": _package_sources(installed_package),
        },
        "pythonApplication": {
            "source": str(source_app_package),
            "packageFiles": _package_files(source_app_package),
        },
        "inputs": input_files,
        "runtimeAssets": runtime_assets,
        "topology": str(topology.resolve()),
        "modelService": service_name,
        "providerNodes": list(provider_nodes),
        "layout": layout,
        "status": "PASS",
    }
    digest_payload = json.dumps(record, sort_keys=True, separators=(",", ":"))
    record["candidateDigest"] = hashlib.sha256(digest_payload.encode()).hexdigest()
    return record


def write_candidate(path: Path, candidate: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(candidate, stream, indent=2, sort_keys=True)
        stream.write("\n")
