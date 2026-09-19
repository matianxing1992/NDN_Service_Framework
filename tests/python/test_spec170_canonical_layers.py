from __future__ import annotations

from pathlib import Path
import hashlib
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.adapters.qwen.canonical_layers import (  # noqa: E402
    CanonicalArtifactProfile, CanonicalLayerCatalog, ModelIdentity,
    canonical_layer_manifest,
)
from ndnsf_distributed_inference.adapters.onnx.graph import (  # noqa: E402
    canonical_onnx_identity,
)
from ndnsf_distributed_inference.artifact_deployment import (  # noqa: E402
    CanonicalCatalogEnsurer,
)


M = "sha256:" + "1" * 64
G = "sha256:" + "2" * 64
R = "sha256:" + "3" * 64


def _digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _identity() -> ModelIdentity:
    return ModelIdentity(
        publisher="/publisher", origin_signature_identity="/origin/key",
        provenance_digest=_digest(b"provenance"),
        source_content_digest=_digest(b"weights"),
        normalized_tensor_map_digest=_digest(b"tensor-map"),
        parameter_config_digest=_digest(b"config"),
        execution_semantics_digest=_digest(b"semantics"),
        graph_digest=G,
        tokenizer_digest=_digest(b"tokenizer"),
        preprocessing_digest=_digest(b"preprocessing"),
        metadata={"alias": "Qwen/Qwen3-0.6B"},
    )


def _profile() -> CanonicalArtifactProfile:
    return CanonicalArtifactProfile(
        layerizer_descriptor_digest=_digest(b"layerizer"),
        adapter_descriptor_digest=_digest(b"adapter"),
        serialization_schema_digest=_digest(b"schema"),
        chunking_layout_digest=_digest(b"chunking"),
        precision_format_digest=_digest(b"fp16"),
        protection_transform_digest=_digest(b"public"),
        protection_epoch="public-v1",
        tool_runtime_digest=_digest(b"onnx-tools"),
    )


def _metadata():
    return {
        "architecture": "qwen3", "parameterCount": 600_000_000,
        "layerCount": 28, "hiddenSize": 1024, "attentionHeads": 16,
        "experts": 0, "precision": "fp16", "sourceRevision": "digest-pinned",
    }


