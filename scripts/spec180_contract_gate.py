#!/usr/bin/env python3
"""Fail-closed document and manifest gate for Spec180.

This gate is intentionally small and dependency-light.  It is the first
promotion boundary for Spec180: it verifies repository-owned documents and the
two independent trust-root entries before an adapter can enumerate candidates
or a release tool can stage a model.  It does not build code, contact NDN,
upload bytes, or invoke a scheduler.
"""

from __future__ import annotations

import argparse
import base64
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
from typing import Any, Iterable, Mapping


SCHEMA = "spec180-contract-gate-v1"
FEATURE_BASENAME = "180-ack-driven-cross-model-qualification"
REGISTRY_NAME = "contracts/trust-root-registry-v1.json"
_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_FR_RE = re.compile(r"^- \*\*(FR-\d{3})\*\*")
_SC_RE = re.compile(r"^- \*\*(SC-\d{3})\*\*\.")
_TASK_RE = re.compile(r"^- \[([ Xx])\] (T\d{3})\b")


@dataclass(frozen=True)
class GateIssue:
    code: str
    detail: str
    path: str = ""

    def to_dict(self) -> dict[str, str]:
        value = {"code": self.code, "detail": self.detail}
        if self.path:
            value["path"] = self.path
        return value


def _issue(code: str, detail: str, path: Path | str = "") -> GateIssue:
    return GateIssue(code, detail, str(path))


def canonical_bytes(value: Any) -> bytes:
    """Return the deterministic JSON view signed by Spec180 authorities."""
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
    ).encode("utf-8")


def sha256_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _read_json(path: Path) -> tuple[Mapping[str, Any] | None, list[GateIssue]]:
    if not path.is_file():
        return None, [_issue("MISSING_JSON", "required JSON file is missing", path)]
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return None, [_issue("INVALID_JSON", str(exc), path)]
    if not isinstance(value, Mapping):
        return None, [_issue("JSON_NOT_OBJECT", "JSON root must be an object", path)]
    return value, []


def _require_nonempty(mapping: Mapping[str, Any], fields: Iterable[str],
                      label: str, path: Path) -> list[GateIssue]:
    return [
        _issue("TRUST_ROOT_FIELD_MISSING", f"{label}.{field} is required", path)
        for field in fields
        if not isinstance(mapping.get(field), str) or not mapping[field].strip()
    ]


def _validate_trust_entry(entry: Any, label: str, feature_dir: Path) -> list[GateIssue]:
    path = feature_dir / REGISTRY_NAME
    if not isinstance(entry, Mapping):
        return [_issue("TRUST_ROOT_ENTRY_MISSING", f"{label} entry is missing", path)]
    issues = _require_nonempty(
        entry,
        ("authorityId", "keyId", "publicKeyAlgorithm", "signatureAlgorithm",
         "publicKeyPath", "publicKeySha256", "manifestSchema"),
        label,
        path,
    )
    if entry.get("publicKeyAlgorithm") != "ed25519" or entry.get("signatureAlgorithm") != "ed25519":
        issues.append(_issue(
            "TRUST_ROOT_ALGORITHM_UNSUPPORTED",
            f"{label} trust root must use ed25519 for both public-key and signature algorithms",
            path,
        ))
    digest = entry.get("publicKeySha256", "")
    if isinstance(digest, str) and digest and not _DIGEST_RE.fullmatch(digest):
        issues.append(_issue("TRUST_ROOT_DIGEST_INVALID",
                             f"{label}.publicKeySha256 is not sha256:<64 hex>", path))
    families = entry.get("acceptedModelFamilies")
    if not isinstance(families, list) or not families or not all(
            isinstance(item, str) and item for item in families):
        issues.append(_issue("TRUST_ROOT_FAMILIES_INVALID",
                             f"{label}.acceptedModelFamilies must be non-empty", path))
    public_key_path = entry.get("publicKeyPath", "")
    if isinstance(public_key_path, str) and public_key_path:
        resolved = (feature_dir / public_key_path).resolve()
        try:
            resolved.relative_to(feature_dir.resolve())
        except ValueError:
            issues.append(_issue("TRUST_ROOT_PATH_ESCAPE",
                                 f"{label}.publicKeyPath escapes feature directory", path))
        else:
            if not resolved.is_file():
                issues.append(_issue("TRUST_ROOT_PUBLIC_KEY_MISSING",
                                     f"{label}.publicKeyPath does not resolve to a file", path))
            else:
                try:
                    actual = hashlib.sha256(resolved.read_bytes()).hexdigest()
                except OSError as exc:
                    issues.append(_issue("TRUST_ROOT_PUBLIC_KEY_READ",
                                         f"{label}.publicKeyPath cannot be read: {exc}", path))
                else:
                    expected = str(entry.get("publicKeySha256", ""))[7:]
                    if expected and actual != expected:
                        issues.append(_issue("TRUST_ROOT_PUBLIC_KEY_DIGEST",
                                             f"{label}.publicKeyPath digest does not match registry", path))
    return issues


