from __future__ import annotations

from pathlib import Path
import hashlib
import json
import struct
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
from ndnsf_distributed_inference.app_sdk.canonical_artifacts import (  # noqa: E402
    canonical_model_object_name,
)


M = "sha256:" + "1" * 64
G = "sha256:" + "2" * 64
R = "sha256:" + "3" * 64


def _digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _framed_digest(domain: str, fields: list[tuple[int, int, bytes]]) -> str:
    raw_domain = domain.encode("ascii")
    wire = struct.pack(">H", len(raw_domain)) + raw_domain + struct.pack(">HH", 2, len(fields))
    for tag, kind, value in fields:
        wire += struct.pack(">HBQ", tag, kind, len(value)) + value
    return _digest(wire)


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
    def test_artifact_spec_uses_active_catalog_source_and_layer_context(self):
        from ndnsf_distributed_inference.plan import ArtifactSpec
        from ndnsf_distributed_inference.app_sdk.canonical_artifacts import _model_naming_identity
        model, profile = _identity(), _profile()
        source, initializer, payload = b"source-model", b"initializers", b"layer-model"
        catalog = CanonicalLayerCatalog()
        with self.assertRaisesRegex(RuntimeError, "ACTIVE"):
            ArtifactSpec.from_canonical_catalog(catalog, name="m", payload=source, filename="m.onnx")
        layer = canonical_layer_manifest(
            model_name="model/full", model_digest=model.digest, profile=profile.digest,
            graph_digest=G, role_kind="tensor", layer_begin=0, layer_end=2,
            rank=1, recipe_digest=R, payload=payload, publisher="/publisher",
            model_identity=model, artifact_profile=profile)
        catalog.publish_layer(layer, payload)
        catalog.activate_model(
            model_name="model/full", model_identity=model, artifact_profile=profile,
            origin_attestation="origin", transformation_attestation="transform",
            activation_epoch="epoch", signer="/publisher", signature="root",
            metadata=_metadata(),
            canonical_source_data_name=canonical_model_object_name(
                publisher="/publisher", model_name="model/full", model_digest=model.digest,
                object_kind="model-file", object_label="canonical-source", object_digest=_digest(source)),
            canonical_source_digest=_digest(source), canonical_source_bytes=len(source),
            canonical_initializer_data_name=canonical_model_object_name(
                publisher="/publisher", model_name="model/full", model_digest=model.digest,
                object_kind="initializer", object_label="initializers", object_digest=_digest(initializer)),
            canonical_initializer_object_digest=_digest(initializer),
            canonical_initializer_bytes=len(initializer),
            verify_origin=lambda *args: True, verify_transformation=lambda *args: True,
            verify_signature=lambda *args: True)
        for kind, data, expected in (("model", source, "model-file"),
                                      ("initializer", initializer, "initializer")):
            artifact = ArtifactSpec.from_canonical_catalog(
                catalog, name="m", payload=data, filename="run-specific.bin", kind=kind)
            self.assertEqual(artifact.content_name["kind"], expected)
            self.assertEqual(artifact.content_name["model_name"], "model/full")
            self.assertEqual(artifact.content_name["model_identity"],
                             _model_naming_identity("model/full", model.digest)[7:])
            with self.assertRaisesRegex(ValueError, "verified source"):
                ArtifactSpec.from_canonical_catalog(
                    catalog, name="m", payload=b"wrong", filename="m", kind=kind)
        artifact = ArtifactSpec.from_canonical_catalog(
            catalog, name="layer", payload=payload, filename="run-specific.onnx",
            layer_manifest_digest=layer.layer_manifest_digest)
        self.assertEqual(artifact.content_name["kind"], "artifact-file")
        self.assertEqual(artifact.content_name["coordinate"], "layers=0-1.rank=1")
        self.assertEqual(artifact.content_name["profile_identity"], profile.digest[7:])
        with self.assertRaisesRegex(ValueError, "verified layer"):
            ArtifactSpec.from_canonical_catalog(
                catalog, name="layer", payload=b"wrong", filename="m",
                layer_manifest_digest=layer.layer_manifest_digest)
        with self.assertRaisesRegex(ValueError, "ACTIVE root"):
            ArtifactSpec.from_canonical_catalog(
                catalog, name="layer", payload=payload, filename="m",
                layer_manifest_digest=_digest(b"missing"))

    def test_manifest_name_preserves_full_producer_and_exact_utf8_payload(self):
        import json
        from ndn.encoding import Component
        from ndnsf_distributed_inference.naming import parse_producer_data_name
        producer = "/org/NDNSF/v=7/%00/owner"
        manifest = canonical_layer_manifest(
            model_name="Qwen/model", model_digest=M, profile=R,
            graph_digest=G, role_kind="stage", layer_begin=0, layer_end=1,
            rank=0, recipe_digest=R, payload=b"bytes", publisher=producer,
            origin_attestation="来源证明", transformation_attestation="变换证明")
        parsed = parse_producer_data_name(manifest.manifest_name, producer)
        self.assertIsNotNone(parsed)
        self.assertEqual(parsed.name_space, "NDNSF-DI")
        self.assertEqual(parsed.category, "model")
        self.assertEqual(len(parsed.suffix), 9)
        self.assertEqual(bytes(Component.get_value(parsed.suffix[0])), b"name=Qwen/model")
        self.assertEqual(Component.get_type(parsed.suffix[6]), Component.TYPE_GENERIC)
        self.assertEqual(bytes(Component.get_value(parsed.suffix[6])), b"layers=0-0")
        identity = _framed_digest("ndnsf-naming/sdk-model", [
            (1, 2, b"Qwen/model"), (2, 4, bytes.fromhex(M[7:])),
        ])
        self.assertEqual(bytes(Component.get_value(parsed.suffix[1])), bytes.fromhex(identity[7:]))
        payload = json.dumps(manifest.to_dict(), sort_keys=True, separators=(",", ":"),
                             ensure_ascii=False).encode("utf-8")
        self.assertEqual(bytes(Component.get_value(parsed.suffix[-1])),
                         hashlib.sha256(payload).digest())
        self.assertEqual(manifest.manifest_digest, _digest(payload))
        self.assertIsNone(parse_producer_data_name(manifest.manifest_name, "/org"))

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
        self.assertTrue(name.startswith("/8=publisher/8=NDNSF-DI/8=model/"))
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

    def test_byte_identity_is_content_prefix_without_fake_segments_or_rank(self):
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=M, profile="fp16",
            graph_digest=G, role_kind="pipeline", layer_begin=0, layer_end=2,
            rank=7, recipe_digest=R, payload=b"x", publisher="/publisher")
        name = manifest.name
        from ndn.encoding import Component
        from ndnsf_distributed_inference.naming import parse_producer_data_name
        parsed = parse_producer_data_name(name, "/publisher")
        self.assertEqual(parsed.name_space, "NDNSF-DI")
        self.assertEqual(parsed.category, "model")
        self.assertEqual(len(parsed.suffix), 10)
        self.assertEqual(Component.get_type(parsed.suffix[0]), 8)
        self.assertEqual(bytes(Component.get_value(parsed.suffix[-1])), hashlib.sha256(b"x").digest())

        same_component_other_rank = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=M, profile="fp16",
            graph_digest=G, role_kind="pipeline", layer_begin=0, layer_end=2,
            rank=0, recipe_digest=R, payload=b"x", publisher="/publisher")
        self.assertEqual(manifest.manifest_digest,
                         same_component_other_rank.manifest_digest)
        self.assertEqual(manifest.name, same_component_other_rank.name)

    def test_equal_bytes_bind_profile_and_reject_fake_segments(self):
        from ndnsf_distributed_inference.app_sdk.canonical_artifacts import canonical_layer_name
        arguments = dict(
            publisher="/publisher", model_name="model", model_digest=M,
            graph_digest=G, role_kind="pipeline", layer_begin=0, layer_end=2,
            object_digest=_digest(b"same-bytes"))
        self.assertNotEqual(canonical_layer_name(**arguments, profile="fp16"),
                         canonical_layer_name(**arguments, profile="fp32", rank=7))
        self.assertEqual(canonical_layer_name(**arguments, profile="fp16"),
                         canonical_layer_name(**arguments, profile="fp16", rank=7))
        for segment in (1, -1, True):
            with self.subTest(segment=segment), self.assertRaises(ValueError):
                canonical_layer_name(**arguments, profile="fp16", segment_number=segment)

    def test_full_labels_and_rank_bind_semantic_identity(self):
        from ndn.encoding import Component
        from ndnsf_distributed_inference.app_sdk.canonical_artifacts import canonical_layer_name
        from ndnsf_distributed_inference.naming import parse_producer_data_name
        arguments = dict(publisher="/publisher", model_digest=M, graph_digest=G,
                         profile="fp16", role_kind="tensor", layer_begin=0, layer_end=2,
                         object_digest=_digest(b"same-bytes"))
        left = canonical_layer_name(**arguments, model_name="m" * 64 + "-left", rank=0)
        right = canonical_layer_name(**arguments, model_name="m" * 64 + "-right", rank=0)
        a = parse_producer_data_name(left, "/publisher")
        b = parse_producer_data_name(right, "/publisher")
        self.assertEqual(a.suffix[0], b.suffix[0])
        self.assertNotEqual(a.suffix[1], b.suffix[1])
        self.assertNotEqual(left, right)
        ranked = canonical_layer_name(**arguments, model_name="m" * 64 + "-left", rank=1)
        self.assertNotEqual(left, ranked)
        self.assertEqual(bytes(Component.get_value(a.suffix[4])), b"layers=0-1.rank=0")
        for rank in (-1, True, 0.5, 2**64):
            with self.subTest(rank=rank), self.assertRaises(ValueError):
                canonical_layer_name(**arguments, model_name="model", rank=rank)

    def test_equal_bytes_keep_distinct_layer_coordinates(self):
        model, profile, payload = _identity(), _profile(), b"shared-layer"
        catalog = CanonicalLayerCatalog()
        layers = []
        for begin in (0, 1):
            layer = canonical_layer_manifest(
                model_name="model", model_digest=model.digest, profile=profile.digest,
                graph_digest=G, role_kind="pipeline", layer_begin=begin,
                layer_end=begin + 1, rank=0, recipe_digest=R, payload=payload,
                publisher="/publisher", model_identity=model, artifact_profile=profile)
            layers.append(layer)
            catalog.publish_layer(layer, payload)
        catalog.activate_model(
            model_name="model", model_identity=model, artifact_profile=profile,
            origin_attestation="origin", transformation_attestation="transform",
            activation_epoch="epoch-1", signer="/publisher", signature="root",
            metadata=_metadata(), verify_origin=lambda *args: True,
            verify_transformation=lambda *args: True, verify_signature=lambda *args: True)
        calls = []

        def publish_object(**kwargs):
            calls.append(kwargs)
            return kwargs["name"]

        catalog.publish_via(publish_object, publisher="/publisher")
        self.assertNotEqual(layers[0].name, layers[1].name)
        self.assertNotEqual(layers[0].manifest_name, layers[1].manifest_name)
        self.assertEqual(len(calls), 5)  # two semantic objects, two descriptors, root
        self.assertEqual(sum(call["payload"] == payload for call in calls), 2)
        self.assertNotEqual(calls[0]["idempotency_key"], calls[2]["idempotency_key"])
        self.assertNotIn("layerManifestDigest", calls[0]["manifest"])
        self.assertEqual(calls[0]["manifest"]["objectBytes"], len(payload))

    def test_catalog_keeps_tensor_and_hybrid_ranks_distinct(self):
        for kind in ("tensor", "hybrid"):
            with self.subTest(kind=kind):
                model, profile, payload = _identity(), _profile(), b"same-rank-bytes"
                catalog = CanonicalLayerCatalog()
                layers = []
                for rank in (0, 1):
                    layer = canonical_layer_manifest(
                        model_name="model", model_digest=model.digest, profile=profile.digest,
                        graph_digest=G, role_kind=kind, layer_begin=0, layer_end=1,
                        rank=rank, recipe_digest=R, payload=payload, publisher="/publisher",
                        model_identity=model, artifact_profile=profile)
                    layers.append(layer)
                    self.assertEqual(catalog.publish_layer(layer, payload), layer.name)
                self.assertNotEqual(layers[0], layers[1])
                self.assertNotEqual(layers[0].layer_manifest_digest, layers[1].layer_manifest_digest)
                self.assertNotEqual(layers[0].manifest_digest, layers[1].manifest_digest)
                self.assertEqual(layers[1].to_dict()["stableCoordinate"], "layers=0-0.rank=1")
                root = catalog.activate_model(
                    model_name="model", model_identity=model, artifact_profile=profile,
                    origin_attestation="origin", transformation_attestation="transform",
                    activation_epoch="epoch-1", signer="/publisher", signature="root",
                    metadata=_metadata(), verify_origin=lambda *args: True,
                    verify_transformation=lambda *args: True, verify_signature=lambda *args: True)
                self.assertEqual(len(root.layer_manifest_digests), 2)
                for rank in (0, 1):
                    selected = catalog.select_components(
                        role_kind=kind, layer_begin=0, layer_end=1, rank=rank)
                    self.assertEqual(selected, (layers[rank],))
                with self.assertRaisesRegex(ValueError, "explicit rank"):
                    catalog.select_components(role_kind=kind, layer_begin=0, layer_end=1)
                calls = []
                def publish_object(**kwargs):
                    calls.append(kwargs)
                    return kwargs["name"]
                catalog.publish_via(publish_object, publisher="/publisher")
                self.assertEqual(len(calls), 5)
                self.assertNotEqual(calls[0]["idempotency_key"], calls[2]["idempotency_key"])

                calls.clear()
                ensurer = CanonicalCatalogEnsurer(catalog, publish_object, publisher="/publisher")
                roles = tuple(SimpleNamespace(
                    role="ranked-role", rank=rank, role_kind=kind,
                    layer_begin=0, layer_end=1,
                    artifact_digest=_digest(b"assembled-rank" + bytes([rank])))
                    for rank in (0, 1))
                binding = ensurer.ensure(
                    SimpleNamespace(candidate_digest=_digest(b"placement")), roles,
                    deadline_ms=2**62)
                self.assertEqual(set(binding.artifact_data_names_by_role),
                                 {"ranked-role#0", "ranked-role#1"})
                self.assertEqual(len(calls), 5)
                ensurer.ensure(SimpleNamespace(candidate_digest=_digest(b"warm-placement")),
                               roles, deadline_ms=2**62)
                self.assertEqual(len(calls), 5)

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
        self.assertEqual(calls[0]["name"], manifest.name)
        self.assertEqual(calls[1]["name"], manifest.manifest_name)
        from ndn.encoding import Component, Name
        for call in calls:
            expected = _framed_digest("ndnsf-naming/sdk-publication", [
                (1, 1, bytes(Name.encode(Name.from_str(call["name"])))),
                (2, 4, hashlib.sha256(call["payload"]).digest()),
            ])
            self.assertEqual(call["idempotency_key"], expected)
        from ndnsf_distributed_inference.naming import parse_producer_data_name
        artifact = parse_producer_data_name(calls[1]["name"], "/publisher")
        self.assertIsNotNone(artifact)
        self.assertEqual(artifact.name_space, "NDNSF-DI")
        self.assertEqual(artifact.category, "model")
        self.assertEqual(bytes(Component.get_value(artifact.suffix[-1])),
                         hashlib.sha256(calls[1]["payload"]).digest())
        model_root = parse_producer_data_name(root, "/publisher")
        self.assertIsNotNone(model_root)
        self.assertEqual(model_root.name_space, "NDNSF-DI")
        self.assertEqual(model_root.category, "model")
        self.assertEqual(len(model_root.suffix), 4)
        self.assertEqual(bytes(Component.get_value(model_root.suffix[-1])),
                         hashlib.sha256(calls[2]["payload"]).digest())
        before = len(calls)
        with self.assertRaisesRegex(ValueError, "does not own"):
            catalog.publish_via(publish_object, publisher="/another/publisher")
        self.assertEqual(len(calls), before)
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

    def test_active_root_rejects_another_full_model_label(self):
        model, profile = _identity(), _profile()
        payload = b"canonical-layer"
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=model.digest,
            profile=profile.digest, graph_digest=G, role_kind="pipeline",
            layer_begin=0, layer_end=2, rank=0, recipe_digest=R,
            payload=payload, publisher="/publisher", model_identity=model,
            artifact_profile=profile)
        catalog = CanonicalLayerCatalog()
        catalog.publish_layer(manifest, payload)
        verifications = []
        with self.assertRaisesRegex(ValueError, "root/layer identity cover mismatch"):
            catalog.activate_model(
                model_name="another-model", model_identity=model, artifact_profile=profile,
                origin_attestation="origin", transformation_attestation="transform",
                activation_epoch="epoch", signer="/publisher", signature="signature",
                metadata=_metadata(), verify_origin=lambda *args: verifications.append(args),
                verify_transformation=lambda *args: verifications.append(args),
                verify_signature=lambda *args: verifications.append(args))
        self.assertEqual(verifications, [])
        with self.assertRaises(RuntimeError):
            _ = catalog.active_manifest

    def test_standalone_model_object_names_bind_full_context(self):
        from ndn.encoding import Component, Name
        from ndnsf_distributed_inference.naming import parse_producer_data_name, canonical_name_uri
        producer = canonical_name_uri(Name.from_str("/org/NDNSF-DI/model/publisher") +
                                      [Component.from_version(257)])
        model_name, label = "Qwen/模型", "权重/α.dat"
        model_hash = _framed_digest("ndnsf-naming/sdk-model", [
            (1, 2, model_name.encode()), (2, 4, bytes.fromhex(M[7:]))])
        parent = json.dumps({"coordinate": "", "kind": "", "modelIdentity": model_hash[7:],
                             "profile": ""}, sort_keys=True, separators=(",", ":")).encode()
        for kind, leaf in (("model-file", "file"), ("weights", "weights"),
                           ("initializer", "initializer"), ("tokenizer", "tokenizer")):
            name = canonical_model_object_name(
                publisher=producer, model_name=model_name, model_digest=M,
                object_kind=kind, object_label=label, object_digest=G)
            parsed = parse_producer_data_name(name, producer)
            self.assertIsNotNone(parsed)
            self.assertEqual((parsed.name_space, parsed.category), ("NDNSF-DI", "model"))
            values = [bytes(Component.get_value(c)) for c in parsed.suffix]
            self.assertEqual(len(values), 6)
            self.assertEqual(values[:4], [b"name=" + model_name.encode(),
                                         bytes.fromhex(model_hash[7:]), leaf.encode(),
                                         b"name=" + label.encode()])
            expected = _framed_digest("ndnsf-naming/semantic-object", [
                (1, 2, b"NDNSF-DI"), (2, 2, ("model/" + leaf).encode()),
                (3, 2, label.encode()), (4, 5, parent)])
            self.assertEqual(values[4:], [bytes.fromhex(expected[7:]), bytes.fromhex(G[7:])])
            self.assertTrue(all(Component.get_type(c) == 8 for c in parsed.suffix))
        names = [canonical_model_object_name(
            publisher=producer, model_name="a" * 80 + tail, model_digest=M,
            object_kind="model-file", object_label="canonical-source", object_digest=G)
            for tail in ("A", "B")]
        self.assertNotEqual(names[0], names[1])
        for kind in ("blob", "material", "artifact-file", "unknown"):
            with self.assertRaises(ValueError):
                canonical_model_object_name(
                    publisher=producer, model_name=model_name, model_digest=M,
                    object_kind=kind, object_label=label, object_digest=G)

    def test_spec175_source_reference_is_strict_and_published_before_root(self):
        model = _identity()
        profile = _profile()
        layer_payload = b"canonical-layer"
        source_payload = b"complete-canonical-onnx"
        source_name = canonical_model_object_name(
            publisher="/publisher", model_name="Qwen/Qwen3-0.6B", model_digest=model.digest,
            object_kind="model-file", object_label="canonical-source",
            object_digest=_digest(source_payload))
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
        candidate = SimpleNamespace(candidate_digest=_digest(b"placement"))
        other_publisher = CanonicalCatalogEnsurer(
            catalog, publish_object, publisher="/publisher-other",
            require_canonical_source=True, canonical_source_payload=source_payload)
        with self.assertRaisesRegex(ValueError, "does not own"):
            other_publisher.ensure(candidate, (role,), deadline_ms=2**62)
        self.assertEqual(calls, [])
        # A reference may point elsewhere, but this actor cannot publish its
        # supplied bytes under a lookalike Producer prefix.
        metadata = catalog.active_manifest.metadata
        original_name = metadata["canonicalSourceDataName"]
        try:
            metadata["canonicalSourceDataName"] = "/publisher-other/NDNSF/BLOB/source"
            foreign_source = CanonicalCatalogEnsurer(
                catalog, publish_object, publisher="/publisher",
                require_canonical_source=True, canonical_source_payload=source_payload)
            with self.assertRaisesRegex(ValueError, "does not own"):
                foreign_source.ensure(candidate, (role,), deadline_ms=2**62)
            self.assertEqual(calls, [])
        finally:
            metadata["canonicalSourceDataName"] = original_name
        invalid_names = ["/publisher/NDNSF-DI/SOURCE/legacy"] + [
            canonical_model_object_name(
                publisher="/publisher", model_name=name, model_digest=model.digest,
                object_kind=kind, object_label="canonical-source", object_digest=digest)
            for name, kind, digest in (
                ("another-model", "model-file", _digest(source_payload)),
                ("Qwen/Qwen3-0.6B", "initializer", _digest(source_payload)),
                ("Qwen/Qwen3-0.6B", "model-file", _digest(b"wrong-source")))
        ]
        try:
            for invalid_name in invalid_names:
                metadata["canonicalSourceDataName"] = invalid_name
                invalid = CanonicalCatalogEnsurer(
                    catalog, publish_object, publisher="/publisher",
                    canonical_source_payload=source_payload)
                with self.assertRaisesRegex(ValueError, "model content identity"):
                    invalid.ensure(candidate, (role,), deadline_ms=2**62)
                self.assertEqual(calls, [])
        finally:
            metadata["canonicalSourceDataName"] = original_name
        original_metadata = dict(metadata)
        initializer_payload = b"external-weights"
        try:
            initializer_name = canonical_model_object_name(
                publisher="/publisher", model_name="Qwen/Qwen3-0.6B", model_digest=model.digest,
                object_kind="initializer", object_label="initializers",
                object_digest=_digest(initializer_payload))
            metadata.update({
                "canonicalInitializerDataName": initializer_name,
                "canonicalInitializerObjectDigest": _digest(initializer_payload),
                "canonicalInitializerBytes": len(initializer_payload),
            })
            for failure in ("missing", "digest", "size", "owner"):
                with self.subTest(initializer_failure=failure):
                    metadata["canonicalInitializerObjectDigest"] = (
                        _digest(b"wrong") if failure == "digest"
                        else _digest(initializer_payload))
                    metadata["canonicalInitializerDataName"] = canonical_model_object_name(
                        publisher="/publisher-other" if failure == "owner" else "/publisher",
                        model_name="Qwen/Qwen3-0.6B", model_digest=model.digest,
                        object_kind="initializer", object_label="initializers",
                        object_digest=metadata["canonicalInitializerObjectDigest"])
                    metadata["canonicalInitializerBytes"] = (
                        len(initializer_payload) + 1 if failure == "size"
                        else len(initializer_payload))
                    invalid = CanonicalCatalogEnsurer(
                        catalog, publish_object, publisher="/publisher",
                        canonical_source_payload=source_payload,
                        canonical_initializer_payload=(
                            None if failure == "missing" else initializer_payload))
                    with self.assertRaises(ValueError):
                        invalid.ensure(candidate, (role,), deadline_ms=2**62)
                    self.assertEqual(calls, [])
                    self.assertEqual(invalid._published_source_name, "")
        finally:
            metadata.clear()
            metadata.update(original_metadata)
        ensurer.ensure(
            SimpleNamespace(candidate_digest=_digest(b"placement")), (role,),
            deadline_ms=2**62)
        self.assertEqual(calls[0]["name"], source_name)
        self.assertEqual(calls[0]["payload"], source_payload)
        self.assertEqual(calls[-1]["name"], catalog.active_manifest.name)
        from ndn.encoding import Name
        expected_key = _framed_digest("ndnsf-naming/sdk-publication", [
            (1, 1, bytes(Name.encode(Name.from_str(source_name)))),
            (2, 4, hashlib.sha256(source_payload).digest())])
        self.assertEqual(calls[0]["idempotency_key"], expected_key)
        count = len(calls)
        ensurer.ensure(SimpleNamespace(candidate_digest=_digest(b"warm-placement")),
                       (role,), deadline_ms=2**62)
        self.assertEqual(len(calls), count)

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
        initializer_name = canonical_model_object_name(
            publisher="/publisher", model_name="YOLO26n", model_digest=model.digest,
            object_kind="initializer", object_label="initializers",
            object_digest=_digest(initializer_payload))
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
        from ndn.encoding import Name
        expected_key = _framed_digest("ndnsf-naming/sdk-publication", [
            (1, 1, bytes(Name.encode(Name.from_str(initializer_name)))),
            (2, 4, hashlib.sha256(initializer_payload).digest())])
        self.assertEqual(calls[0]["idempotency_key"], expected_key)


if __name__ == "__main__":
    unittest.main()
