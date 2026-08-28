#!/usr/bin/env python3
"""Create or reuse the bounded content-addressed Spec 168 Qwen3 fixture."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[3]
PIPELINE_DIR = (
    ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline")
sys.path.insert(0, str(PIPELINE_DIR))

from llm_pipeline_lib import (  # noqa: E402
    encode_qwen_input_ids,
    qwen_transformer_model_from_stage_package,
    role_name,
    run_qwen_transformer_stage,
    split_layer_ranges,
    write_tiny_qwen3_transformer_stage_artifacts,
)


PROFILE = "spec168-tiny-qwen3-v1"
MODEL_NAME = "NDNSF/TinyQwen3-Fixture"
REVISION = "seed-168-config-v1"
INPUT_IDS = [151644, 872, 198, 151645]


def canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False,
    ).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fixture_config(tokenizer_digest: str) -> dict[str, object]:
    return {
        "profile": PROFILE,
        "model": MODEL_NAME,
        "revision": REVISION,
        "seed": 168,
        "stages": 3,
        "layers": 3,
        "vocabSize": 151936,
        "hiddenSize": 64,
        "intermediateSize": 128,
        "attentionHeads": 4,
        "keyValueHeads": 2,
        "dtype": "float32",
        "tokenizerDigest": tokenizer_digest,
    }


def existing_bundle(artifact_root: Path, config_digest: str) -> Path | None:
    for manifest_path in sorted(
            (artifact_root / "sha256").glob("*/stage-manifest.json")):
        try:
            document = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError, json.JSONDecodeError):
            continue
        if (document.get("fixtureProfile") == PROFILE
                and document.get("fixtureConfigDigest") == config_digest):
            for stage in document.get("stages", ()):
                path = manifest_path.parent / "qwen-transformers-stage-artifacts" / str(
                    stage["filename"])
                if (not path.is_file()
                        or path.stat().st_size != int(stage["bytes"])
                        or sha256_file(path) != str(stage["sha256"])):
                    raise RuntimeError(
                        f"existing tiny-Qwen bundle is corrupt: {path}")
            return manifest_path.parent
    return None


def reference_tokens(artifacts, *, max_new_tokens: int = 4) -> list[int]:
    roles = [item.role for item in artifacts]
    models = [qwen_transformer_model_from_stage_package(
        item.path, device="cpu") for item in artifacts]
    context = list(INPUT_IDS)
    generated: list[int] = []
    for epoch in range(max_new_tokens):
        payload = encode_qwen_input_ids(
            [context], request_id=f"/spec168/tiny-qwen/reference/{epoch}")
        for role, model in zip(roles, models):
            payload = run_qwen_transformer_stage(
                payload, role=role, stages=3, model=model)
        token = int(json.loads(payload)["topToken"])
        generated.append(token)
        context.append(token)
    return generated


def create_bundle(
    artifact_root: Path,
    *,
    tokenizer_dir: Path,
    tokenizer_digest: str,
) -> tuple[Path, bool]:
    config = fixture_config(tokenizer_digest)
    config_digest = "sha256:" + sha256_bytes(canonical_bytes(config))
    artifact_root.mkdir(parents=True, exist_ok=True)
    reused = existing_bundle(artifact_root, config_digest)
    if reused is not None:
        return reused, True

    staging = Path(tempfile.mkdtemp(prefix=".tiny-qwen-", dir=artifact_root))
    try:
        roles = [role_name(index) for index in range(3)]
        artifacts = write_tiny_qwen3_transformer_stage_artifacts(
            staging, roles=roles, stages=3, layer_count=3,
        )
        ranges = split_layer_ranges(3, 3)
        stage_rows = []
        for index, artifact in enumerate(artifacts):
            path = Path(artifact.path)
            stage_rows.append({
                "bytes": path.stat().st_size,
                "cpuFallback": False,
                "cpuLogicValidated": True,
                "cudaValidated": False,
                "dtype": "float32",
                "filename": path.name,
                "forwardValidated": True,
                "layerCount": 3,
                "layerRange": {
                    "start": ranges[index][0],
                    "endExclusive": ranges[index][1],
                },
                "path": path.name,
                "role": artifact.role,
                "runtime": "qwen-transformers",
                "sha256": sha256_file(path),
                "stageCount": 3,
                "stageIndex": index,
            })
        if sum(int(item["bytes"]) for item in stage_rows) >= 128 * 1024 * 1024:
            raise RuntimeError("tiny-Qwen fixture exceeds the 128 MiB bundle bound")

        model_digest = "sha256:" + sha256_bytes(canonical_bytes({
            "fixtureConfigDigest": config_digest,
            "stageDigests": [item["sha256"] for item in stage_rows],
        }))
        manifest = {
            "schemaVersion": "ndnsf-di-qwen36-stage-manifest-v1",
            "fixtureProfile": PROFILE,
            "fixtureConfigDigest": config_digest,
            "modelDigest": model_digest,
            "modelProfile": "tiny-qwen3-local-logic",
            "repository": MODEL_NAME,
            "revision": REVISION,
            "dtype": "float32",
            "quantization": "none",
            "layerCount": 3,
            "layerRanges": [list(value) for value in ranges],
            "cpuFallbackAllowed": False,
            "localResourceContract": {
                "hostMemoryGiB": 8,
                "containerMemoryGiB": 6,
                "containerMemorySwapGiB": 7,
                "requireZeroCgroupOom": True,
            },
            "runtime": {"torch": "2.6.0+cu124", "transformers": "5.14.1"},
            "tokenizer": {
                "digest": tokenizer_digest,
                "path": str(tokenizer_dir),
            },
            "stages": stage_rows,
        }
        manifest_path = staging / "stage-manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        manifest_digest = sha256_file(manifest_path)
        generated = reference_tokens(artifacts)
        workload_digest = "sha256:" + sha256_bytes(canonical_bytes(INPUT_IDS))
        campaign = {
            "schemaVersion": "ndnsf-di-qwen-generation-campaign-v1",
            "campaignId": "spec168-tiny-qwen3-local-logic-v1",
            "candidateId": config_digest,
            "stageManifestSha256": "sha256:" + manifest_digest,
            "model": {
                "digest": model_digest,
                "repository": MODEL_NAME,
                "revision": REVISION,
                "dtype": "float32",
                "quantization": "none",
            },
            "generation": {
                "enableThinking": False,
                "maxNewTokens": 4,
                "requireEos": False,
                "strategy": "greedy",
                "useCache": False,
            },
            "repetitions": {
                "warmupPerPrompt": 0,
                "measuredPerPrompt": 1,
                "sequential": True,
            },
            "prompts": [{
                "promptId": "tiny-qwen-logic",
                "language": "token-fixture",
                "text": "Spec 168 bounded local Qwen lifecycle fixture",
                "formattedInputIds": list(INPUT_IDS),
                "formattedInputSha256": workload_digest[7:],
                "inputTokenCount": len(INPUT_IDS),
                "referenceGeneratedTokenIds": generated,
                "referenceGeneratedTokenSha256": sha256_bytes(
                    canonical_bytes(generated)),
                "referenceStopReason": "MAX_NEW_TOKENS",
                "eosTokenIds": [151643, 151645],
            }],
        }
        (staging / "smoke-campaign.json").write_text(
            json.dumps(campaign, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        destination = artifact_root / "sha256" / manifest_digest
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            raise RuntimeError(f"content-address destination exists: {destination}")
        os.replace(staging, destination)
        return destination, False
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-root", required=True)
    parser.add_argument("--tokenizer-dir", required=True)
    parser.add_argument("--tokenizer-digest", required=True)
    args = parser.parse_args()
    bundle, reused = create_bundle(
        Path(args.artifact_root).resolve(),
        tokenizer_dir=Path(args.tokenizer_dir).resolve(),
        tokenizer_digest=str(args.tokenizer_digest),
    )
    manifest = bundle / "stage-manifest.json"
    print(json.dumps({
        "bundle": str(bundle),
        "manifest": str(manifest),
        "manifestSha256": "sha256:" + sha256_file(manifest),
        "modelDigest": json.loads(manifest.read_text(encoding="utf-8"))[
            "modelDigest"],
        "reused": reused,
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
