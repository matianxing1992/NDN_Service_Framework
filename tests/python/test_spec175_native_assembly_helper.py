from __future__ import annotations

import hashlib
import json
from pathlib import Path
import os
import subprocess
import sys
import tempfile

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper
import onnxruntime as ort

ROOT = Path(__file__).resolve().parents[2]
DI_ROOT = ROOT / "NDNSF-DistributedInference"
REPO_WRAPPER = ROOT / "NDNSF-DistributedRepo/pythonWrapper"
sys.path.insert(0, str(DI_ROOT))

from ndnsf_distributed_inference.adapters.onnx.executor import (  # noqa: E402
    CertifiedOnnxAssemblyRecipe,
)
from ndnsf_distributed_inference.adapters.onnx.graph import (  # noqa: E402
    canonical_onnx_identity,
)
from ndnsf_distributed_inference.native_assembly_helper import _digest  # noqa: E402
from ndnsf_distributed_inference.sdk.placement import RoleAssemblySpec  # noqa: E402


def _sha256(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _plain(value):
    if isinstance(value, dict):
        return {str(key): _plain(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [_plain(item) for item in value]
    if hasattr(value, "items"):
        return {str(key): _plain(item) for key, item in value.items()}
    return value


def _canonical_model() -> bytes:
    weight = numpy_helper.from_array(
        np.asarray([[2.0]], dtype=np.float32), name="weight")
    bias = numpy_helper.from_array(
        np.asarray([[1.0]], dtype=np.float32), name="bias")
    graph = helper.make_graph(
        [helper.make_node("MatMul", ["x", "weight"], ["m"]),
         helper.make_node("Add", ["m", "bias"], ["y"])],
        "spec175-native-assembly",
        [helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1])],
        [helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1])],
        [weight, bias],
        value_info=[helper.make_tensor_value_info(
            "m", TensorProto.FLOAT, [1, 1])],
    )
    return helper.make_model(
        graph, opset_imports=[helper.make_opsetid("", 13)]
    ).SerializeToString(deterministic=True)