class Spec170CanonicalLayersTest(unittest.TestCase):
    def test_binding_keeps_planning_and_canonical_graph_identities_distinct(self):
        from dataclasses import replace
        from ndnsf_distributed_inference.app_sdk.canonical_artifacts import CanonicalArtifactBinding
        binding = CanonicalArtifactBinding(
            model_manifest_digest=M, artifact_profile_digest=R, graph_digest=G,
            canonical_initializer_digest=M, adapter_descriptor_digest=R,
            assembler_descriptor_digest=M, backend_abi="onnxruntime-cpu-v1",
            canonical_source_bytes=128)
        self.assertEqual(binding.canonical_graph_digest, "")
        canonical = replace(binding, canonical_graph_digest=R)
        self.assertEqual(canonical.graph_digest, G)
        self.assertEqual(canonical.canonical_graph_digest, R)
        with self.assertRaisesRegex(ValueError, "canonical_graph_digest"):
            replace(binding, canonical_graph_digest="invalid")
        with self.assertRaises(ValueError):
            replace(binding, canonical_initializer_data_name="/weights")
        complete = replace(binding, canonical_initializer_data_name="/weights",
                           canonical_initializer_object_digest=M,
                           canonical_initializer_bytes=64)
        self.assertEqual(complete.canonical_initializer_bytes, 64)

    def test_onnx_identity_ignores_file_and_initializer_packing_order(self):
        import numpy as np
        import onnx
        from onnx import TensorProto, helper, numpy_helper

        def write_model(path, initializer_order, changed=False):
            values = {
                "weight": np.asarray([[2.0 if changed else 1.0]], dtype=np.float32),
                "bias": np.asarray([0.5], dtype=np.float32),
            }
            initializers = [
                numpy_helper.from_array(values[name], name=name)
                for name in initializer_order
            ]
            graph = helper.make_graph(
                [helper.make_node("MatMul", ["x", "weight"], ["m"]),
                 helper.make_node("Add", ["m", "bias"], ["y"])],
                "canonical", [helper.make_tensor_value_info(
                    "x", TensorProto.FLOAT, [1, 1])],
                [helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1])],
                initializers)
            onnx.save(helper.make_model(graph), path)

        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "packed-a.onnx"
            second = Path(directory) / "different-name.onnx"
            changed = Path(directory) / "changed.onnx"
            write_model(first, ("weight", "bias"))
            write_model(second, ("bias", "weight"))
            write_model(changed, ("bias", "weight"), changed=True)
            identity_a = canonical_onnx_identity(first)
            identity_b = canonical_onnx_identity(second)
            identity_changed = canonical_onnx_identity(changed)
            self.assertEqual(identity_a, identity_b)
            self.assertNotEqual(identity_a.normalized_tensor_map_digest,
                                identity_changed.normalized_tensor_map_digest)
            self.assertEqual(identity_a.graph_digest,
                             identity_changed.graph_digest)

    def test_external_identity_streams_safe_subdirectory_and_zero_length(self):
        import numpy as np
        import onnx
        from onnx import TensorProto, helper, numpy_helper

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "weights").mkdir()
            initializer = numpy_helper.from_array(
                np.asarray([[2.0]], dtype=np.float32), name="weight")
            graph = helper.make_graph(
                [helper.make_node("MatMul", ["x", "weight"], ["y"])],
                "external-identity",
                [helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1])],
                [helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1])],
                [initializer],
            )
            model = helper.make_model(graph)
            inline = root / "inline.onnx"
            inline.write_bytes(model.SerializeToString(deterministic=True))
            external = root / "external.onnx"
            onnx.save_model(
                model, str(external), save_as_external_data=True,
                all_tensors_to_one_file=True, location="weights/weights.bin",
                size_threshold=0,
            )
            external_model = onnx.load(str(external), load_external_data=False)
            for entry in external_model.graph.initializer[0].external_data:
                if entry.key == "length":
                    entry.value = "0"
            external.write_bytes(external_model.SerializeToString(deterministic=True))

            self.assertEqual(
                canonical_onnx_identity(inline),
                canonical_onnx_identity(external),
            )

    def test_external_identity_rejects_duplicate_or_escaping_metadata(self):
        import numpy as np
        import onnx
        from onnx import TensorProto, helper, numpy_helper

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = helper.make_model(helper.make_graph(
                [], "external-invalid", [], [], [numpy_helper.from_array(
                    np.asarray([1.0], dtype=np.float32), name="weight")]))
            external = root / "external.onnx"
            onnx.save_model(
                model, str(external), save_as_external_data=True,
                all_tensors_to_one_file=True, location="weights.bin",
                size_threshold=0,
            )
            document = onnx.load(str(external), load_external_data=False)
            entries = document.graph.initializer[0].external_data
            entries.add(key="location", value="weights.bin")
            external.write_bytes(document.SerializeToString(deterministic=True))
            with self.assertRaisesRegex(ValueError, "duplicate external tensor metadata"):
                canonical_onnx_identity(external)

            document = onnx.load(str(external), load_external_data=False)
            entries = document.graph.initializer[0].external_data
            entries[0].value = "../weights.bin"
            del entries[1:]
            external.write_bytes(document.SerializeToString(deterministic=True))
            with self.assertRaisesRegex(ValueError, "escapes source root"):
                canonical_onnx_identity(external)

    def test_root_last_idempotent_publication(self):
        payload = b"layer-0"
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=M, profile="fp16",
            graph_digest=G, role_kind="pipeline", layer_begin=0, layer_end=2,
            rank=0, recipe_digest=R, payload=payload, publisher="/publisher")
        catalog = CanonicalLayerCatalog()
        name = catalog.publish_layer(manifest, payload)
        self.assertIn("/NDNSF-DI/MODEL/v1/NAME/", name)
        self.assertEqual(catalog.publish_layer(manifest, payload), name)
        self.assertTrue(catalog.publish_root().startswith("sha256:"))
        self.assertEqual(len(catalog.root()), 1)

    def test_same_payload_name_conflict_is_rejected(self):
        payload = b"layer-0"
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=M, profile="fp16",
            graph_digest=G, role_kind="pipeline", layer_begin=0, layer_end=2,
            rank=0, recipe_digest=R, payload=payload, publisher="/publisher")
        catalog = CanonicalLayerCatalog()
        catalog.publish_layer(manifest, payload)
        with self.assertRaises(ValueError):
            catalog.publish_layer(manifest, b"tampered")

    def test_alias_names_are_not_accepted(self):
        with self.assertRaises(ValueError):
            canonical_layer_manifest(
                model_name="Qwen/Qwen3-0.6B", model_digest=M, profile="REV",
                graph_digest=G, role_kind="pipeline", layer_begin=0,
                layer_end=2, rank=0, recipe_digest=R, payload=b"x",
                publisher="/publisher")

    def test_v3_name_uses_manifest_and_segment_grammar_without_rank(self):
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=M, profile="fp16",
            graph_digest=G, role_kind="pipeline", layer_begin=0, layer_end=2,
            rank=7, recipe_digest=R, payload=b"x", publisher="/publisher")
        name = manifest.name
        self.assertIn("/PROFILE/sha256:", name)
        self.assertIn("/MANIFEST/sha256:", name)
        self.assertIn("/LAYER/PIPELINE_RANGE/layers-0-2/", name)
        self.assertIn("/OBJECT/sha256:", name)
        self.assertTrue(name.endswith("/0"))
        self.assertNotIn("/RANK/", name)
        self.assertNotIn("/RECIPE/", name)

        same_component_other_rank = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=M, profile="fp16",
            graph_digest=G, role_kind="pipeline", layer_begin=0, layer_end=2,
            rank=0, recipe_digest=R, payload=b"x", publisher="/publisher")
        self.assertEqual(manifest.manifest_digest,
                         same_component_other_rank.manifest_digest)
        self.assertEqual(manifest.name, same_component_other_rank.name)

    def test_repo_adapter_is_root_last_and_idempotent(self):
        payload = b"aaaabbbb"
        model = _identity()
        profile = _profile()
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=model.digest,
            profile=profile.digest,
            graph_digest=G, role_kind="pipeline", layer_begin=0, layer_end=2,
            rank=0, recipe_digest=R, payload=payload, publisher="/publisher",
            model_identity=model, artifact_profile=profile,
            tensor_index=(
                {"tensorName": "a", "dtype": "uint8", "shape": [4],
                 "byteOrder": "na", "offset": 0, "length": 4,
                 "chunkDigest": _digest(b"aaaa")},
                {"tensorName": "b", "dtype": "uint8", "shape": [4],
                 "byteOrder": "na", "offset": 4, "length": 4,
                 "chunkDigest": _digest(b"bbbb")},
            ))
        catalog = CanonicalLayerCatalog()
        catalog.publish_layer(manifest, payload)
        root_manifest = catalog.activate_model(
            model_name="Qwen/Qwen3-0.6B", model_identity=model,
            artifact_profile=profile,
            origin_attestation="origin:" + model.digest,
            transformation_attestation="transform:" + profile.digest,
            activation_epoch="epoch-1", signer="/publisher",
            signature="signed-root", metadata=_metadata(),
            verify_origin=lambda identity, value: identity.digest in value,
            verify_transformation=lambda identity, selected, value:
                selected.digest in value,
            verify_signature=lambda wire, signature, signer:
                signature == "signed-root" and signer == "/publisher",
        )
        calls = []

        def publish_object(**kwargs):
            calls.append(kwargs)
            return kwargs["name"]

        root = catalog.publish_via(publish_object, publisher="/publisher")
        self.assertEqual(root, root_manifest.name)
        self.assertEqual(len(calls), 3)
        self.assertIn("/OBJECT/sha256:", calls[0]["name"])
        self.assertTrue(calls[1]["name"].endswith(
            "/MANIFEST/" + manifest.layer_manifest_digest))
        self.assertEqual(calls[2]["name"], root)

        # Two request-scoped placements select different tensors from the same
        # immutable canonical object instead of publishing role-specific copies.
        placement_a = catalog.select_tensors(("a",))
        placement_b = catalog.select_tensors(("a", "b"))
        self.assertEqual(placement_a[0].object_name,
                         placement_b[0].object_name)
        self.assertEqual(placement_a[0].payload, b"aaaa")
        self.assertEqual({item.tensor_name for item in placement_b}, {"a", "b"})

    def test_activation_rejects_attestation_or_incomplete_cover(self):
        model = _identity()
        profile = _profile()
        payload = b"layer"
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=model.digest,
            profile=profile.digest, graph_digest=G, role_kind="pipeline",
            layer_begin=0, layer_end=1, rank=0, recipe_digest=R,
            payload=payload, publisher="/publisher", model_identity=model,
            artifact_profile=profile)
        catalog = CanonicalLayerCatalog()
        catalog.publish_layer(manifest, payload)
        with self.assertRaisesRegex(ValueError, "origin attestation"):
            catalog.activate_model(
                model_name="Qwen/Qwen3-0.6B", model_identity=model,
                artifact_profile=profile, origin_attestation="tampered",
                transformation_attestation="transform:" + profile.digest,
                activation_epoch="epoch-1", signer="/publisher",
                signature="signed-root", metadata=_metadata(),
                verify_origin=lambda identity, value: identity.digest in value,
                verify_transformation=lambda identity, selected, value: True,
                verify_signature=lambda wire, signature, signer: True)

    def test_two_ack_driven_placements_publish_canonical_bytes_once(self):
        model = _identity()
        profile = _profile()
        payload = b"canonical-layer"
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=model.digest,
            profile=profile.digest, graph_digest=G, role_kind="pipeline",
            layer_begin=0, layer_end=2, rank=0, recipe_digest=R,
            payload=payload, publisher="/publisher", model_identity=model,
            artifact_profile=profile)
        catalog = CanonicalLayerCatalog()
        catalog.publish_layer(manifest, payload)
        catalog.activate_model(
            model_name="Qwen/Qwen3-0.6B", model_identity=model,
            artifact_profile=profile,
            origin_attestation="origin:" + model.digest,
            transformation_attestation="transform:" + profile.digest,
            activation_epoch="epoch-1", signer="/publisher",
            signature="signed-root", metadata=_metadata(),
            verify_origin=lambda identity, value: identity.digest in value,
            verify_transformation=lambda identity, selected, value:
                selected.digest in value,
            verify_signature=lambda wire, signature, signer: True)
        calls = []

        def publish_object(**kwargs):
            calls.append(kwargs)
            return kwargs["name"]

        ensurer = CanonicalCatalogEnsurer(
            catalog, publish_object, publisher="/publisher")
        role = SimpleNamespace(
            role="stage", rank=0, role_kind="PIPELINE_RANGE",
            layer_begin=0, layer_end=2, artifact_digest=_digest(b"assembly"))
        first = ensurer.ensure(
            SimpleNamespace(candidate_digest=_digest(b"placement-a")), (role,),
            deadline_ms=2**62)
        second = ensurer.ensure(
            SimpleNamespace(candidate_digest=_digest(b"placement-b")), (role,),
            deadline_ms=2**62)
        self.assertEqual(len(calls), 3)  # object, layer manifest, ACTIVE root
        self.assertNotEqual(first.candidate_digest, second.candidate_digest)
        self.assertEqual(first.artifact_data_names_by_role,
                         second.artifact_data_names_by_role)

    def test_spec175_source_reference_is_strict_and_published_before_root(self):
        model = _identity()
        profile = _profile()
        layer_payload = b"canonical-layer"
        source_payload = b"complete-canonical-onnx"
        source_name = "/publisher/NDNSF-DI/SOURCE/Qwen3-0.6B/v1"
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=model.digest,
            profile=profile.digest, graph_digest=G, role_kind="pipeline",
            layer_begin=0, layer_end=2, rank=0, recipe_digest=R,
            payload=layer_payload, publisher="/publisher",
            model_identity=model, artifact_profile=profile)
        catalog = CanonicalLayerCatalog()
        catalog.publish_layer(manifest, layer_payload)
        catalog.activate_model(
            model_name="Qwen/Qwen3-0.6B", model_identity=model,
            artifact_profile=profile,
            origin_attestation="origin:" + model.digest,
            transformation_attestation="transform:" + profile.digest,
            activation_epoch="epoch-1", signer="/publisher",
            signature="signed-root", metadata=_metadata(),
            canonical_source_data_name=source_name,
            canonical_source_digest=_digest(source_payload),
            canonical_source_bytes=len(source_payload),
            verify_origin=lambda identity, value: identity.digest in value,
            verify_transformation=lambda identity, selected, value:
                selected.digest in value,
            verify_signature=lambda wire, signature, signer: True)
        calls = []

        def publish_object(**kwargs):
            calls.append(kwargs)
            return kwargs["name"]

        ensurer = CanonicalCatalogEnsurer(
            catalog, publish_object, publisher="/publisher",
            require_canonical_source=True,
            canonical_source_payload=source_payload)
        binding = ensurer.describe()
        self.assertEqual(binding.canonical_source_data_name, source_name)
        self.assertEqual(binding.canonical_source_digest, _digest(source_payload))
        role = SimpleNamespace(
            role="stage", rank=0, role_kind="PIPELINE_RANGE",
            layer_begin=0, layer_end=2, artifact_digest=_digest(b"assembly"))
        ensurer.ensure(
            SimpleNamespace(candidate_digest=_digest(b"placement")), (role,),
            deadline_ms=2**62)
        self.assertEqual(calls[0]["name"], source_name)
        self.assertEqual(calls[0]["payload"], source_payload)
        self.assertEqual(calls[-1]["name"], catalog.active_manifest.name)

    def test_spec175_strict_ensurer_rejects_legacy_root_without_source(self):
        model = _identity()
        profile = _profile()
        payload = b"canonical-layer"
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=model.digest,
            profile=profile.digest, graph_digest=G, role_kind="pipeline",
            layer_begin=0, layer_end=2, rank=0, recipe_digest=R,
            payload=payload, publisher="/publisher",
            model_identity=model, artifact_profile=profile)
        catalog = CanonicalLayerCatalog()
        catalog.publish_layer(manifest, payload)
        catalog.activate_model(
            model_name="Qwen/Qwen3-0.6B", model_identity=model,
            artifact_profile=profile,
            origin_attestation="origin:" + model.digest,
            transformation_attestation="transform:" + profile.digest,
            activation_epoch="epoch-1", signer="/publisher",
            signature="signed-root", metadata=_metadata(),
            verify_origin=lambda identity, value: identity.digest in value,
            verify_transformation=lambda identity, selected, value: True,
            verify_signature=lambda wire, signature, signer: True)
        with self.assertRaisesRegex(ValueError, "canonical source reference"):
            CanonicalCatalogEnsurer(
                catalog, lambda **kwargs: kwargs["name"],
                publisher="/publisher", require_canonical_source=True).describe()

    def test_spec180_external_initializer_root_is_bound_and_published(self):
        model = _identity()
        profile = _profile()
        layer_payload = b"canonical-layer"
        initializer_payload = b"external-initializer"
        initializer_name = "/publisher/NDNSF-DI/SOURCE/YOLO26n/v1/weights"
        manifest = canonical_layer_manifest(
            model_name="YOLO26n", model_digest=model.digest,
            profile=profile.digest, graph_digest=G, role_kind="component",
            layer_begin=0, layer_end=1, rank=0, recipe_digest=R,
            payload=layer_payload, publisher="/publisher",
            model_identity=model, artifact_profile=profile)
        catalog = CanonicalLayerCatalog()
        catalog.publish_layer(manifest, layer_payload)
        catalog.activate_model(
            model_name="YOLO26n", model_identity=model,
            artifact_profile=profile,
            origin_attestation="origin:" + model.digest,
            transformation_attestation="transform:" + profile.digest,
            activation_epoch="epoch-1", signer="/publisher",
            signature="signed-root", metadata=_metadata(),
            canonical_initializer_data_name=initializer_name,
            canonical_initializer_object_digest=_digest(initializer_payload),
            canonical_initializer_bytes=len(initializer_payload),
            verify_origin=lambda identity, value: identity.digest in value,
            verify_transformation=lambda identity, selected, value:
                selected.digest in value,
            verify_signature=lambda wire, signature, signer: True)
        calls = []

        def publish_object(**kwargs):
            calls.append(kwargs)
            return kwargs["name"]

        ensurer = CanonicalCatalogEnsurer(
            catalog, publish_object, publisher="/publisher",
            canonical_initializer_payload=initializer_payload)
        binding = ensurer.describe()
        self.assertEqual(binding.canonical_initializer_data_name,
                         initializer_name)
        self.assertEqual(binding.canonical_initializer_object_digest,
                         _digest(initializer_payload))
        self.assertEqual(binding.canonical_initializer_bytes,
                         len(initializer_payload))
        role = SimpleNamespace(
            role="FullModel", rank=0, role_kind="COMPONENT_SET",
            layer_begin=0, layer_end=1, artifact_digest=_digest(b"assembly"))
        ensurer.ensure(
            SimpleNamespace(candidate_digest=_digest(b"placement")), (role,),
            deadline_ms=2**62)
        self.assertEqual(calls[0]["name"], initializer_name)
        self.assertEqual(calls[0]["payload"], initializer_payload)
        self.assertEqual(calls[-1]["name"], catalog.active_manifest.name)


if __name__ == "__main__":
    unittest.main()
