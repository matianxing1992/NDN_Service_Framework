"""Checks for a source-matched, installed native experiment candidate."""

from __future__ import annotations

import hashlib
import base64
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sysconfig
import sqlite3
from typing import Any
from spec201_naming_inventory import naming_inventory
from spec201_sdk_closure import verify_sdk_closure


RUN_ID_RE = re.compile(r"^[a-f0-9]{32}$")
SPEC201_CLOSURE_SCHEMA = "ndnsf-spec201-native-closure-1"
SPEC201_DISCOVERY_ENVIRONMENT = {
    "yolo": {"NDNSF_DISABLE_NDNSD": "1"},
    "qwen": {"NDNSF_ENABLE_NDNSD": "0"},
}


def inspect_spec201_credentials(bundles: dict[str, Any]) -> None:
    """Read actual NDN credentials without importing them into a keychain."""
    from ndn.encoding import Name, Component, parse_data, parse_and_check_tl, parse_tl_num
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.backends import default_backend
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import rsa, ec, padding
    from cryptography.exceptions import InvalidSignature, UnsupportedAlgorithm
    from ndn.app_support.security_v2 import parse_certificate
    from datetime import datetime, timezone

    def decoded(path):
        raw = Path(path).read_bytes()
        if raw[:1] in (b"\x06", b"\x80"):
            return raw
        return base64.b64decode(b"".join(line.strip() for line in raw.splitlines()
                                        if not line.startswith(b"-----")), validate=True)

    def certificate(wire, identity, kind="rsa"):
        name, meta, content, signature = parse_data(wire)
        subject = Name.from_str(identity)
        if (len(name) != len(subject) + 4 or
                Name.to_bytes(name[:-4]) != Name.to_bytes(subject) or
                bytes(name[-4]) != bytes(Component.from_str("KEY")) or
                meta.content_type != 2 or not content or not signature.signature_info):
            raise ValueError("certificate subject or format mismatch")
        public = serialization.load_der_public_key(bytes(content), backend=default_backend())
        expected = rsa.RSAPublicKey if kind == "rsa" else ec.EllipticCurvePublicKey
        if not isinstance(public, expected):
            raise ValueError("credential key type mismatch")
        return Name.to_bytes(name[:-2]), public

    def public_bytes(key):
        return key.public_bytes(serialization.Encoding.DER,
                                serialization.PublicFormat.SubjectPublicKeyInfo)

    def trusted_certificate(wire, signer_name, signer, signer_certificate=None):
        _, _, _, signature = parse_data(wire)
        info = signature.signature_info
        if (not info.key_locator or not info.key_locator.name or
                Name.to_bytes(info.key_locator.name) not in (signer_name, signer_certificate)):
            raise ValueError("certificate signer mismatch")
        signed = b"".join(bytes(part) for part in signature.signature_covered_part)
        value = bytes(signature.signature_value_buf)
        if isinstance(signer, rsa.RSAPublicKey) and info.signature_type == 1:
            signer.verify(value, signed, padding.PKCS1v15(), hashes.SHA256())
        elif isinstance(signer, ec.EllipticCurvePublicKey) and info.signature_type == 3:
            signer.verify(value, signed, ec.ECDSA(hashes.SHA256()))
        else:
            raise ValueError("certificate signature type mismatch")
        validity = parse_certificate(wire).signature_info.validity_period
        if validity is None:
            raise ValueError("certificate validity missing")
        before, after = (datetime.strptime(bytes(value).decode("ascii"), "%Y%m%dT%H%M%S")
                         .replace(tzinfo=timezone.utc)
                         for value in (validity.not_before, validity.not_after))
        if not before <= datetime.now(timezone.utc) <= after:
            raise ValueError("certificate outside validity period")

    def safe_bag(path, identity, kind, root_name, root_public, root_certificate_name):
        body = parse_and_check_tl(decoded(path), 128)
        typ, n = parse_tl_num(body, 0)
        size, m = parse_tl_num(body, n)
        end = n + m + size
        if typ != 6 or end >= len(body):
            raise ValueError("invalid SafeBag certificate")
        _, public = certificate(bytes(body[:end]), identity, kind)
        trusted_certificate(bytes(body[:end]), root_name, root_public, root_certificate_name)
        encrypted = bytes(parse_and_check_tl(body[end:], 129))
        private = serialization.load_der_private_key(
            encrypted, password=b"123456", backend=default_backend())
        if public_bytes(private.public_key()) != public_bytes(public):
            raise ValueError("SafeBag certificate/private key mismatch")

    try:
        yolo, qwen = bundles["yolo"], bundles["qwen"]
        root_wire = decoded(yolo["rootCert"])
        root_name, root_public = certificate(root_wire, yolo["applicationRoot"])
        root_certificate_name = Name.to_bytes(parse_data(root_wire)[0])
        trusted_certificate(root_wire, root_name, root_public, root_certificate_name)
        for identity, keys in yolo["keys"].items():
            safe_bag(keys["rsa"], identity, "rsa", root_name, root_public, root_certificate_name)
            if keys.get("ec"):
                safe_bag(keys["ec"], identity, "ec", root_name, root_public, root_certificate_name)
        home = Path(qwen["bootstrapHome"])
        with sqlite3.connect((home / "pib/pib.db").resolve().as_uri() + "?mode=ro",
                             uri=True) as conn:
            all_identities = [Name.to_str(Name.decode(bytes(row[0]))[0])
                              for row in conn.execute("select identity from identities")]
            if (len(all_identities) != len(qwen["identities"]) or
                    set(all_identities) != set(qwen["identities"])):
                raise ValueError("bootstrap identity inventory mismatch")
            if (conn.execute("select count(*) from keys").fetchone()[0] != len(all_identities)
                    or conn.execute("select count(*) from certificates").fetchone()[0]
                    != len(all_identities)):
                raise ValueError("bootstrap key/certificate inventory mismatch")
            rows = conn.execute(
                "select identities.identity, keys.key_name, keys.key_bits, "
                "certificates.certificate_name, certificates.certificate_data from identities "
                "join keys on keys.identity_id=identities.id and keys.is_default=1 "
                "join certificates on certificates.key_id=keys.id and certificates.is_default=1").fetchall()
            if len(rows) != len(qwen["identities"]):
                raise ValueError("bootstrap default key/certificate coverage mismatch")
            seen = set()
            for identity_wire, key_name, key_bits, cert_name, cert_wire in rows:
                identity = Name.to_str(Name.decode(bytes(identity_wire))[0])
                if identity in seen or identity not in qwen["certs"]:
                    raise ValueError("bootstrap foreign or duplicate default identity")
                seen.add(identity)
                if bytes(cert_wire) != decoded(qwen["certs"][identity]):
                    raise ValueError("bootstrap default certificate mismatch")
                actual_key, public = certificate(bytes(cert_wire), identity)
                trusted_certificate(bytes(cert_wire), actual_key, public, bytes(cert_name))
                if bytes(cert_name) != Name.to_bytes(parse_data(bytes(cert_wire))[0]):
                    raise ValueError("bootstrap certificate name mismatch")
                if actual_key != bytes(key_name) or public_bytes(public) != bytes(key_bits):
                    raise ValueError("bootstrap public key mismatch")
                private_path = home / "tpm/ndnsec-key-file" / (
                    hashlib.sha256(bytes(key_name)).hexdigest() + ".privkey")
                private = serialization.load_der_private_key(
                    decoded(private_path), password=None, backend=default_backend())
                if public_bytes(private.public_key()) != public_bytes(public):
                    raise ValueError("bootstrap TPM owner mismatch")
    except (OSError, ValueError, TypeError, KeyError, IndexError, AttributeError,
            InvalidSignature, UnsupportedAlgorithm, sqlite3.Error) as error:
        raise RuntimeError("SPEC201_CANDIDATE_CREDENTIAL_IDENTITY_INVALID") from error


