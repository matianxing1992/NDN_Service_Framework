from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest

import numpy as np
import onnx
import onnxruntime as ort
import torch


ROOT = Path(__file__).resolve().parents[2]
MODEL = ROOT / "NDNSF-UAV-APP/models/mvcnn_vehicle_cpu.onnx"
CHECKPOINT = ROOT / "NDNSF-UAV-APP/models/mvcnn_vehicle_cpu.pt"
REGISTRY = ROOT / "NDNSF-UAV-APP/configs/uav_multiview_models.json"
TOOLS = ROOT / "NDNSF-UAV-APP/tools"
sys.path.insert(0, str(TOOLS))
from mvcnn_model import make_model  # noqa: E402
from prepare_mvcnn_artifact import render  # noqa: E402


class MvcnnOnnxContractTest(unittest.TestCase):
    def test_graph_has_explicit_joint_inputs_and_outputs(self) -> None:
        onnx.checker.check_model(str(MODEL))
        session = ort.InferenceSession(str(MODEL), providers=["CPUExecutionProvider"])
        self.assertEqual(session.get_providers(), ["CPUExecutionProvider"])
        self.assertEqual({value.name for value in session.get_inputs()}, {"images", "viewMask"})
        self.assertEqual({value.name for value in session.get_outputs()}, {"logits", "pooledFeatures"})

    def test_masked_slots_do_not_change_joint_representation(self) -> None:
        session = ort.InferenceSession(str(MODEL), providers=["CPUExecutionProvider"])
        rng = np.random.default_rng(178)
        images = rng.random((1, 6, 3, 224, 224), dtype=np.float32)
        mask = np.array([[True, True, False, False, False, False]], dtype=np.bool_)
        padded = images.copy()
        padded[:, 2:] = rng.random((1, 4, 3, 224, 224), dtype=np.float32)
        first = session.run(None, {"images": images, "viewMask": mask})
        second = session.run(None, {"images": padded, "viewMask": mask})
        np.testing.assert_allclose(first[0], second[0], rtol=1e-5, atol=1e-6)
        np.testing.assert_allclose(first[1], second[1], rtol=1e-5, atol=1e-6)

    def test_valid_view_permutation_is_equivalent(self) -> None:
        session = ort.InferenceSession(str(MODEL), providers=["CPUExecutionProvider"])
        rng = np.random.default_rng(179)
        images = rng.random((1, 6, 3, 224, 224), dtype=np.float32)
        mask = np.array([[True, True, True, True, False, False]], dtype=np.bool_)
        permuted = images.copy()
        permuted[:, :4] = images[:, [3, 1, 0, 2]]
        first = session.run(None, {"images": images, "viewMask": mask})
        second = session.run(None, {"images": permuted, "viewMask": mask})
        np.testing.assert_allclose(first[0], second[0], rtol=1e-5, atol=1e-6)
        np.testing.assert_allclose(first[1], second[1], rtol=1e-5, atol=1e-6)

    def test_native_checkpoint_and_onnx_outputs_match(self) -> None:
        checkpoint = torch.load(CHECKPOINT, map_location="cpu", weights_only=True)
        native = make_model(class_count=3, feature_dim=32)
        native.load_state_dict(checkpoint["state_dict"])
        native.eval()
        native_images = torch.stack([
            torch.nn.functional.interpolate(
                torch.stack([render(0, 0, view) for view in range(6)]),
                size=(224, 224), mode="bilinear", align_corners=False)
        ])
        mask = torch.ones((1, 6), dtype=torch.bool)
        with torch.no_grad():
            native_logits, native_pooled = native(native_images, mask)
        session = ort.InferenceSession(str(MODEL), providers=["CPUExecutionProvider"])
        exported_logits, exported_pooled = session.run(
            ["logits", "pooledFeatures"],
            {"images": native_images.numpy(), "viewMask": mask.numpy()})
        np.testing.assert_allclose(native_logits.numpy(), exported_logits, rtol=1e-4, atol=1e-5)
        np.testing.assert_allclose(native_pooled.numpy(), exported_pooled, rtol=1e-4, atol=1e-5)

    def test_registry_contract_is_not_yolo_or_cuda(self) -> None:
        registry = json.loads(REGISTRY.read_text())
        profile = registry["profiles"][0]
        self.assertEqual(profile["algorithm_id"], "mvcnn-onnx-maxpool/v1")
        self.assertNotIn("yolo", profile["model_id"].lower())
        self.assertEqual(profile["device_class"], "cpu")
        self.assertEqual(profile["execution_provider"], "CPUExecutionProvider")


if __name__ == "__main__":
    unittest.main()