def load_trust_registry(feature_dir: Path) -> tuple[Mapping[str, Any] | None, list[GateIssue]]:
    path = feature_dir / REGISTRY_NAME
    registry, issues = _read_json(path)
    if registry is None:
        return None, issues
    if registry.get("schemaVersion") != 1:
        issues.append(_issue("TRUST_ROOT_SCHEMA_UNSUPPORTED",
                             "trust-root registry schemaVersion must be 1", path))
    status = registry.get("status")
    if status != "CONFIGURED":
        issues.append(_issue(
            "TRUST_ROOT_UNCONFIGURED",
            "trust-root registry must be CONFIGURED before T001 can pass",
            path,
        ))
    issues.extend(_validate_trust_entry(registry.get("catalogue"), "catalogue", feature_dir))
    issues.extend(_validate_trust_entry(
        registry.get("modelManifest"), "modelManifest", feature_dir))
    return registry, issues


def _signature_payload(manifest: Mapping[str, Any]) -> dict[str, Any]:
    """Remove only the detached signature envelope from a manifest."""
    payload = dict(manifest)
    payload.pop("signature", None)
    return payload


def verify_signed_manifest(
    manifest: Mapping[str, Any],
    trust_entry: Mapping[str, Any],
    *,
    feature_dir: Path,
    model_family: str | None = None,
) -> tuple[bool, list[GateIssue]]:
    """Verify an authority-signed manifest before enumeration/staging.

    The function is shared by T004/T005 and T012/T017.  It deliberately does
    not accept a public key or key ID from the manifest as authority; those
    values must match the fixed registry entry.
    """
    issues: list[GateIssue] = []
    if not isinstance(manifest, Mapping):
        return False, [_issue("MANIFEST_NOT_OBJECT", "manifest must be an object")]
    signature = manifest.get("signature")
    if not isinstance(signature, Mapping):
        return False, [_issue("MANIFEST_UNSIGNED",
                              "manifest has no detached signature envelope")]
    expected_key = trust_entry.get("keyId")
    expected_algorithm = trust_entry.get("signatureAlgorithm")
    if signature.get("keyId") != expected_key:
        issues.append(_issue("MANIFEST_UNKNOWN_KEY", "manifest keyId is not the registered key"))
    if signature.get("algorithm") != expected_algorithm:
        issues.append(_issue("MANIFEST_SIGNATURE_ALGORITHM",
                             "manifest signature algorithm is not registered"))
    if model_family is None:
        model_family = str(manifest.get("modelFamily", ""))
    families = trust_entry.get("acceptedModelFamilies", ())
    if model_family not in families:
        issues.append(_issue("MANIFEST_MODEL_FAMILY", "model family is not accepted by this authority"))
    signature_b64 = signature.get("valueB64")
    if not isinstance(signature_b64, str) or not signature_b64:
        issues.append(_issue("MANIFEST_SIGNATURE_MISSING", "signature value is missing"))
    public_key_path = trust_entry.get("publicKeyPath", "")
    key_path = (feature_dir / public_key_path).resolve() if isinstance(public_key_path, str) else None
    if key_path is None or not key_path.is_file():
        issues.append(_issue("TRUST_ROOT_PUBLIC_KEY_MISSING",
                             "registered public-key file is missing"))
    if issues:
        return False, issues
    try:
        encoded = base64.b64decode(signature_b64, validate=True)
        public_bytes = key_path.read_bytes()
        if hashlib.sha256(public_bytes).hexdigest() != str(
                trust_entry["publicKeySha256"])[7:]:
            return False, [_issue("TRUST_ROOT_PUBLIC_KEY_DIGEST",
                                  "registered public-key digest does not match file")]
        from cryptography.hazmat.primitives import serialization
        try:
            from cryptography.hazmat.backends import default_backend
        except ImportError:  # pragma: no cover - modern cryptography
            default_backend = None
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
        try:
            if default_backend is None:
                public_key = serialization.load_pem_public_key(public_bytes)
            else:
                public_key = serialization.load_pem_public_key(
                    public_bytes, backend=default_backend())
        except ValueError:
            public_key = Ed25519PublicKey.from_public_bytes(public_bytes)
        public_key.verify(encoded, canonical_bytes(_signature_payload(manifest)))
    except Exception as exc:  # cryptographic backends expose several exception types
        return False, [_issue("MANIFEST_SIGNATURE_INVALID", str(exc))]
    return True, []