def spec201_profile_arguments(arguments: dict[str, Any]) -> dict[str, Any]:
    """Keep effective options, excluding only per-run storage and self identity."""
    transient = {"spec201_candidate", "spec201_candidate_sha256", "run_root",
                 "fixed_work_root", "results_dir", "requester_publication_dir",
                 "artifact_cache_root", "encrypted_repository_path"}
    return {key: str(value.expanduser().resolve()) if isinstance(value, Path) else value
            for key, value in sorted(arguments.items()) if key not in transient}


def validate_spec201_candidate(path: Path, expected_sha256: str,
                               executables: list[Path],
                               selected_inputs: list[Path] | None = None,
                               selected_profile: tuple[str, dict[str, Any]] | None = None) -> dict[str, Any]:
    """Read-only fence, called before staging and again immediately before launch.

    The frozen bundle includes both models and their launch inputs so that both
    runs use one candidateId. A successful earlier run is not a substitute
    for comparing the files and dependency closure that will be used now.
    """
    if not re.fullmatch(r"[a-f0-9]{64}", expected_sha256 or ""):
        raise RuntimeError("SPEC201_CANDIDATE_DIGEST_REQUIRED")
    identity = file_identity(path)
    if identity["sha256"] != expected_sha256:
        raise RuntimeError("SPEC201_CANDIDATE_DIGEST_MISMATCH")
    try:
        record = json.loads(Path(identity["path"]).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError("SPEC201_CANDIDATE_UNREADABLE") from error
    if not isinstance(record, dict) or record.get("schema") != SPEC201_CLOSURE_SCHEMA:
        raise RuntimeError("SPEC201_CANDIDATE_SCHEMA_MISMATCH")
    unsigned = {key: value for key, value in record.items() if key != "candidateId"}
    digest = hashlib.sha256(json.dumps(unsigned, sort_keys=True,
                                      separators=(",", ":")).encode()).hexdigest()
    if record.get("candidateId") != digest:
        raise RuntimeError("SPEC201_CANDIDATE_ID_MISMATCH")
    if selected_profile is not None:
        name, arguments = selected_profile
        if record.get("profileArguments", {}).get(name) != spec201_profile_arguments(arguments):
            raise RuntimeError("SPEC201_CANDIDATE_EFFECTIVE_CONFIGURATION_MISMATCH")
    checked_files = {}
    for group in ("sources", "headers", "contracts", "oracles", "models", "buildMaps",
                  "configs", "generatedSchemas", "topologies"):
        rows = record.get(group)
        if not isinstance(rows, list) or not rows:
            raise RuntimeError(f"SPEC201_CANDIDATE_INVENTORY_MISSING:{group}")
        for row in rows:
            if not isinstance(row, dict) or set(row) != {"path", "bytes", "sha256"}:
                raise RuntimeError(f"SPEC201_CANDIDATE_FILE_RECORD_INVALID:{group}")
            if file_identity(Path(row["path"])) != row:
                raise RuntimeError(f"SPEC201_CANDIDATE_FILE_CHANGED:{row['path']}")
            checked_files[row["path"]] = row
    for selected in selected_inputs or []:
        actual = file_identity(selected)
        if checked_files.get(actual["path"]) != actual:
            raise RuntimeError(f"SPEC201_CANDIDATE_LAUNCH_INPUT_NOT_FROZEN:{actual['path']}")
    for schema in record["generatedSchemas"]:
        text = Path(schema["path"]).read_text(encoding="utf-8")
        active = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
        if "trust-anchor" not in active or re.search(r"\btype\s+any\b", active):
            raise RuntimeError("SPEC201_CANDIDATE_UNANCHORED_SCHEMA")
    bundles = record.get("credentialBundles")
    if not isinstance(bundles, dict) or set(bundles) != {"yolo", "qwen"}:
        raise RuntimeError("SPEC201_CANDIDATE_CREDENTIAL_BUNDLES_REQUIRED")
    def frozen_path(value):
        identity = file_identity(Path(value))
        if checked_files.get(identity["path"]) != identity:
            raise RuntimeError("SPEC201_CANDIDATE_CREDENTIAL_NOT_FROZEN")
        return Path(identity["path"])
    for bundle in bundles.values():
        if not isinstance(bundle, dict) or not bundle.get("identities"):
            raise RuntimeError("SPEC201_CANDIDATE_CREDENTIAL_IDENTITIES_REQUIRED")
        schema = frozen_path(bundle["schema"])
        if str(schema) not in {row["path"] for row in record["generatedSchemas"]}:
            raise RuntimeError("SPEC201_CANDIDATE_CREDENTIAL_SCHEMA_NOT_FROZEN")
    yolo = bundles["yolo"]
    frozen_path(yolo["rootCert"])
    if set(yolo["keys"]) != set(yolo["identities"]):
        raise RuntimeError("SPEC201_CANDIDATE_CREDENTIAL_KEYS_MISMATCH")
    for keys in yolo["keys"].values():
        frozen_path(keys["rsa"])
        if keys.get("ec"):
            frozen_path(keys["ec"])
    qwen = bundles["qwen"]
    if set(qwen["certs"]) != set(qwen["identities"]):
        raise RuntimeError("SPEC201_CANDIDATE_CREDENTIAL_CERTS_MISMATCH")
    for cert in qwen["certs"].values():
        frozen_path(cert)
    from native_probe_validation_links import render_anchored_probe_validation_schema
    for name, certs in (("yolo", [yolo["rootCert"]]),
                        ("qwen", [qwen["certs"][identity]
                                  for identity in sorted(qwen["certs"])])):
        bundle = bundles[name]
        expected = render_anchored_probe_validation_schema(bundle["applicationRoot"], certs)
        if Path(bundle["schema"]).read_text() != expected:
            raise RuntimeError("SPEC201_CANDIDATE_CREDENTIAL_SCHEMA_MISMATCH")
    bootstrap = Path(qwen["bootstrapHome"]).resolve(strict=True)
    for directory in ("pib", "tpm"):
        members = [p for p in (bootstrap / directory).rglob("*") if p.is_file()]
        if not members:
            raise RuntimeError("SPEC201_CANDIDATE_CREDENTIAL_BOOTSTRAP_EMPTY")
        for member in members:
            frozen_path(member)
    inspect_spec201_credentials(bundles)
    header_pairs = record.get("installedHeaders")
    if not isinstance(header_pairs, list) or not header_pairs:
        raise RuntimeError("SPEC201_CANDIDATE_INSTALLED_HEADERS_MISSING")
    for pair in header_pairs:
        source, installed = pair["source"], pair["installed"]
        if (source["path"] == installed["path"]
                or source["sha256"] != installed["sha256"]
                or checked_files.get(source["path"]) != source
                or checked_files.get(installed["path"]) != installed):
            raise RuntimeError("SPEC201_CANDIDATE_INSTALLED_HEADER_MISMATCH")
    coverage = record.get("inventory")
    required = {f"C{index:02d}" for index in range(2, 11)} | {"C12"}
    if not isinstance(coverage, dict) or set(coverage) != required:
        raise RuntimeError("SPEC201_CANDIDATE_FAMILY_COVERAGE_INCOMPLETE")
    if coverage != naming_inventory(Path(__file__).resolve().parents[1]):
        raise RuntimeError("SPEC201_CANDIDATE_CONTRACT_ROW_COVERAGE_MISMATCH")
    for family, rows in coverage.items():
        if not isinstance(rows, list) or not rows:
            raise RuntimeError(f"SPEC201_CANDIDATE_FAMILY_EMPTY:{family}")
        for row in rows:
            fields = ("writer", "consumer", "validator", "referenceOwner", "target")
            if not isinstance(row, dict) or any(not row.get(key) for key in fields):
                raise RuntimeError(f"SPEC201_CANDIDATE_FAMILY_OWNER_MISSING:{family}")
            if (not isinstance(row.get("sourceFiles"), list) or not row["sourceFiles"]
                    or any(source not in checked_files for source in row["sourceFiles"])):
                raise RuntimeError(f"SPEC201_CANDIDATE_FAMILY_SOURCE_NOT_FROZEN:{family}")
    from native_prefix_roles import prefix_role_plan
    role_plans = record.get("prefixRolePlans", {})
    if set(role_plans) != {"yolo", "qwen"}:
        raise RuntimeError("SPEC201_CANDIDATE_PREFIX_ROLES_MISSING")
    for plan in role_plans.values():
        if plan.get("protocolVersion") != 3:
            raise RuntimeError("SPEC201_CANDIDATE_SVS_PROTOCOL_MISMATCH")
        actual_roles = prefix_role_plan(actors=plan["actors"], groups=plan["groups"],
                                       data_roots=plan["dataRoots"],
                                       protocol_version=plan["protocolVersion"])
        if list(actual_roles) != plan.get("roles"):
            raise RuntimeError("SPEC201_CANDIDATE_PREFIX_ROLE_PLAN_MISMATCH")
    if record.get("discoveryEnvironment") != SPEC201_DISCOVERY_ENVIRONMENT:
        raise RuntimeError("SPEC201_CANDIDATE_DISCOVERY_CONFIGURATION_MISMATCH")
    artifacts = record.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        raise RuntimeError("SPEC201_CANDIDATE_ARTIFACTS_MISSING")
    build_artifacts = {}
    for build_map in record["buildMaps"]:
        graph = json.loads(Path(build_map["path"]).read_text(encoding="utf-8"))
        if graph.get("schema") != "ndnsf-spec201-build-closure-1" or not graph.get("rows"):
            raise RuntimeError("SPEC201_CANDIDATE_BUILD_MAP_INVALID")
        for row in graph["rows"]:
            if (not row.get("storedSignature")
                    or row.get("storedSignature") != row.get("currentSignature")):
                raise RuntimeError("SPEC201_CANDIDATE_BUILD_SIGNATURE_STALE")
            for item in [row["source"], *row["dependencies"], *row["outputs"]]:
                if file_identity(Path(item["path"])) != item:
                    raise RuntimeError("SPEC201_CANDIDATE_BUILD_INPUT_CHANGED")
        for artifact in graph.get("artifacts", []):
            build_artifacts[artifact["path"]] = artifact["sha256"]
    qualified = set()
    sdk_artifacts = {}
    for sdk_receipt in record.get("sdkReceipts", []):
        if sdk_receipt not in checked_files:
            raise RuntimeError("SPEC201_CANDIDATE_SDK_RECEIPT_NOT_FROZEN")
        proof = json.loads(Path(sdk_receipt).read_text(encoding="utf-8"))
        built = verify_sdk_closure(proof, binary_check)
        sdk_artifacts[built["path"]] = built["sha256"]
    binary_hashes = {}
    for artifact in artifacts:
        built = artifact.get("build", {})
        if (build_artifacts.get(built.get("path")) != built.get("sha256")
                and sdk_artifacts.get(built.get("path")) != built.get("sha256")):
            raise RuntimeError("SPEC201_CANDIDATE_ARTIFACT_OUTSIDE_BUILD_MAP")
        for key in ("installed", "build"):
            prior = artifact.get(key, {})
            current = binary_check(Path(prior.get("path", "")))
            fields = ("sha256", "sharedLibraries", "status")
            if current.get("status") != "PASS" or any(
                    current.get(field) != prior.get(field) for field in fields):
                raise RuntimeError(f"SPEC201_CANDIDATE_BINARY_CHANGED:{prior.get('path')}")
        if artifact["installed"]["sha256"] != artifact["build"]["sha256"]:
            raise RuntimeError("SPEC201_CANDIDATE_INSTALLED_BUILD_MISMATCH")
        def libraries(value):
            return sorted((entry["name"], entry["sha256"])
                          for entry in value["sharedLibraries"])
        if libraries(artifact["installed"]) != libraries(artifact["build"]):
            raise RuntimeError("SPEC201_CANDIDATE_LIBRARY_CLOSURE_MISMATCH")
        qualified.add(Path(artifact["installed"]["path"]).resolve())
        for key in ("installed", "build"):
            binary_hashes[str(Path(artifact[key]["path"]).resolve())] = artifact[key]["sha256"]
    if any(executable.expanduser().resolve() not in qualified for executable in executables):
        raise RuntimeError("SPEC201_CANDIDATE_EXECUTABLE_NOT_QUALIFIED")
    validation = record.get("validation", {})
    if (validation.get("backend") != "original-packets"
            or validation.get("nameBytesHardLimit") != 1024
            or validation.get("packetBytesHardLimit") != 8800
            or validation.get("uriModes") != [0, 1]
            or validation.get("status") != "PASS"):
        raise RuntimeError("SPEC201_CANDIDATE_VALIDATION_CONTRACT_MISMATCH")
    receipts = validation.get("nativeReceipts")
    if not isinstance(receipts, list) or not receipts:
        raise RuntimeError("SPEC201_CANDIDATE_NATIVE_RECEIPTS_MISSING")
    observed = set()
    for receipt_path in receipts:
        if receipt_path not in checked_files:
            raise RuntimeError("SPEC201_CANDIDATE_NATIVE_RECEIPT_NOT_FROZEN")
        receipt = json.loads(Path(receipt_path).read_text(encoding="utf-8"))
        command = receipt.get("command", [])
        if not command or receipt.get("exit_status") != 0:
            raise RuntimeError("SPEC201_CANDIDATE_NATIVE_RECEIPT_FAILED")
        binary = str(Path(command[0]).resolve())
        if binary_hashes.get(binary) != receipt.get("binary_sha256"):
            raise RuntimeError("SPEC201_CANDIDATE_NATIVE_ORACLE_STALE")
        mode = receipt.get("environment", {}).get("NDN_NAME_ALT_URI")
        if mode not in ("0", "1"):
            raise RuntimeError("SPEC201_CANDIDATE_NATIVE_URI_MODE_MISSING")
        log_path = receipt.get("log")
        if log_path not in checked_files:
            raise RuntimeError("SPEC201_CANDIDATE_NATIVE_LOG_NOT_FROZEN")
        log = Path(log_path).read_text(encoding="utf-8")
        passed = re.search(r"(\d+) test cases? out of (\d+) passed", log)
        assertions = re.search(r"(\d+) assertions? out of (\d+) passed", log)
        if (not passed or int(passed.group(1)) == 0 or passed.group(1) != passed.group(2)
                or not assertions or int(assertions.group(1)) == 0
                or assertions.group(1) != assertions.group(2)
                or any(marker in log for marker in
                       ("ERROR: AddressSanitizer", "runtime error:", "LeakSanitizer"))):
            raise RuntimeError("SPEC201_CANDIDATE_NATIVE_LOG_FAILED")
        selectors = [arg for arg in command[1:] if arg.startswith("--run_test=")]
        if selectors != ["--run_test=*"]:
            raise RuntimeError("SPEC201_CANDIDATE_NATIVE_SELECTOR_INCOMPLETE")
        observed.add((Path(binary).name, mode))
    targets = {row["target"] for rows in coverage.values() for row in rows}
    targets.add("spec201-candidate-closure")
    targets.add("spec201-transparent-repo")
    if any((target, mode) not in observed for target in targets for mode in ("0", "1")):
        raise RuntimeError("SPEC201_CANDIDATE_NATIVE_FAMILY_NOT_QUALIFIED")
    return record


def freeze_spec201_candidate(*, files: dict[str, list[Path]],
                             artifacts: list[tuple[Path, Path]],
                             native_receipts: list[Path],
                             prefix_role_plans: dict[str, Any],
                             installed_headers: list[tuple[Path, Path]],
                             profile_arguments: dict[str, dict[str, Any]],
                             credential_bundles: dict[str, Any],
                             sdk_receipts: list[Path] = ()) -> dict[str, Any]:
    """Collect current bytes; validation still has to accept the resulting record."""
    record = {"schema": SPEC201_CLOSURE_SCHEMA,
              **{group: _inventory_files(paths) for group, paths in files.items()},
              "inventory": naming_inventory(Path(__file__).resolve().parents[1]),
              "prefixRolePlans": prefix_role_plans,
              "credentialBundles": credential_bundles,
              "sdkReceipts": [str(path.resolve(strict=True)) for path in sdk_receipts],
              "profileArguments": {name: spec201_profile_arguments(arguments)
                                   for name, arguments in profile_arguments.items()},
              "installedHeaders": [{"source": file_identity(source),
                                    "installed": file_identity(installed)}
                                   for source, installed in installed_headers],
              "discoveryEnvironment": SPEC201_DISCOVERY_ENVIRONMENT,
              "artifacts": [{"installed": binary_check(installed),
                             "build": binary_check(built)}
                            for installed, built in artifacts],
              "validation": {"backend": "original-packets", "status": "PASS",
                             "nameBytesHardLimit": 1024, "packetBytesHardLimit": 8800,
                             "uriModes": [0, 1],
                             "nativeReceipts": [str(path.resolve(strict=True))
                                                for path in native_receipts]}}
    payload = json.dumps(record, sort_keys=True, separators=(",", ":"))
    record["candidateId"] = hashlib.sha256(payload.encode()).hexdigest()
    return record


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
    # The native API registers each operation as its own Core service.
    # Reuse the template's explicit actors, adding only the native calls used
    # by publication, fetch and replica workers.
    read_template = next(service for service in repo_services
                         if service.get("name") == prefix + "STATUS")
    write_template = next(service for service in repo_services
                          if service.get("name") == prefix + "INSERT")
    repo_actors = [item["identity"] for item in read_template.get("providers", [])]
    compute_actors = [item["identity"] for service in config.get("services", [])
                      if str(service.get("name", "")).startswith("/AI/YOLO/")
                      for item in service.get("providers", [])]
    for service in repo_services:
        if service.get("name") in {prefix + operation for operation in
                                   ("STATUS", "MANIFEST", "FETCH")}:
            service["users"] = sorted(set(service.get("users", [])) |
                                      set(repo_actors) | set(compute_actors))
    for operation in ("STORE", "STORE_RANGE", "COMMIT_RANGES", "ABORT_RANGES",
                      "CAPABILITY", "FETCH_RANGE", "CACHE_STATUS"):
        write = operation in {"STORE", "STORE_RANGE", "COMMIT_RANGES", "ABORT_RANGES"}
        entry = dict(write_template if write else read_template)
        entry["name"] = prefix + operation
        entry["model"] = entry["name"]
        entry["users"] = sorted(set(entry.get("users", [])) | set(repo_actors) |
                                (set() if write else set(compute_actors)))
        repo_services.append(entry)
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
    is_ndnsf_extension = bool(extension_suffix) and path.name in (
        f"_ndnsf{extension_suffix}", f"_py_repoclient{extension_suffix}")
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
        runtime_inputs: list[Path] | None = None,
        naming_candidate_id: str | None = None) -> dict[str, Any]:
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
    if naming_candidate_id is not None:
        record["namingCandidateId"] = naming_candidate_id
    digest_payload = json.dumps(record, sort_keys=True, separators=(",", ":"))
    record["candidateDigest"] = hashlib.sha256(digest_payload.encode()).hexdigest()
    return record


def write_candidate(path: Path, candidate: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(candidate, stream, indent=2, sort_keys=True)
        stream.write("\n")
