#!/usr/bin/env python3
"""Smoke test for the NDNSF-DI application-facing service API."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path

import numpy as np

from ndnsf_distributed_inference import (
    APPClient,
    ArtifactSpec,
    RuntimeSpec,
    load_npz_payload,
    load_or_generate_deployment,
)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="ndnsf-di-app-api-") as tmp:
        root = Path(tmp)
        model = root / "toy-stage0.onnx"
        model.write_bytes(b"toy onnx model shard")
        config = {
            "application": "app-api-smoke",
            "controller": "/example/di/controller",
            "group": "/example/di/group",
            "identities": {
                "user": "/example/di/user",
                "provider_prefix": "/example/di/provider",
            },
            "services": [
                {
                    "name": "/AI/Toy/Inference",
                    "model": "/Model/Toy/v1",
                    "roles": ["/Stage/0"],
                    "dependencies": [],
                    "input": {
                        "codec": "npz",
                        "fields": {
                            "images": {
                                "dtype": "float32",
                                "shape": [1, 3, 2, 2],
                            }
                        },
                    },
                    "output": {"codec": "bytes"},
                    "users": ["/example/di/user"],
                    "providers": [
                        {
                            "identity": "/example/di/provider/A",
                            "roles": "all",
                        }
                    ],
                    "artifacts": [
                        {
                            "role": "/Stage/0",
                            "path": str(model),
                            "artifact": "/Model/Toy/v1/Stage/0",
                            "filename": "toy-stage0.onnx",
                            "kind": "onnx-model",
                            "backend": "onnxruntime",
                        }
                    ],
                }
            ],
        }
        config_path = root / "policy.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        deployment = load_or_generate_deployment(config_path, root / "generated")
        client = APPClient(deployment, client=None)
        image = np.arange(12, dtype=np.float32).reshape(1, 3, 2, 2)
        encoded = client.encode_input("/AI/Toy/Inference", image)
        decoded = load_npz_payload(encoded)
        assert np.array_equal(decoded["images"], image)

        plan = client.service_plan("/AI/Toy/Inference")
        assert plan.service == "/AI/Toy/Inference"
        assert len(plan.roles) == 1
        assert plan.roles[0].role == "/Stage/0"
        assert plan.roles[0].allow_dynamic_provisioning
        assert plan.roles[0].model_artifact.payload == model.read_bytes()

        runtime = RuntimeSpec(
            name="/Runtime/Toy/OnnxRuntime",
            backend="onnxruntime",
            entrypoint="runner",
            artifact=ArtifactSpec(
                name="runner",
                payload=b"#!/usr/bin/env python3\n",
                filename="runner.py",
                kind="runtime-script",
                executable=True,
                cache_name="/Runtime/Toy/OnnxRuntime",
            ),
        )
        manifests = {
            "roles": {
                "/Stage/0": {
                    "model": {"objectName": "/repo/model/toy-stage0"},
                    "runner": {"objectName": "/repo/runtime/runner"},
                }
            }
        }
        repo_plan = client.service_plan(
            "/AI/Toy/Inference",
            runtime=runtime,
            repo_manifests=manifests,
        )
        role = repo_plan.roles[0]
        assert role.model_artifact.payload == b""
        assert role.model_artifact.repo_manifest["objectName"] == "/repo/model/toy-stage0"
        assert role.runtime.artifact is not None
        assert role.runtime.artifact.payload == b""
        assert role.runtime.artifact.repo_manifest["objectName"] == "/repo/runtime/runner"

    print("APP_API_SERVICE_PLAN_OK service=/AI/Toy/Inference roles=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
