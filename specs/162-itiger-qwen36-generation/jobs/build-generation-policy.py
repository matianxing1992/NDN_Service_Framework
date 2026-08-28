#!/usr/bin/env python3
"""Build a candidate-local Qwen policy with the current Repo services."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil


QWEN_SERVICE = "/AI/LLM/Pipeline/Fake"
QWEN_ROLES = tuple(f"/LLM/Pipeline/Stage/{index}" for index in range(3))
QWEN_CUT_EDGES = (
    "hidden-layer-20-to-21",
    "hidden-layer-41-to-42",
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _automatic_scope(index: int, producer: str, consumer: str,
                     tensor_edge: str) -> str:
    """Match app-sdk placement's request-scoped tensor key identity.

    The runtime plan derives each scope from the immutable RoleDependency
    tuple.  The provider policy is only a compatibility graph for the Core
    callback, so it must describe the same edge without reintroducing the old
    symbolic ``pipeline-stage-*`` names.
    """
    payload = json.dumps({
        "consumer": consumer,
        "producer": producer,
        "tensor_edges": [tensor_edge],
    }, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    return f"tensor-{index}-{hashlib.sha256(payload).hexdigest()[:16]}"


def _align_qwen_dynamic_dependencies(path: Path) -> None:
    """Align the provider-side fallback graph with deferred Qwen planning."""
    document = __import__("yaml").safe_load(path.read_text(encoding="utf-8"))
    changed = False
    for service in document.get("services", []):
        if str(service.get("name", "")) != QWEN_SERVICE:
            continue
        dependencies = []
        for index, tensor_edge in enumerate(QWEN_CUT_EDGES):
            producer = QWEN_ROLES[index]
            consumer = QWEN_ROLES[index + 1]
            dependencies.append({
                "producers": [producer],
                "consumers": [consumer],
                "key_scope": _automatic_scope(
                    index, producer, consumer, tensor_edge),
                "topic_prefix": "/activation",
                "required": True,
                "tensors": [tensor_edge],
                "object_name_template": "",
            })
        if service.get("dependencies") != dependencies:
            service["dependencies"] = dependencies
            changed = True
    if changed:
        path.write_text(
            __import__("yaml").safe_dump(document, sort_keys=False),
            encoding="utf-8",
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--provider-prefix", required=True)
    parser.add_argument("--repo-provider-prefix", required=True)
    args = parser.parse_args()
    if args.output.exists() or args.provenance.exists():
        raise FileExistsError("candidate policy output already exists")

    source_digest = sha256_file(args.input)
    shutil.copyfile(args.input, args.output)
    prepare_path = Path(__file__).with_name("prepare-qwen36.py")
    module_spec = importlib.util.spec_from_file_location(
        "spec162_prepare_policy", prepare_path)
    module = importlib.util.module_from_spec(module_spec)
    assert module_spec.loader is not None
    module_spec.loader.exec_module(module)
    module._add_distributed_repo_services(
        args.output,
        user=args.user,
        provider_prefix=args.provider_prefix,
        repo_provider_prefix=args.repo_provider_prefix,
    )
    _align_qwen_dynamic_dependencies(args.output)
    output_digest = sha256_file(args.output)
    provenance = {
        "schemaVersion": "spec162-candidate-policy-provenance-v1",
        "input": str(args.input),
        "inputSha256": source_digest,
        "output": str(args.output),
        "outputSha256": output_digest,
        "transform": (
            "_add_distributed_repo_services;"
            "_align_qwen_dynamic_dependencies"
        ),
    }
    args.provenance.write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        "SPEC162_CANDIDATE_POLICY_READY",
        f"inputSha256={source_digest}",
        f"outputSha256={output_digest}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