def _ids_from_lines(text: str, pattern: re.Pattern[str], group: int = 1) -> list[str]:
    return [match.group(group) for line in text.splitlines()
            if (match := pattern.match(line))]


def _unique_issues(ids: list[str], label: str, path: Path) -> list[GateIssue]:
    duplicates = sorted({value for value in ids if ids.count(value) > 1})
    return [_issue(f"DUPLICATE_{label}_ID", ", ".join(duplicates), path)] if duplicates else []


def audit_documents(project_root: Path, feature_dir: Path) -> tuple[dict[str, Any], list[GateIssue]]:
    issues: list[GateIssue] = []
    spec_path = feature_dir / "spec.md"
    tasks_path = feature_dir / "tasks.md"
    trace_path = feature_dir / "traceability.md"
    for path in (spec_path, feature_dir / "plan.md", tasks_path, trace_path):
        if not path.is_file():
            issues.append(_issue("MISSING_FEATURE_DOCUMENT", "required feature document is missing", path))
    if not spec_path.is_file() or not tasks_path.is_file() or not trace_path.is_file():
        return {}, issues
    spec = spec_path.read_text(encoding="utf-8")
    tasks = tasks_path.read_text(encoding="utf-8")
    trace = trace_path.read_text(encoding="utf-8")
    fr = _ids_from_lines(spec, _FR_RE)
    sc = _ids_from_lines(spec, _SC_RE)
    task_ids = _ids_from_lines(tasks, _TASK_RE, group=2)
    issues.extend(_unique_issues(fr, "FR", spec_path))
    issues.extend(_unique_issues(sc, "SC", spec_path))
    issues.extend(_unique_issues(task_ids, "TASK", tasks_path))
    if sorted(task_ids) != [f"T{index:03d}" for index in range(1, 21)]:
        issues.append(_issue("TASK_REGISTRY_INCOMPLETE",
                             "Spec180 task registry must contain T001..T020 exactly", tasks_path))
    missing_trace = [item for item in fr + sc if f"{item}" not in trace]
    if missing_trace:
        issues.append(_issue("TRACEABILITY_MISSING", ", ".join(missing_trace), trace_path))

    handoff = project_root / "specs/175-ndnsf-di-streamed-invocation/handoff-to-spec180.md"
    closure = project_root / "specs/175-ndnsf-di-streamed-invocation/evidence/local-closure-current.md"
    if not handoff.is_file() or not closure.is_file():
        issues.append(_issue("SPEC175_HANDOFF_MISSING",
                             "sealed Spec175 handoff and current closure are required", handoff))
    else:
        handoff_text = handoff.read_text(encoding="utf-8")
        closure_text = closure.read_text(encoding="utf-8")
        if "LOCAL_FUNCTIONAL_PASS" not in handoff_text or "LOCAL_FUNCTIONAL_PASS" not in closure_text:
            issues.append(_issue("SPEC175_HANDOFF_NOT_PASS",
                                 "Spec175 handoff must report LOCAL_FUNCTIONAL_PASS", handoff))
        if not re.search(r"source revision\s*`[0-9a-f]{40}`", handoff_text):
            issues.append(_issue("SPEC175_SOURCE_REVISION_MISSING",
                                 "Spec175 handoff must name a source revision", handoff))

    owner_map_markers = ("## Source-owner status", "| existing |", "| planned |")
    for marker in owner_map_markers:
        if marker not in trace:
            issues.append(_issue("OWNER_MAP_INCOMPLETE",
                                 "traceability owner map lacks " + marker
                                 + " (文档语言策略：节标题与状态词必须保持英文原样，"
                                   "机器门禁不识别中文翻译；叙述正文可用中文，见 "
                                   "constitution 1.4.0 Document Language Policy)",
                                 trace_path))
    # Historical candidates may remain as evidence, but can never be promoted
    # as the current candidate identity.
    evidence_root = feature_dir / "evidence"
    if evidence_root.is_dir():
        for evidence_path in sorted(evidence_root.rglob("*.json")):
            try:
                evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                continue
            if not isinstance(evidence, Mapping) or evidence.get("historical") is not True:
                continue
            if evidence.get("current") is True or evidence.get("status") == "CURRENT":
                issues.append(_issue(
                    "HISTORICAL_CANDIDATE_CURRENT",
                    "historical evidence cannot be marked current",
                    evidence_path,
                ))
    return {
        "functionalRequirements": sorted(set(fr)),
        "successCriteria": sorted(set(sc)),
        "tasks": sorted(set(task_ids)),
        "traceabilityComplete": not any(item for item in fr + sc if item not in trace),
    }, issues


