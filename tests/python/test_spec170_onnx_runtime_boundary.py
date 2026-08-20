from __future__ import annotations

import ast
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest

import numpy as np


ROOT = Path(__file__).resolve().parents[2]
DI_ROOT = ROOT / "NDNSF-DistributedInference"
sys.path.insert(0, str(DI_ROOT))

from ndnsf_distributed_inference.adapters.qwen import (  # noqa: E402
    QWEN36_STAGE_ROLES,
    StandaloneQwenTokenizer,
    build_qwen_three_stage_adapter,
)


FORBIDDEN = {"torch", "transformers"}
RUNTIME_SOURCE_ROOTS = (
    DI_ROOT / "ndnsf_distributed_inference/core",
    DI_ROOT / "ndnsf_distributed_inference/sdk",
    DI_ROOT / "ndnsf_distributed_inference/app_sdk",
    DI_ROOT / "ndnsf_distributed_inference/adapters/onnx",
    DI_ROOT / "ndnsf_distributed_inference/adapters/qwen",
)


def _digest(token: str) -> str:
    import hashlib
    return "sha256:" + hashlib.sha256(token.encode()).hexdigest()


class Spec170OnnxRuntimeBoundaryTest(unittest.TestCase):
    def test_qwen_deployment_adapter_is_onnx_runtime_only(self):
        artifacts = {role: _digest(role) for role in QWEN36_STAGE_ROLES}
        weights = {role: 1024 for role in QWEN36_STAGE_ROLES}
        adapter = build_qwen_three_stage_adapter(
            model_name="Qwen/Qwen3.6-27B", revision="sealed-onnx",
            layer_ranges=((0, 1), (1, 2), (2, 3)),
            artifact_digests_by_role=artifacts,
            weight_bytes_by_role=weights,
            precision="float32",
        )
        self.assertEqual(adapter.descriptor.model_formats, ("onnx",))
        self.assertEqual(
            adapter.descriptor.backends,
            ("onnxruntime-cuda", "onnxruntime-cpu"),
        )
        model = adapter.describe_model(
            "Qwen/Qwen3.6-27B", _digest("model"), _digest("semantics"),
            source_revision="sealed-onnx")
        graph = adapter.graph.inspect(model)
        candidate = adapter.splitter.enumerate_candidates(model, graph)[0]
        for requirement in candidate.requirements_by_role.values():
            self.assertEqual(
                requirement.backends,
                ("onnxruntime",),
            )

    def test_deployment_source_profiles_import_no_torch_or_transformers(self):
        violations = []
        for source_root in RUNTIME_SOURCE_ROOTS:
            for path in source_root.rglob("*.py"):
                tree = ast.parse(path.read_text(), filename=str(path))
                for node in ast.walk(tree):
                    names = []
                    if isinstance(node, ast.Import):
                        names = [item.name for item in node.names]
                    elif isinstance(node, ast.ImportFrom) and node.module:
                        names = [node.module]
                    for name in names:
                        if name.split(".", 1)[0] in FORBIDDEN:
                            violations.append(f"{path.relative_to(ROOT)}:{node.lineno}:{name}")
        self.assertEqual(violations, [])

    def test_container_locks_and_final_runtime_exclude_exporter_packages(self):
        locks = (
            ROOT / "packaging/ndnsf-di-container/oci/locks/gpu.lock",
            ROOT / "packaging/ndnsf-di-container/oci/layered/locks/ml-runtime.lock.json",
        )
        for path in locks:
            lock = json.loads(path.read_text())
            deployment = {name.lower() for name in lock["deploymentPythonPackages"]}
            exporter = {name.lower() for name in lock["offlineExporterPackages"]}
            self.assertTrue({"onnxruntime-gpu", "tokenizers"} <= deployment)
            self.assertTrue(FORBIDDEN.isdisjoint(deployment))
            self.assertEqual(exporter, FORBIDDEN)
        for path in (
            ROOT / "packaging/ndnsf-di-container/oci/Dockerfile.gpu",
            ROOT / "packaging/ndnsf-di-container/oci/layered/Dockerfile.ml",
        ):
            text = path.read_text()
            self.assertIn("COPY --from=", text)
            self.assertIn("/opt/runtime-venv /opt/venv", text)
            self.assertIn("! /opt/venv/bin/python -c 'import torch'", text)
            self.assertIn("! /opt/venv/bin/python -c 'import transformers'", text)

    def test_minimal_onnx_numerics_and_standalone_token_ids_match_reference(self):
        import onnx
        import onnxruntime as ort
        from onnx import TensorProto, helper, numpy_helper
        from tokenizers import Tokenizer
        from tokenizers.models import WordLevel
        from tokenizers.pre_tokenizers import Whitespace

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tokenizer_path = root / "tokenizer.json"
            tokenizer = Tokenizer(WordLevel(
                {"[UNK]": 0, "hello": 1, "world": 2},
                unk_token="[UNK]"))
            tokenizer.pre_tokenizer = Whitespace()
            tokenizer.save(str(tokenizer_path))
            runtime_tokenizer = StandaloneQwenTokenizer.from_file(tokenizer_path)
            self.assertEqual(runtime_tokenizer.encode(
                "hello world", add_special_tokens=False), (1, 2))
            self.assertEqual(runtime_tokenizer.decode((1, 2)), "hello world")

            weight = numpy_helper.from_array(
                np.asarray([[2.0]], dtype=np.float32), name="weight")
            graph = helper.make_graph(
                [helper.make_node("MatMul", ["x", "weight"], ["y"])],
                "runtime-boundary",
                [helper.make_tensor_value_info(
                    "x", TensorProto.FLOAT, [1, 1])],
                [helper.make_tensor_value_info(
                    "y", TensorProto.FLOAT, [1, 1])],
                [weight],
            )
            model = helper.make_model(
                graph, opset_imports=[helper.make_opsetid("", 13)])
            wire = model.SerializeToString(deterministic=True)
            observed = ort.InferenceSession(
                wire, providers=["CPUExecutionProvider"]).run(
                    None, {"x": np.asarray([[4.0]], dtype=np.float32)})[0]
            np.testing.assert_array_equal(
                observed, np.asarray([[8.0]], dtype=np.float32))

    def test_isolated_runtime_import_succeeds_when_forbidden_imports_raise(self):
        script = textwrap.dedent("""
            import builtins
            real_import = builtins.__import__
            def guarded(name, *args, **kwargs):
                if name.split('.', 1)[0] in {'torch', 'transformers'}:
                    raise AssertionError('forbidden deployment import:' + name)
                return real_import(name, *args, **kwargs)
            builtins.__import__ = guarded
            import onnxruntime
            import tokenizers
            from ndnsf_distributed_inference.adapters.onnx import executor
            from ndnsf_distributed_inference.adapters.qwen import StandaloneQwenTokenizer
            print('ONNX_RUNTIME_BOUNDARY_PASS')
        """)
        result = subprocess.run(
            [sys.executable, "-c", script],
            cwd=ROOT,
            env={
                "PATH": str(Path(sys.executable).parent),
                "PYTHONPATH": (
                    f"{DI_ROOT}:{ROOT / 'NDNSF-DistributedRepo/pythonWrapper'}"
                ),
            },
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ONNX_RUNTIME_BOUNDARY_PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
