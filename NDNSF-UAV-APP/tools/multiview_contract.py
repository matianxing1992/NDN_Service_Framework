"""Transport-neutral contract helpers for Spec 177.

The C++ runtime owns NDNSF transport and authorization.  This module owns the
small JSON contract used by the model adapter and local functional harness.
It deliberately carries exact named-Data references, never image bytes.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
from typing import Any, Iterable, Mapping


SCHEMA_JOB = "ndnsf-uav-multiview-job/v1"
SCHEMA_RESULT = "ndnsf-uav-multiview-result/v2"
LEGACY_SCHEMA_RESULT = "ndnsf-uav-multiview-result/v1"
TERMINAL_STATUSES = {
    "completed",
    "insufficient-views",
    "validation-failed",
    "correlation-failed",
    "unsupported-model-profile",
    "inference-failed",
    "annotation-publication-failed",
    "delivery-timeout",
    "cancelled",
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _nonempty(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


@dataclass(frozen=True)
class ViewReference:
    view_id: str
    producer_identity: str
    exact_data_name: str
    content_digest: str
    capture_time_ms: int
    target_id: str
    media_type: str = "image/png"
    viewpoint: str = ""
    nominal_distance_m: float | None = None

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "viewId": self.view_id,
            "producerIdentity": self.producer_identity,
            "exactDataName": self.exact_data_name,
            "contentDigest": self.content_digest,
            "captureTimeMs": self.capture_time_ms,
            "targetId": self.target_id,
            "mediaType": self.media_type,
        }
        if self.viewpoint:
            result["viewpoint"] = self.viewpoint
        if self.nominal_distance_m is not None:
            result["nominalDistanceM"] = self.nominal_distance_m
        return result

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "ViewReference":
        return cls(
            view_id=str(value.get("viewId", value.get("view_id", ""))),
            producer_identity=str(value.get("producerIdentity", value.get("producer_identity", ""))),
            exact_data_name=str(value.get("exactDataName", value.get("exact_data_name", ""))),
            content_digest=str(value.get("contentDigest", value.get("content_digest", ""))),
            capture_time_ms=int(value.get("captureTimeMs", value.get("capture_time_ms", 0))),
            target_id=str(value.get("targetId", value.get("target_id", ""))),
            media_type=str(value.get("mediaType", value.get("media_type", "image/png"))),
            viewpoint=str(value.get("viewpoint", "")),
            nominal_distance_m=(
                float(value["nominalDistanceM"])
                if value.get("nominalDistanceM") is not None
                else None
            ),
        )


@dataclass
class MultiViewJob:
    mission_session_id: str
    job_id: str
    attempt: int
    target_id: str
    capture_window_start_ms: int
    capture_window_end_ms: int
    views: list[ViewReference]
    minimum_views: int = 2
    minimum_distinct_producers: int = 2
    model_profile_id: str = "vehicle-mvcnn-v1"
    deadline_ms: int = 5000

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": SCHEMA_JOB,
            "missionSessionId": self.mission_session_id,
            "jobId": self.job_id,
            "attempt": self.attempt,
            "targetId": self.target_id,
            "captureWindow": {
                "startMs": self.capture_window_start_ms,
                "endMs": self.capture_window_end_ms,
            },
            "minimumViews": self.minimum_views,
            "minimumDistinctProducers": self.minimum_distinct_producers,
            "modelProfileId": self.model_profile_id,
            "deadlineMs": self.deadline_ms,
            "views": [view.to_dict() for view in self.views],
        }


def validate_view(view: ViewReference, root: Path | None = None) -> list[str]:
    errors: list[str] = []
    for field_name, value in (
        ("viewId", view.view_id),
        ("producerIdentity", view.producer_identity),
        ("exactDataName", view.exact_data_name),
        ("contentDigest", view.content_digest),
        ("targetId", view.target_id),
        ("mediaType", view.media_type),
    ):
        if not _nonempty(value):
            errors.append(f"{field_name} is empty")
    if not view.producer_identity.startswith("/"):
        errors.append("producerIdentity must be an NDN name")
    if not view.exact_data_name.startswith("/") or "://" in view.exact_data_name:
        errors.append("exactDataName must be an exact NDN name")
    if not view.content_digest.startswith("sha256:") or len(view.content_digest) != 71:
        errors.append("contentDigest must be a sha256 digest")
    if view.capture_time_ms < 0:
        errors.append("captureTimeMs must be non-negative")
    if root is not None:
        image = root / view.exact_data_name
        # Local fixture references are checked by validate_fixture_manifest,
        # not by this generic wire contract.
        if image.exists() and sha256_file(image) != view.content_digest.removeprefix("sha256:"):
            errors.append(f"content digest mismatch for {view.view_id}")
    return errors


def validate_job(job: MultiViewJob, profile: Mapping[str, Any] | None = None) -> list[str]:
    errors: list[str] = []
    for name, value in (
        ("missionSessionId", job.mission_session_id),
        ("jobId", job.job_id),
        ("targetId", job.target_id),
        ("modelProfileId", job.model_profile_id),
    ):
        if not _nonempty(value):
            errors.append(f"{name} is empty")
    if job.attempt < 1:
        errors.append("attempt must be positive")
    if job.capture_window_start_ms > job.capture_window_end_ms:
        errors.append("capture window is inverted")
    if job.deadline_ms <= 0:
        errors.append("deadlineMs must be positive")
    if not 2 <= job.minimum_views <= 6:
        errors.append("minimumViews must be in the supported 2-6 range")
    if not 1 <= job.minimum_distinct_producers <= job.minimum_views:
        errors.append("minimumDistinctProducers is outside the view policy")
    if len(job.views) > 6:
        errors.append("view count exceeds the bounded six-view policy")
    seen_view_ids: set[str] = set()
    names: set[str] = set()
    tuples: set[tuple[str, str, str]] = set()
    producers: set[str] = set()
    for view in job.views:
        errors.extend(f"{view.view_id}: {error}" for error in validate_view(view))
        if view.view_id in seen_view_ids:
            errors.append(f"duplicate viewId: {view.view_id}")
        seen_view_ids.add(view.view_id)
        if view.exact_data_name in names:
            errors.append(f"duplicate exactDataName: {view.exact_data_name}")
        names.add(view.exact_data_name)
        producers.add(view.producer_identity)
        key = (view.producer_identity, view.view_id, view.content_digest)
        if key in tuples:
            errors.append(f"duplicate producer/view/digest tuple: {view.view_id}")
        tuples.add(key)
        if view.target_id != job.target_id:
            errors.append(f"cross-target view: {view.view_id}")
        if not job.capture_window_start_ms <= view.capture_time_ms <= job.capture_window_end_ms:
            errors.append(f"view outside capture window: {view.view_id}")
    if profile is not None:
        if job.model_profile_id != profile.get("profile_id", profile.get("profileId")):
            errors.append("job model profile does not match registration")
        minimum = int(profile.get("minimum_views", profile.get("minimumViews", 2)))
        maximum = int(profile.get("maximum_views", profile.get("maximumViews", 6)))
        if job.minimum_views < minimum or len(job.views) > maximum:
            errors.append("job violates registered model view bounds")
    if len(job.views) < job.minimum_views:
        errors.append("view count is below minimumViews")
    if len(producers) < job.minimum_distinct_producers:
        errors.append("distinct producer count is below minimumDistinctProducers")
    return errors


def validate_fixture_manifest(manifest: Mapping[str, Any], fixture_root: Path) -> list[str]:
    errors: list[str] = []
    required = {"fixture_id", "target_id", "purpose", "scientific_accuracy_claim_allowed", "views"}
    errors.extend(f"missing manifest field: {name}" for name in sorted(required - set(manifest)))
    if manifest.get("purpose") != "functional-pipeline-testing-only":
        errors.append("fixture purpose must remain functional-pipeline-testing-only")
    if manifest.get("scientific_accuracy_claim_allowed") is not False:
        errors.append("generated fixture must not allow scientific accuracy claims")
    views = manifest.get("views")
    if not isinstance(views, list) or len(views) != 6:
        errors.append("fixture must declare exactly six views")
        return errors
    seen: set[str] = set()
    producers: set[str] = set()
    for entry in views:
        view_id = str(entry.get("view_id", ""))
        filename = str(entry.get("file", ""))
        if view_id in seen:
            errors.append(f"duplicate fixture view: {view_id}")
        seen.add(view_id)
        producer = str(entry.get("producer_identity", ""))
        producers.add(producer)
        path = fixture_root / filename
        if not path.is_file():
            errors.append(f"missing fixture image: {filename}")
            continue
        expected = str(entry.get("sha256", ""))
        actual = sha256_file(path)
        if expected != actual:
            errors.append(f"fixture hash mismatch: {filename}")
    if len(producers) < 2:
        errors.append("fixture must contain at least two producer identities")
    return errors


def load_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def dump_json(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def validate_result(result: Mapping[str, Any], job: MultiViewJob | None = None,
                    profile: Mapping[str, Any] | None = None) -> list[str]:
    """Validate compact worker output and annotation completeness."""
    errors: list[str] = []
    if result.get("schema") not in (SCHEMA_RESULT, LEGACY_SCHEMA_RESULT):
        errors.append("result schema mismatch")
    status = result.get("status")
    if status not in TERMINAL_STATUSES:
        errors.append("unknown terminal status")
    contributing = list(result.get("contributingViews", []))
    annotations = list(result.get("annotatedViews", []))
    if status == "completed":
        minimum = job.minimum_views if job else 2
        if len(set(contributing)) < minimum:
            errors.append("completed result has insufficient unique views")
        if len(annotations) != len(contributing):
            errors.append("completed result annotation count mismatch")
        if len({entry.get("viewId") for entry in annotations}) != len(annotations):
            errors.append("duplicate annotation view")
        for entry in annotations:
            for key in ("viewId", "sourceDigest", "exactDataName", "contentDigest", "signerIdentity"):
                if not _nonempty(entry.get(key)):
                    errors.append(f"annotation missing {key}")
            if not str(entry.get("exactDataName", "")).startswith("/"):
                errors.append("annotation name is not an NDN name")
    if job is not None:
        if result.get("jobId") != job.job_id or result.get("missionSessionId") != job.mission_session_id:
            errors.append("result/job lineage mismatch")
    if profile is not None and result.get("model", {}).get("profileId") != profile.get("profile_id"):
        errors.append("result model profile mismatch")
    return errors