def _formal_qualification_ready(feature_dir: Path, *, contract_ok: bool) -> bool:
    """Return true only after the implementation/convergence gates are closed.

    This gate is intentionally also useful before implementation, so a clean
    document/trust-root result must not be confused with formal qualification
    readiness.  The latter requires every Spec180 task and a fresh audit PASS.
    """
    if not contract_ok:
        return False
    tasks_path = feature_dir / "tasks.md"
    audit_path = feature_dir / "audit.md"
    if not tasks_path.is_file() or not audit_path.is_file():
        return False
    task_ids = _ids_from_lines(
        tasks_path.read_text(encoding="utf-8"), _TASK_RE, group=2)
    task_states = {
        match.group(2): match.group(1).upper()
        for line in tasks_path.read_text(encoding="utf-8").splitlines()
        if (match := _TASK_RE.match(line))
    }
    if (task_ids != [f"T{index:03d}" for index in range(1, 21)]
            or any(task_states.get(task_id) != "X"
                   for task_id in task_ids)):
        return False
    audit = audit_path.read_text(encoding="utf-8")
    return bool(re.search(r"\*\*Verdict\*\*:\s*\*\*PASS\*\*", audit))


def run_gate(project_root: Path, feature_dir: Path) -> dict[str, Any]:
    registry, registry_issues = load_trust_registry(feature_dir)
    document_summary, document_issues = audit_documents(project_root, feature_dir)
    issues = registry_issues + document_issues
    contract_ok = not issues
    return {
        "schema": SCHEMA,
        "feature": FEATURE_BASENAME,
        "status": "PASS" if not issues else "BLOCKED",
        "contractReady": contract_ok,
        "qualificationReady": _formal_qualification_ready(
            feature_dir, contract_ok=contract_ok),
        "readinessScope": "DOCUMENT_AND_TRUST_ROOT_CONTRACT",
        "summary": document_summary,
        "trustRootStatus": registry.get("status") if registry else "MISSING",
        "issues": [item.to_dict() for item in issues],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--feature-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "specs" / FEATURE_BASENAME)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    result = run_gate(args.project_root.resolve(), args.feature_dir.resolve())
    payload = json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0 if result["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
