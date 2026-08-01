"""Canonical, content-addressed workload description for Spec 165."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

QWEN3_MODEL_NAME = "Qwen/Qwen3-0.6B"
QWEN3_MODEL_REVISION = "e6de91484c29aa9480d55605af694f39b081c455"
_STANDARD_MODEL_SNAPSHOT = (
    Path.home()
    / ".cache/huggingface/hub/models--Qwen--Qwen3-0.6B"
    / "snapshots"
    / QWEN3_MODEL_REVISION
)
_SPEC163_MODEL_SNAPSHOT = (
    Path.home()
    / ".cache/ndnsf-spec163-hf/hub/models--Qwen--Qwen3-0.6B"
    / "snapshots"
    / QWEN3_MODEL_REVISION
)


def _readable_config(snapshot: Path) -> bool:
    try:
        return (snapshot / "config.json").is_file()
    except OSError:
        return False


DEFAULT_MODEL_SNAPSHOT = (
    _SPEC163_MODEL_SNAPSHOT
    if _readable_config(_SPEC163_MODEL_SNAPSHOT)
    else _STANDARD_MODEL_SNAPSHOT
)
DEFAULT_PROMPTS = (
    "Explain in two sentences why immutable model identity matters.",
    "Describe how progress-driven deadlines distinguish slow work from a stall.",
)


def canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def digest_value(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json_bytes(value)).hexdigest()


def snapshot_manifest(snapshot: Path) -> dict[str, Any]:
    snapshot = snapshot.resolve()
    if not snapshot.is_dir():
        raise FileNotFoundError(f"pinned model snapshot is absent: {snapshot}")
    files: list[dict[str, Any]] = []
    for path in sorted(item for item in snapshot.rglob("*") if item.is_file()):
        hasher = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                hasher.update(chunk)
        files.append(
            {
                "path": path.relative_to(snapshot).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": hasher.hexdigest(),
            }
        )
    if not files:
        raise ValueError(f"pinned model snapshot contains no files: {snapshot}")
    manifest = {
        "schemaVersion": 1,
        "name": QWEN3_MODEL_NAME,
        "revision": QWEN3_MODEL_REVISION,
        "files": files,
    }
    manifest["contentDigest"] = digest_value(manifest)
    return manifest


def canonical_workload(
    *,
    snapshot: Path = DEFAULT_MODEL_SNAPSHOT,
    backend: str = "cpu",
    prompts: tuple[str, ...] = DEFAULT_PROMPTS,
    warmup_per_prompt: int = 1,
    measured_per_prompt: int = 3,
    minimum_generated_tokens: int = 8,
    maximum_generated_tokens: int = 64,
    seed: int = 165,
    include_snapshot_manifest: bool = True,
) -> dict[str, Any]:
    if backend not in {"cpu", "cuda"}:
        raise ValueError("backend must be cpu or cuda")
    if len(prompts) < 2 or any(not prompt.strip() for prompt in prompts):
        raise ValueError("at least two non-empty prompts are required")
    if warmup_per_prompt < 1 or measured_per_prompt < 3:
        raise ValueError("workload requires >=1 warmup and >=3 measured requests")
    if minimum_generated_tokens < 8:
        raise ValueError("minimum generated-token threshold must be >=8")
    if maximum_generated_tokens < minimum_generated_tokens:
        raise ValueError("maximum generated tokens must cover the minimum")

    model_manifest = (
        snapshot_manifest(snapshot)
        if include_snapshot_manifest
        else {
            "schemaVersion": 1,
            "name": QWEN3_MODEL_NAME,
            "revision": QWEN3_MODEL_REVISION,
            "contentDigest": "unresolved",
        }
    )
    model_identity = {
        "name": QWEN3_MODEL_NAME,
        "revision": QWEN3_MODEL_REVISION,
        "contentDigest": model_manifest["contentDigest"],
        "localSnapshot": str(snapshot.resolve()),
    }
    body: dict[str, Any] = {
        "schemaVersion": 1,
        "workloadId": "spec165-qwen3-minimum",
        "modelIdentity": model_identity,
        "prompts": [
            {"promptId": f"prompt-{index + 1}", "text": prompt}
            for index, prompt in enumerate(prompts)
        ],
        "warmupPerPrompt": warmup_per_prompt,
        "measuredPerPrompt": measured_per_prompt,
        "minimumGeneratedTokens": minimum_generated_tokens,
        "maximumGeneratedTokens": maximum_generated_tokens,
        "seed": seed,
        "requestedBackend": backend,
        "fallbackPolicy": "forbid",
    }
    body["workloadDigest"] = digest_value(body)
    body["modelManifest"] = model_manifest
    return body


def write_workload(path: Path, workload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(workload, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