def _request(model: bytes, provider: str, *, identity=None) -> dict:
    if identity is None:
        with tempfile.NamedTemporaryFile(suffix=".onnx") as stream:
            stream.write(model)
            stream.flush()
            identity = canonical_onnx_identity(stream.name)
    recipe = CertifiedOnnxAssemblyRecipe(
        model_manifest_digest=_sha256(b"root-manifest"),
        artifact_profile_digest=_sha256(b"profile"),
        graph_digest=identity.graph_digest,
        canonical_initializer_digest=identity.normalized_initializer_content_digest,
        adapter_descriptor_digest=_sha256(b"adapter"),
        assembler_descriptor_digest=_sha256(b"assembler"),
        backend_abi="onnxruntime-1.19-cpu",
        role_kind="PIPELINE_RANGE",
        layer_begin=0,
        layer_end=1,
        node_indices=(0,),
        input_names=("x",),
        output_names=("m",),
        # NativeExecutionPlanJson stores shape dimensions as strings in the
        # C++ projection, even when the dimensions are concrete.
        expected_inputs=({"name": "x", "dtype": "float32", "shape": ["1", "1"]},),
        expected_outputs=({"name": "m", "dtype": "float32", "shape": ["1", "1"]},),
        precision="fp32",
        max_source_bytes=1024 * 1024,
        max_assembled_bytes=1024 * 1024,
        max_nodes=8,
    )
    role = RoleAssemblySpec(
        role="/LLM/Pipeline/Stage/0",
        rank=0,
        layer_begin=0,
        layer_end=1,
        recipe_digest=recipe.digest,
        artifact_digest=_sha256(b"assigned-root"),
        backend="onnxruntime-cpu",
        role_kind=recipe.role_kind,
        model_manifest_digest=recipe.model_manifest_digest,
        artifact_profile_digest=recipe.artifact_profile_digest,
        graph_digest=recipe.graph_digest,
        canonical_initializer_digest=recipe.canonical_initializer_digest,
        adapter_descriptor_digest=recipe.adapter_descriptor_digest,
        assembler_descriptor_digest=recipe.assembler_descriptor_digest,
        backend_abi=recipe.backend_abi,
        node_indices=recipe.node_indices,
        expected_inputs=recipe.expected_inputs,
        expected_outputs=recipe.expected_outputs,
        precision=recipe.precision,
        quantization=recipe.quantization,
        layout=recipe.layout,
        padding=recipe.padding,
        resource_envelope={
            "maxSourceBytes": recipe.max_source_bytes,
            "maxAssembledBytes": recipe.max_assembled_bytes,
            "maxNodes": recipe.max_nodes,
        },
    )
    role_payload = _plain(role.__dict__)
    role_payload["resource_envelope"] = {
        "maxSourceBytes": recipe.max_source_bytes,
        "maxAssembledBytes": recipe.max_assembled_bytes,
        "maxNodes": recipe.max_nodes,
    }
    recipe_payload = {
        "schema": recipe.schema,
        "model_manifest_digest": recipe.model_manifest_digest,
        "artifact_profile_digest": recipe.artifact_profile_digest,
        "graph_digest": recipe.graph_digest,
        "canonical_initializer_digest": recipe.canonical_initializer_digest,
        "adapter_descriptor_digest": recipe.adapter_descriptor_digest,
        "assembler_descriptor_digest": recipe.assembler_descriptor_digest,
        "backend_abi": recipe.backend_abi,
        "role_kind": recipe.role_kind,
        "layer_begin": recipe.layer_begin,
        "layer_end": recipe.layer_end,
        "node_indices": list(recipe.node_indices),
        "input_names": list(recipe.input_names),
        "output_names": list(recipe.output_names),
        "expected_inputs": _plain(recipe.expected_inputs),
        "expected_outputs": _plain(recipe.expected_outputs),
        "precision": recipe.precision,
        "quantization": recipe.quantization,
        "layout": recipe.layout,
        "padding": recipe.padding,
        "max_source_bytes": recipe.max_source_bytes,
        "max_assembled_bytes": recipe.max_assembled_bytes,
        "max_nodes": recipe.max_nodes,
    }
    return {
        "canonical_model_path": "",
        "model_name": "spec175-fixture",
        "model_digest": _sha256(b"model-identity"),
        "profile_digest": recipe.artifact_profile_digest,
        "provider": provider,
        # This is the exact snake-case wire shape emitted by
        # NativeCanonicalOnnxAssembler::assemblyRequestJson().
        "role_spec": role_payload,
        "recipe": recipe_payload,
    }


def _run_helper(model: bytes, provider: str, root: Path,
                initializer: bytes | None = None,
                identity=None) -> dict:
    source = root / "canonical.onnx"
    request = root / f"{provider.rsplit('/', 1)[-1]}.json"
    output = root / f"result-{provider.rsplit('/', 1)[-1]}"
    source.write_bytes(model)
    initializer_path = source.with_name("model.onnx.data")
    if initializer is not None:
        initializer_path.write_bytes(initializer)
    payload = _request(model, provider, identity=identity)
    payload["canonical_model_path"] = str(source)
    if initializer is not None:
        payload["canonical_initializer_path"] = str(initializer_path)
    request.write_text(json.dumps(payload, sort_keys=True), encoding="utf-8")
    env = os.environ.copy()
    env["PYTHONPATH"] = os.pathsep.join(
        item for item in (str(DI_ROOT), str(REPO_WRAPPER),
                          env.get("PYTHONPATH", "")) if item)
    completed = subprocess.run(
        [
            sys.executable,
            "-m",
            "ndnsf_distributed_inference.native_assembly_helper",
            "--input",
            str(request),
            "--output-dir",
            str(output),
        ],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        check=False,
        timeout=60,
    )
    assert completed.returncode == 0, completed.stderr
    result = json.loads(completed.stdout)
    assert result["schema"] == "ndnsf-di-native-assembly-result-v1"
    assert Path(result["model_path"]).is_file()
    assert Path(result["manifest_path"]).is_file()
    assert _digest(Path(result["model_path"]).read_bytes()) == result["model_digest"]
    assert _digest(Path(result["manifest_path"]).read_bytes()) == result["manifest_digest"]
    return result


