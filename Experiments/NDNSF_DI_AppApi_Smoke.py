#!/usr/bin/env python3
"""Smoke test for the NDNSF-DI application-facing service API."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
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
            "runtime": {
                "user_identity": "/example/di/users/alice",
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
                    "users": ["/example/di/users/alice"],
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

        bad_config = copy.deepcopy(config)
        bad_config["runtime"]["user_identity"] = "/example/di/users/bob"
        bad_config_path = root / "bad-policy.json"
        bad_config_path.write_text(json.dumps(bad_config), encoding="utf-8")
        try:
            load_or_generate_deployment(bad_config_path, root / "bad-generated")
            raise AssertionError("unauthorized runtime.user_identity was accepted")
        except ValueError as exc:
            assert "runtime.user_identity /example/di/users/bob" in str(exc)

        missing_role_config = copy.deepcopy(config)
        service_config = missing_role_config["services"][0]
        service_config["roles"] = ["/Stage/0", "/Stage/1"]
        service_config["dependencies"] = [
            {
                "producers": ["/Stage/0"],
                "consumers": ["/Stage/1"],
                "key_scope": "stage0-to-stage1",
                "topic_prefix": "/activation",
            }
        ]
        service_config["providers"][0]["roles"] = ["/Stage/0"]
        missing_role_path = root / "missing-role-policy.json"
        missing_role_path.write_text(json.dumps(missing_role_config), encoding="utf-8")
        try:
            load_or_generate_deployment(
                missing_role_path,
                root / "missing-role-generated",
            )
            raise AssertionError("provider role coverage gap was accepted")
        except ValueError as exc:
            assert "no authorized provider: /Stage/1" in str(exc)

        client = APPClient(deployment, client=None)
        policy_text = Path(deployment.policy_file).read_text(encoding="utf-8")
        assert "for /example/di/users/alice" in policy_text
        assert "for /example/di/provider/A" in policy_text
        assert "for-prefix" not in policy_text
        assert client.describe_input("/AI/Toy/Inference")["codec"] == "npz"
        assert client.describe_output("/AI/Toy/Inference")["codec"] == "bytes"
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

        summary = subprocess.run(
            [
                sys.executable,
                "-m",
                "ndnsf_distributed_inference.policy",
                "--config",
                str(config_path),
                "--out-dir",
                str(root / "summary-generated"),
                "--print-summary",
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        assert "User permissions" in summary
        assert "/example/di/users/alice" in summary
        assert "Provider permissions" in summary
        assert "/example/di/provider/A" in summary
        assert "Role coverage" in summary
        assert "/Stage/0" in summary
        assert "Artifact coverage" in summary
        assert "toy-stage0.onnx" in summary

    print("APP_API_SERVICE_PLAN_OK service=/AI/Toy/Inference roles=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
