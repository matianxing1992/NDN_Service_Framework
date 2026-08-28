#!/usr/bin/env python3
"""Prepare model-mode inputs, or prove that a control-plane canary needs none."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def _sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _valid_request_id(value: str) -> bool:
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")
    return bool(value) and all(character in allowed for character in value)


def prepare(args: argparse.Namespace) -> None:
    if not _valid_request_id(args.request_id):
        raise RuntimeError("SPEC168_REQUEST_ID_INVALID")
    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    campaign = json.loads(args.campaign_manifest.read_text(encoding="utf-8"))
    bindings = campaign["bindingDigests"]

    if args.mode == "canary":
        if args.stage_manifest is not None or args.generation_template is not None:
            raise RuntimeError("SPEC168_CANARY_MODEL_INPUT_FORBIDDEN")
        (output / "canary-mode.txt").write_text(
            "mode=CONTROL_PLANE_CANARY modelWorkAllowed=false\n",
            encoding="utf-8",
        )
        return

    if args.stage_manifest is None or args.generation_template is None:
        raise RuntimeError("SPEC168_MODEL_INPUT_REQUIRED")
    stage_binding_key = str(args.stage_binding_key)
    if stage_binding_key not in {
        "remoteSmallStageManifestDigest",
        "remoteLargeStageManifestDigest",
    }:
        raise RuntimeError("SPEC168_STAGE_BINDING_KEY_INVALID")
    if _sha256(args.stage_manifest) != bindings[stage_binding_key]:
        raise RuntimeError("SPEC168_STAGE_BINDING_MISMATCH")
    stage = json.loads(args.stage_manifest.read_text(encoding="utf-8"))
    template = json.loads(args.generation_template.read_text(encoding="utf-8"))
    if len(template.get("prompts", [])) != 1:
        raise RuntimeError("SPEC168_SINGLE_CONTROL_PROMPT_REQUIRED")
    template["campaignId"] = (
        "spec168-control-" + campaign["campaignId"].rsplit("-", 1)[-1]
    )
    template["repetitions"] = {"warmupPerPrompt": 0, "measuredPerPrompt": 1}
    template["generation"]["maxNewTokens"] = 64
    template["generation"]["requireEos"] = False
    (output / "generation-campaign.json").write_text(
        json.dumps(template, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (output / "model-identity.digest").write_text(
        str(stage["modelDigest"]) + "\n", encoding="utf-8"
    )
    (output / "workload.digest").write_text(
        str(bindings["promptSetDigest"]) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("model", "canary"), required=True)
    parser.add_argument("--campaign-manifest", type=Path, required=True)
    parser.add_argument("--request-id", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--stage-manifest", type=Path)
    parser.add_argument("--generation-template", type=Path)
    parser.add_argument(
        "--stage-binding-key",
        default="remoteSmallStageManifestDigest",
        choices=("remoteSmallStageManifestDigest", "remoteLargeStageManifestDigest"),
    )
    args = parser.parse_args()
    try:
        prepare(args)
    except (KeyError, OSError, TypeError, ValueError, RuntimeError) as error:
        raise SystemExit(str(error)) from error
    print(f"SPEC168_SINGLE_CAMPAIGN_PREPARE_PASS mode={args.mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
