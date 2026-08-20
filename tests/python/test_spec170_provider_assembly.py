from __future__ import annotations

from pathlib import Path
from dataclasses import replace
import hashlib
import sys
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.app_sdk.canonical_artifacts import (  # noqa: E402
    AssembledOnnxArtifactV1,
)
from ndnsf_distributed_inference.artifact_deployment import (  # noqa: E402
    assemble_onnx_role, assemble_onnx_role_v3,
)
from ndnsf_distributed_inference.adapters.onnx.executor import (  # noqa: E402
    CertifiedOnnxAssemblyRecipe,
)
from ndnsf_distributed_inference.adapters.onnx.graph import (  # noqa: E402
    canonical_onnx_identity,
)
from ndnsf_distributed_inference.sdk.placement import RoleAssemblySpec  # noqa: E402


def _digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


class Spec170ProviderAssemblyTest(unittest.TestCase):
    def canonical_model(self, *, changed_weight=False):
        import onnx
        from onnx import TensorProto, helper, numpy_helper
        weight = numpy_helper.from_array(
            np.asarray([[3.0 if changed_weight else 2.0]], dtype=np.float32),
            name="weight")
        bias = numpy_helper.from_array(
            np.asarray([[1.0]], dtype=np.float32), name="bias")
        graph = helper.make_graph(
            [helper.make_node("MatMul", ["x", "weight"], ["m"]),
             helper.make_node("Add", ["m", "bias"], ["y"])],
            "provider-assembly",
            [helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1])],
            [helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1])],
            [weight, bias],
            value_info=[helper.make_tensor_value_info(
                "m", TensorProto.FLOAT, [1, 1])])
        return helper.make_model(
            graph, opset_imports=[helper.make_opsetid("", 13)]
        ).SerializeToString(deterministic=True)

    def recipe_and_role(self, model_bytes, *, output="m", expected_shape=(1, 1),
                        expected_dtype=1,
                        graph_digest="", initializer_digest="",
                        backend_abi="onnxruntime-1.26-cpu"):
        with tempfile.NamedTemporaryFile(suffix=".onnx") as stream:
            stream.write(model_bytes)
            stream.flush()
            identity = canonical_onnx_identity(stream.name)
        recipe = CertifiedOnnxAssemblyRecipe(
            model_manifest_digest=_digest(b"model-manifest"),
            artifact_profile_digest=_digest(b"profile"),
            graph_digest=graph_digest or identity.graph_digest,
            canonical_initializer_digest=(initializer_digest
                                          or identity.normalized_initializer_content_digest),
            adapter_descriptor_digest=_digest(b"adapter"),
            assembler_descriptor_digest=_digest(b"assembler"),
            backend_abi="onnxruntime-1.26-cpu", role_kind="PIPELINE_RANGE",
            layer_begin=0, layer_end=1, node_indices=(0,),
            input_names=("x",), output_names=(output,),
            expected_inputs=({"name": "x", "dtype": expected_dtype,
                              "shape": [1, 1]},),
            expected_outputs=({"name": output, "dtype": expected_dtype,
                               "shape": list(expected_shape)},),
            precision="fp32")
        role = RoleAssemblySpec(
            role="stage-0", rank=0, layer_begin=0, layer_end=1,
            recipe_digest=recipe.digest, artifact_digest=_digest(b"canonical-root"),
            backend="onnxruntime-cpu", role_kind="PIPELINE_RANGE",
            model_manifest_digest=recipe.model_manifest_digest,
            artifact_profile_digest=recipe.artifact_profile_digest,
            graph_digest=recipe.graph_digest,
            canonical_initializer_digest=recipe.canonical_initializer_digest,
            adapter_descriptor_digest=recipe.adapter_descriptor_digest,
            assembler_descriptor_digest=recipe.assembler_descriptor_digest,
            backend_abi=backend_abi,
            node_indices=recipe.node_indices,
            expected_inputs=recipe.expected_inputs,
            expected_outputs=recipe.expected_outputs,
            precision=recipe.precision,
            quantization=recipe.quantization,
            layout=recipe.layout,
            padding=recipe.padding,
            resource_envelope={"maxSourceBytes": recipe.max_source_bytes,
                               "maxAssembledBytes": recipe.max_assembled_bytes,
                               "maxNodes": recipe.max_nodes})
        return recipe, role

    def bundle(self, external: bool = False):
        entries = {"model.onnx": b"onnx-bytes"}
        if external:
            entries["model.onnx.data"] = b"external-data"
        entry_digests = {
            name: "sha256:" + hashlib.sha256(payload).hexdigest()
            for name, payload in entries.items()
        }
        return AssembledOnnxArtifactV1(
            manifest={
                "schema": "ndnsf-di-assembled-onnx-v1",
                "model": "Qwen/Qwen3-0.6B",
                "signer": "/provider/p0",
                "layout": "ONNX_EXTERNAL_DATA" if external else "INLINE_ONNX",
                "entryDigests": entry_digests,
            }, entries=entries, signer="/provider/p0", signature="signed")

    def test_deterministic_single_file_round_trip(self):
        original = self.bundle(external=True)
        wire = original.to_bytes()
        restored = AssembledOnnxArtifactV1.from_bytes(wire)
        self.assertEqual(restored.to_bytes(), wire)
        self.assertEqual(restored.object_digest, original.object_digest)
        restored.verify_provider("/provider/p0")
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "stage.ndnsf-onnx-artifact"
            self.assertEqual(original.write_atomic(target), original.object_digest)
            self.assertEqual(target.read_bytes(), wire)

    def test_unsafe_or_duplicate_entries_fail(self):
        with self.assertRaises(ValueError):
            AssembledOnnxArtifactV1(
                manifest={"signer": "/provider/p0"},
                entries={"../model.onnx": b"x"}, signer="/provider/p0",
                signature="signed")
        wire = self.bundle().to_bytes()
        with self.assertRaises(ValueError):
            AssembledOnnxArtifactV1.from_bytes(wire + b"trailing")

    def test_cross_provider_import_fails_closed(self):
        with self.assertRaises(ValueError):
            self.bundle().verify_provider("/provider/p1")

    def test_provider_assembly_is_atomic_and_content_addressed(self):
        payloads = {"/layer/0": b"layer-zero", "/layer/1": b"layer-one"}
        digests = {
            name: "sha256:" + hashlib.sha256(payload).hexdigest()
            for name, payload in payloads.items()
        }
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "stage.ndnsf-onnx-artifact"
            result = assemble_onnx_role(
                role="stage-0", model_name="Qwen/Qwen3-0.6B",
                model_digest="sha256:" + "a" * 64,
                profile="qwen-onnx-cpu", graph_digest="sha256:" + "b" * 64,
                layer_payloads=payloads, layer_digests=digests,
                recipe_digest="sha256:" + "c" * 64,
                provider="/provider/p0", signature="signed", output_path=target,
            )
            self.assertTrue(target.is_file())
            self.assertEqual(result.path, target)
            self.assertFalse(target.with_name(target.name + ".tmp").exists())
            restored = AssembledOnnxArtifactV1.from_bytes(target.read_bytes())
            restored.verify_provider("/provider/p0")

    def test_v3_provider_extracts_checks_loads_and_reuses_exact_role(self):
        import onnxruntime as ort
        model = self.canonical_model()
        recipe, role = self.recipe_and_role(model)
        with tempfile.TemporaryDirectory() as directory:
            first = assemble_onnx_role_v3(
                role_spec=role, recipe=recipe, canonical_model=model,
                model_name="fixture", model_digest=_digest(b"model"),
                profile_digest=recipe.artifact_profile_digest,
                provider="/provider/p0", signature="signed",
                cache_dir=directory)
            second = assemble_onnx_role_v3(
                role_spec=role, recipe=recipe, canonical_model=model,
                model_name="fixture", model_digest=_digest(b"model"),
                profile_digest=recipe.artifact_profile_digest,
                provider="/provider/p0", signature="signed",
                cache_dir=directory)
            self.assertEqual(first.path, second.path)
            self.assertEqual(first.object_digest, second.object_digest)
            restored = AssembledOnnxArtifactV1.from_bytes(first.path.read_bytes())
            session = ort.InferenceSession(
                restored.entries["model.onnx"], providers=["CPUExecutionProvider"])
            output = session.run(None, {"x": np.asarray([[4.0]], dtype=np.float32)})[0]
            np.testing.assert_allclose(output, np.asarray([[8.0]], dtype=np.float32))
            self.assertEqual(restored.manifest["onnxChecker"], "PASS")
            self.assertEqual(restored.manifest["onnxRuntimeLoad"], "PASS")

    def test_v3_provider_rejects_digest_shape_abi_and_uncertified_slice(self):
        model = self.canonical_model()
        recipe, role = self.recipe_and_role(model)
        with tempfile.TemporaryDirectory() as directory:
            bad_weight = self.canonical_model(changed_weight=True)
            with self.assertRaisesRegex(ValueError, "initializer digest"):
                assemble_onnx_role_v3(
                    role_spec=role, recipe=recipe, canonical_model=bad_weight,
                    model_name="fixture", model_digest=_digest(b"model"),
                    profile_digest=recipe.artifact_profile_digest,
                    provider="/provider/p0", signature="signed",
                    cache_dir=directory)

            bad_recipe, bad_shape_role = self.recipe_and_role(
                model, expected_shape=(1, 2))
            with self.assertRaisesRegex(ValueError, "dtype/shape"):
                assemble_onnx_role_v3(
                    role_spec=bad_shape_role, recipe=bad_recipe,
                    canonical_model=model, model_name="fixture",
                    model_digest=_digest(b"model"),
                    profile_digest=bad_recipe.artifact_profile_digest,
                    provider="/provider/p0", signature="signed",
                    cache_dir=directory)

            bad_dtype_recipe, bad_dtype_role = self.recipe_and_role(
                model, expected_dtype=7)
            with self.assertRaisesRegex(ValueError, "dtype/shape"):
                assemble_onnx_role_v3(
                    role_spec=bad_dtype_role, recipe=bad_dtype_recipe,
                    canonical_model=model, model_name="fixture",
                    model_digest=_digest(b"model"),
                    profile_digest=bad_dtype_recipe.artifact_profile_digest,
                    provider="/provider/p0", signature="signed",
                    cache_dir=directory)

            _, bad_abi_role = self.recipe_and_role(
                model, backend_abi="onnxruntime-wrong")
            with self.assertRaisesRegex(ValueError, "backend ABI"):
                assemble_onnx_role_v3(
                    role_spec=bad_abi_role, recipe=recipe,
                    canonical_model=model, model_name="fixture",
                    model_digest=_digest(b"model"),
                    profile_digest=recipe.artifact_profile_digest,
                    provider="/provider/p0", signature="signed",
                    cache_dir=directory)

            bad_adapter_role = replace(
                role, adapter_descriptor_digest=_digest(b"wrong-adapter"))
            with self.assertRaisesRegex(ValueError, "adapter descriptor"):
                assemble_onnx_role_v3(
                    role_spec=bad_adapter_role, recipe=recipe,
                    canonical_model=model, model_name="fixture",
                    model_digest=_digest(b"model"),
                    profile_digest=recipe.artifact_profile_digest,
                    provider="/provider/p0", signature="signed",
                    cache_dir=directory)

            # Asking extraction to reach y pulls two nodes, but the signed
            # certificate permits only node 0.
            broad_recipe, broad_role = self.recipe_and_role(model, output="y")
            with self.assertRaisesRegex(ValueError, "node cover"):
                assemble_onnx_role_v3(
                    role_spec=broad_role, recipe=broad_recipe,
                    canonical_model=model, model_name="fixture",
                    model_digest=_digest(b"model"),
                    profile_digest=broad_recipe.artifact_profile_digest,
                    provider="/provider/p0", signature="signed",
                    cache_dir=directory)

            with self.assertRaisesRegex(ValueError, "missing or overlapping"):
                replace(recipe, node_indices=(0, 0))

            import onnx
            malicious = onnx.load_model_from_string(model)
            initializer = malicious.graph.initializer[0]
            initializer.ClearField("raw_data")
            initializer.data_location = onnx.TensorProto.EXTERNAL
            location = initializer.external_data.add()
            location.key = "location"
            location.value = "../escape.bin"
            with self.assertRaisesRegex(ValueError, "unsafe.*external-data"):
                assemble_onnx_role_v3(
                    role_spec=role, recipe=recipe,
                    canonical_model=malicious.SerializeToString(deterministic=True),
                    model_name="fixture", model_digest=_digest(b"model"),
                    profile_digest=recipe.artifact_profile_digest,
                    provider="/provider/p0", signature="signed",
                    cache_dir=directory)


if __name__ == "__main__":
    unittest.main()