def test_native_helper_assembles_one_two_and_four_provider_roles() -> None:
    model = _canonical_model()
    with tempfile.TemporaryDirectory(prefix="spec175-native-helper-") as directory:
        root = Path(directory)
        results = [
            _run_helper(model, f"/provider/p{index}", root)
            for index in (0, 1, 2, 3)
        ]
        assert len(results) == 4
        model_digests = {item["model_digest"] for item in results}
        assert len(model_digests) == 1
        for item in results:
            manifest = json.loads(Path(item["manifest_path"]).read_text())
            assert manifest["schema"] == "ndnsf-di-assembled-onnx-v1"
            assert manifest["onnxChecker"] == "PASS"
            assert manifest["onnxRuntimeLoad"] == "PENDING_NATIVE_PROVIDER"
            session = ort.InferenceSession(
                Path(item["model_path"]).read_bytes(),
                providers=["CPUExecutionProvider"],
            )
            output = session.run(None, {"x": np.asarray([[4.0]], dtype=np.float32)})[0]
            np.testing.assert_allclose(output, np.asarray([[8.0]], dtype=np.float32))


def test_native_helper_rejects_mutated_recipe_or_source() -> None:
    model = _canonical_model()
    with tempfile.TemporaryDirectory(prefix="spec175-native-helper-mutation-") as directory:
        root = Path(directory)
        source = root / "canonical.onnx"
        source.write_bytes(model)
        payload = _request(model, "/provider/p0")
        payload["canonical_model_path"] = str(source)
        payload["role_spec"]["graph_digest"] = _sha256(b"tampered-graph")
        request = root / "tampered.json"
        request.write_text(json.dumps(payload), encoding="utf-8")
        output = root / "result"
        env = os.environ.copy()
        env["PYTHONPATH"] = os.pathsep.join(
            item for item in (str(DI_ROOT), str(REPO_WRAPPER),
                              env.get("PYTHONPATH", "")) if item)
        completed = subprocess.run(
            [sys.executable, "-m", "ndnsf_distributed_inference.native_assembly_helper",
             "--input", str(request), "--output-dir", str(output)],
            cwd=ROOT, env=env, capture_output=True, text=True, check=False,
            timeout=60,
        )
        assert completed.returncode != 0
        assert not output.exists() or not any(output.iterdir())


def _external_canonical_model() -> tuple[bytes, bytes, object]:
    model = onnx.load_model_from_string(_canonical_model())
    with tempfile.TemporaryDirectory(prefix="spec175-external-model-") as directory:
        root = Path(directory)
        source_path = root / "model.onnx"
        onnx.save_model(
            model,
            str(source_path),
            save_as_external_data=True,
            all_tensors_to_one_file=True,
            location="yolo26n.weights",
            size_threshold=0,
        )
        source = source_path.read_bytes()
        initializer = (root / "yolo26n.weights").read_bytes()
        identity = canonical_onnx_identity(source_path)
    return source, initializer, identity


def test_native_helper_requires_and_assembles_external_initializer() -> None:
    model, initializer, identity = _external_canonical_model()
    with tempfile.TemporaryDirectory(prefix="spec175-native-external-") as directory:
        root = Path(directory)
        result = _run_helper(
            model, "/provider/external", root,
            initializer=initializer, identity=identity)
        assembled = onnx.load(result["model_path"], load_external_data=False)
        assert {item.name for item in assembled.graph.initializer} == {"weight"}
        session = ort.InferenceSession(
            Path(result["model_path"]).read_bytes(),
            providers=["CPUExecutionProvider"],
        )
        output = session.run(None, {"x": np.asarray([[4.0]], dtype=np.float32)})[0]
        np.testing.assert_allclose(output, np.asarray([[8.0]], dtype=np.float32))

        source = root / "canonical.onnx"
        request = root / "missing-initializer.json"
        payload = _request(model, "/provider/missing", identity=identity)
        payload["canonical_model_path"] = str(source)
        request.write_text(json.dumps(payload), encoding="utf-8")
        output_dir = root / "missing-result"
        env = os.environ.copy()
        env["PYTHONPATH"] = os.pathsep.join(
            item for item in (str(DI_ROOT), str(REPO_WRAPPER),
                              env.get("PYTHONPATH", "")) if item)
        completed = subprocess.run(
            [sys.executable, "-m", "ndnsf_distributed_inference.native_assembly_helper",
             "--input", str(request), "--output-dir", str(output_dir)],
            cwd=ROOT, env=env, capture_output=True, text=True, check=False,
            timeout=60,
        )
        assert completed.returncode != 0
        assert "external initializer bytes are required" in completed.stderr
