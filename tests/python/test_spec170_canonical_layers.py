from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.adapters.qwen.canonical_layers import (  # noqa: E402
    CanonicalLayerCatalog, canonical_layer_manifest,
)


M = "sha256:" + "1" * 64
G = "sha256:" + "2" * 64
R = "sha256:" + "3" * 64


class Spec170CanonicalLayersTest(unittest.TestCase):
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

    def test_repo_adapter_is_root_last_and_idempotent(self):
        payload = b"layer-0"
        manifest = canonical_layer_manifest(
            model_name="Qwen/Qwen3-0.6B", model_digest=M, profile="fp16",
            graph_digest=G, role_kind="pipeline", layer_begin=0, layer_end=2,
            rank=0, recipe_digest=R, payload=payload, publisher="/publisher")
        catalog = CanonicalLayerCatalog()
        catalog.publish_layer(manifest, payload)
        calls = []

        def publish_object(**kwargs):
            calls.append(kwargs)
            return kwargs["name"]

        root = catalog.publish_via(publish_object, publisher="/publisher")
        self.assertIn("/ROOT/sha256:", root)
        self.assertEqual(len(calls), 2)
        self.assertTrue(calls[-1]["name"].endswith(root.split("/ROOT/", 1)[1]))


if __name__ == "__main__":
    unittest.main()
