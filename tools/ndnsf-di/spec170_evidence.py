"""Deterministic evidence identities for Spec 170 gates.

This module deliberately has no experiment/runtime dependencies.  It is the
small, reviewable boundary used by local and TigerCluster gate runners to
bind a result to one source/model/artifact/workload candidate.  A frozen
candidate is immutable by digest: changing any executable, image, SIF,
dependency, model, route, schedule, prompt, or security input produces an
``INVALID_CANDIDATE`` identity instead of silently mixing evidence.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import copy
import hashlib
import json
from pathlib import Path
from typing import Any, Mapping


SCHEMA_VERSION = "spec170-evidence-v1"
REQUIRED_CANDIDATE_SECTIONS = (
    "source",
    "oci",
    "sif",
    "dependencyLock",
    "model",
    "canonicalArtifact",
    "promptCorpus",
    "security",
    "route",
    "schedule",
    "freezeTimestamp",
)


def canonical_json(value: Any) -> str:
    """Return the single JSON representation used in all evidence digests."""

    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False)


def sha256_digest(value: Any) -> str:
    if isinstance(value, bytes):
        payload = value
    else:
        payload = canonical_json(value).encode("utf-8")
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def _plain(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {str(k): _plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_plain(item) for item in value]
    if isinstance(value, Path):
        return str(value)
    return value


def _require_non_empty_mapping(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, Mapping) or not value:
        raise ValueError(f"{name} must be a non-empty mapping")
    return dict(_plain(value))


@dataclass(frozen=True)
class EvidenceRow:
    """One complete or negative observation row.

    Negative rows are first-class evidence; they must not be discarded when a
    later retry succeeds.
    """

    row_id: str
    status: str
    payload: Mapping[str, Any] = field(default_factory=dict)
    reason: str = ""

    def __post_init__(self) -> None:
        if not self.row_id:
            raise ValueError("evidence row_id is required")
        if self.status not in {"COMPLETE", "NEGATIVE", "INVALID_CANDIDATE"}:
            raise ValueError("evidence row status is not classified")
        if not isinstance(self.payload, Mapping):
            raise ValueError("evidence row payload must be a mapping")

    def to_dict(self) -> dict[str, Any]:
        return {
            "rowId": self.row_id,
            "status": self.status,
            "payload": _plain(self.payload),
            "reason": self.reason,
        }


@dataclass(frozen=True)
class CandidateIdentity:
    """All immutable inputs which define a Spec 170 candidate."""

    source: Mapping[str, Any]
    oci: Mapping[str, Any]
    sif: Mapping[str, Any]
    dependency_lock: Mapping[str, Any]
    model: Mapping[str, Any]
    canonical_artifact: Mapping[str, Any]
    prompt_corpus: Mapping[str, Any]
    security: Mapping[str, Any]
    route: Mapping[str, Any]
    schedule: Mapping[str, Any]
    freeze_timestamp: str
    schema_version: str = SCHEMA_VERSION

    def __post_init__(self) -> None:
        if self.schema_version != SCHEMA_VERSION:
            raise ValueError("unsupported Spec 170 evidence schema")
        for name in REQUIRED_CANDIDATE_SECTIONS[:-1]:
            _require_non_empty_mapping(getattr(self, self._snake(name)), name)
        if not self.freeze_timestamp:
            raise ValueError("freezeTimestamp is required")

    @staticmethod
    def _snake(name: str) -> str:
        return {
            "dependencyLock": "dependency_lock",
            "canonicalArtifact": "canonical_artifact",
            "promptCorpus": "prompt_corpus",
            "freezeTimestamp": "freeze_timestamp",
        }.get(name, name)

    def to_dict(self) -> dict[str, Any]:
        return {
            "schemaVersion": self.schema_version,
            "source": _plain(self.source),
            "oci": _plain(self.oci),
            "sif": _plain(self.sif),
            "dependencyLock": _plain(self.dependency_lock),
            "model": _plain(self.model),
            "canonicalArtifact": _plain(self.canonical_artifact),
            "promptCorpus": _plain(self.prompt_corpus),
            "security": _plain(self.security),
            "route": _plain(self.route),
            "schedule": _plain(self.schedule),
            "freezeTimestamp": self.freeze_timestamp,
        }

    @property
    def candidate_digest(self) -> str:
        return sha256_digest(self.to_dict())

    @property
    def candidate_id(self) -> str:
        return "candidate:" + self.candidate_digest[7:23]

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "CandidateIdentity":
        data = dict(value)
        if data.get("schemaVersion", SCHEMA_VERSION) != SCHEMA_VERSION:
            raise ValueError("unsupported Spec 170 evidence schema")
        missing = [name for name in REQUIRED_CANDIDATE_SECTIONS
                   if name not in data]
        if missing:
            raise ValueError("candidate missing sections: " + ",".join(missing))
        return cls(
            source=_require_non_empty_mapping(data["source"], "source"),
            oci=_require_non_empty_mapping(data["oci"], "oci"),
            sif=_require_non_empty_mapping(data["sif"], "sif"),
            dependency_lock=_require_non_empty_mapping(
                data["dependencyLock"], "dependencyLock"),
            model=_require_non_empty_mapping(data["model"], "model"),
            canonical_artifact=_require_non_empty_mapping(
                data["canonicalArtifact"], "canonicalArtifact"),
            prompt_corpus=_require_non_empty_mapping(
                data["promptCorpus"], "promptCorpus"),
            security=_require_non_empty_mapping(data["security"], "security"),
            route=_require_non_empty_mapping(data["route"], "route"),
            schedule=_require_non_empty_mapping(data["schedule"], "schedule"),
            freeze_timestamp=str(data["freezeTimestamp"]),
            schema_version=str(data.get("schemaVersion", SCHEMA_VERSION)),
        )


@dataclass(frozen=True)
class RunIdentity:
    candidate_id: str
    gate: str
    run_id: str
    started_at: str
    topology: Mapping[str, Any]

    def __post_init__(self) -> None:
        for name in ("candidate_id", "gate", "run_id", "started_at"):
            if not getattr(self, name):
                raise ValueError(f"{name} is required")
        if not isinstance(self.topology, Mapping) or not self.topology:
            raise ValueError("topology must be a non-empty mapping")

    def to_dict(self) -> dict[str, Any]:
        return {
            "candidateId": self.candidate_id,
            "gate": self.gate,
            "runId": self.run_id,
            "startedAt": self.started_at,
            "topology": _plain(self.topology),
        }

    @property
    def run_digest(self) -> str:
        return sha256_digest(self.to_dict())


@dataclass
class EvidenceBundle:
    """Mutable while collecting rows, immutable after ``freeze``."""

    candidate: CandidateIdentity
    run: RunIdentity
    complete_rows: list[EvidenceRow] = field(default_factory=list)
    negative_rows: list[EvidenceRow] = field(default_factory=list)
    _frozen_digest: str | None = field(default=None, init=False, repr=False)

    @classmethod
    def create(cls, candidate: CandidateIdentity, run: RunIdentity) -> "EvidenceBundle":
        if run.candidate_id != candidate.candidate_id:
            raise ValueError("run is bound to a different candidate")
        return cls(candidate, run)

    @property
    def frozen(self) -> bool:
        return self._frozen_digest is not None

    def _assert_mutable(self) -> None:
        if self.frozen:
            raise RuntimeError("frozen evidence bundle is immutable")

    def add_complete(self, row: EvidenceRow) -> None:
        self._assert_mutable()
        if row.status != "COMPLETE":
            raise ValueError("complete row must have COMPLETE status")
        self.complete_rows.append(row)

    def add_negative(self, row: EvidenceRow) -> None:
        self._assert_mutable()
        if row.status not in {"NEGATIVE", "INVALID_CANDIDATE"}:
            raise ValueError("negative row must be NEGATIVE or INVALID_CANDIDATE")
        self.negative_rows.append(row)

    def to_dict(self) -> dict[str, Any]:
        return {
            "schemaVersion": SCHEMA_VERSION,
            "candidate": self.candidate.to_dict(),
            "run": self.run.to_dict(),
            "completeRows": [row.to_dict() for row in self.complete_rows],
            "negativeRows": [row.to_dict() for row in self.negative_rows],
            "frozenDigest": self._frozen_digest,
        }

    @property
    def digest(self) -> str:
        value = copy.deepcopy(self.to_dict())
        value["frozenDigest"] = None
        return sha256_digest(value)

    def freeze(self) -> str:
        if self.frozen:
            return str(self._frozen_digest)
        self._frozen_digest = self.digest
        return str(self._frozen_digest)

    def assert_integrity(self) -> None:
        if not self.frozen:
            return
        expected = self._frozen_digest
        self._frozen_digest = None
        try:
            actual = self.digest
        finally:
            self._frozen_digest = expected
        if actual != expected:
            raise ValueError("INVALID_CANDIDATE: frozen evidence digest changed")

    def to_json(self) -> str:
        self.assert_integrity()
        return canonical_json(self.to_dict())

    def write(self, path: str | Path) -> None:
        self.assert_integrity()
        Path(path).write_text(self.to_json() + "\n", encoding="utf-8")

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "EvidenceBundle":
        if value.get("schemaVersion") != SCHEMA_VERSION:
            raise ValueError("unsupported Spec 170 evidence schema")
        candidate = CandidateIdentity.from_dict(value["candidate"])
        run = RunIdentity(
            candidate_id=str(value["run"]["candidateId"]),
            gate=str(value["run"]["gate"]),
            run_id=str(value["run"]["runId"]),
            started_at=str(value["run"]["startedAt"]),
            topology=dict(value["run"]["topology"]),
        )
        bundle = cls.create(candidate, run)
        for raw in value.get("completeRows", ()):
            bundle.complete_rows.append(EvidenceRow(
                str(raw["rowId"]), str(raw["status"]),
                dict(raw.get("payload", {})), str(raw.get("reason", ""))))
        for raw in value.get("negativeRows", ()):
            bundle.negative_rows.append(EvidenceRow(
                str(raw["rowId"]), str(raw["status"]),
                dict(raw.get("payload", {})), str(raw.get("reason", ""))))
        frozen = value.get("frozenDigest")
        if frozen:
            bundle._frozen_digest = str(frozen)
            bundle.assert_integrity()
        return bundle


def invalid_candidate_identity(expected: str, observed: str) -> str:
    """Return a stable identity for a rejected post-freeze candidate."""

    return "INVALID_CANDIDATE:" + sha256_digest({
        "expected": str(expected), "observed": str(observed),
    })[7:]


__all__ = [
    "SCHEMA_VERSION", "CandidateIdentity", "EvidenceBundle", "EvidenceRow",
    "RunIdentity", "canonical_json", "invalid_candidate_identity",
    "sha256_digest",
]
