"""Verify that the offline merge preserves ONNX's real external tensor reads."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

try:
    import onnx
    from onnx import TensorProto, helper
except ImportError:
    onnx = None

TOOL = Path(__file__).resolve().parents[2] / "tools/ndnsf-di/merge-onnx-external-data.py"
EXPERIMENTS = TOOL.parents[2] / "Experiments"


@unittest.skipIf(onnx is None, "offline ONNX preparation tools are not installed")
class ExternalDataMergeTest(unittest.TestCase):
    def fixture(self, root, *, stale_shape=False):
        inputs = []
        for name, sidecar, value in (("a", "weights_b", 2.), ("b", "weights_a", -3.)):
            (root / sidecar).write_bytes(b"pad" + struct.pack("<f", value) + b"tail")
            tensor = helper.make_tensor(name, TensorProto.FLOAT, [1], struct.pack("<f", value), raw=True)
            onnx.external_data_helper.set_external_data(tensor, sidecar, offset=3, length=4)
            tensor.ClearField("raw_data")
            inputs.append(tensor)
        outputs = [helper.make_tensor_value_info(t.name, TensorProto.FLOAT, [1]) for t in inputs]
        nodes, data_inputs, value_info = [], [], []
        if stale_shape:
            shape = helper.make_tensor("target", TensorProto.INT64, [2], [2, 3])
            nodes = [helper.make_node("Constant", [], ["target"], value=shape),
                     helper.make_node("Reshape", ["data", "target"], ["reshaped"])]
            data_inputs = [helper.make_tensor_value_info("data", TensorProto.FLOAT, [6])]
            outputs.append(helper.make_tensor_value_info("reshaped", TensorProto.FLOAT, None))
            value_info = [helper.make_tensor_value_info("target", TensorProto.INT64, [])]
        graph = helper.make_model(helper.make_graph(nodes, "fixture", data_inputs, outputs,
                                                    initializer=inputs, value_info=value_info))
        (root / "source.onnx").write_bytes(graph.SerializeToString())
        pins = {}
        for path in root.iterdir():
            data = path.read_bytes()
            pins[path.name] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        (root / "pins.json").write_text(json.dumps(pins))

    def merge(self, root, output, *flags):
        return subprocess.run([sys.executable, str(TOOL), "--source-root", str(root),
                               "--graph", "source.onnx", "--commitments", str(root / "pins.json"),
                               "--output", str(output), *flags], capture_output=True, text=True)

    def test_preserves_values_and_applies_source_offsets(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            output = root / "out"
            result = self.merge(root, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = onnx.load(str(output / "model.onnx"))
            self.assertEqual([struct.unpack("<f", t.raw_data)[0] for t in result.graph.initializer], [2., -3.])
            receipt = json.loads((output / "receipt.json").read_text())
            self.assertEqual(receipt["files"]["model.onnx_data"]["bytes"], 22)
            self.assertNotEqual(self.merge(root, output).returncode, 0)
            self.assertEqual(receipt, json.loads((output / "receipt.json").read_text()))

    def test_corrupt_input_removes_only_unsealed_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            corrupt = root / "weights_b"
            corrupt.write_bytes(b"x" * corrupt.stat().st_size)
            output = root / "out"
            result = self.merge(root, output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("external file digest mismatch", result.stderr)
            self.assertFalse(output.exists())
            self.assertTrue((root / "source.onnx").is_file())

    def test_optional_normalization_corrects_stale_constant_rank(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root, stale_shape=True)
            output = root / "out"
            result = self.merge(root, output, "--normalize-shapes")
            self.assertEqual(result.returncode, 0, result.stderr)
            model = onnx.load(str(output / "model.onnx"))
            onnx.checker.check_model(model)
            self.assertEqual([node.op_type for node in model.graph.node], ["Reshape"])
            self.assertEqual([dim.dim_value for dim in model.graph.output[-1]
                              .type.tensor_type.shape.dim], [2, 3])
            self.assertEqual([struct.unpack("<f", t.raw_data)[0]
                              for t in model.graph.initializer[:2]], [2., -3.])
            receipt = json.loads((output / "receipt.json").read_text())
            self.assertEqual(receipt["shape_normalization"]["constants"], 1)

    def test_shared_preparer_seals_and_reopens_external_fp16_state_package(self):
        spec = importlib.util.spec_from_file_location(
            "prepare_external_model_assets", EXPERIMENTS / "prepare-model-assets.py")
        prepare = importlib.util.module_from_spec(spec)
        with patch.object(sys, "path", [str(EXPERIMENTS), *sys.path]):
            spec.loader.exec_module(prepare)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            source.mkdir()
            self.fixture(source)
            graph = onnx.load(source / "source.onnx", load_external_data=False)
            for name in ("input_ids", "attention_mask", "position_ids"):
                graph.graph.input.append(helper.make_tensor_value_info(name, TensorProto.INT64, [1, "T"]))
            graph.graph.node.append(helper.make_node("Identity", ["a"], ["logits"]))
            graph.graph.output.append(helper.make_tensor_value_info("logits", TensorProto.FLOAT, [1]))
            for kind in ("key", "value"):
                past, present = f"past_key_values.0.{kind}", f"present.0.{kind}"
                for values, name in ((graph.graph.input, past), (graph.graph.output, present)):
                    values.append(helper.make_tensor_value_info(name, TensorProto.FLOAT16, [1, 2, "P", 3]))
                graph.graph.node.append(helper.make_node("Identity", [past], [present]))
            (source / "source.onnx").write_bytes(graph.SerializeToString())
            (source / "LICENSE").write_text("synthetic test input")

            def commitment(file, path):
                blob = file.read_bytes()
                return dict(path=path, bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest())

            source_files = dict(sourceModel="source.onnx", valuesA="weights_a", valuesB="weights_b")
            files = {role: commitment(source / name, "source/" + name)
                     for role, name in source_files.items()}
            (source / "pins.json").write_text(json.dumps({
                name: {key: files[role][key] for key in ("bytes", "sha256")}
                for role, name in source_files.items()}))
            expected = root / "expected"
            merged = self.merge(source, expected)
            self.assertEqual(merged.returncode, 0, merged.stderr)
            files.update(canonicalGraph=commitment(expected / "model.onnx", "canonical/model.onnx"),
                         canonicalWeights=commitment(expected / "model.onnx_data", "canonical/model.onnx_data"),
                         license=commitment(source / "LICENSE", "license/LICENSE"))
            normalizer = TOOL.with_name("normalize_onnx_constants.py")
            recipe = dict(assetId="synthetic-external", recipeVersion=1, repository="test/fixture",
                          revision="a" * 40, license={}, toolVersions={}, files=files,
                          sourceFiles=source_files,
                          externalOnnx=dict(sidecarRoles=["valuesA", "valuesB"], normalizeShapes=False),
                          transforms=[dict(tool=prepare.MERGE_TOOL, sha256=prepare.file_digest(TOOL),
                                           normalizerSha256=prepare.file_digest(normalizer))],
                          modelContract=dict(nodes=3, inputs=5, outputs=5, layers=1, kvDtype="FLOAT16",
                                             kvHeads=2, headDimension=3,
                                             matMulNBits={"count": 0, "bits": 4, "block_size": 32}))
            recipe_path = root / "recipe.json"
            recipe_path.write_text(json.dumps(recipe))
            package = root / "assets"
            args = ["--recipe-file", str(recipe_path), "--asset-root", str(package), "--offline"]
            with patch.object(prepare.shutil, "disk_usage", return_value=SimpleNamespace(free=5 << 30)):
                self.assertEqual(prepare.main(args + ["--source-dir", str(source)]), 0)
            self.assertEqual(prepare.main(args), 0)
            self.assertTrue((package / "source/weights_a").samefile(source / "weights_a"))
            self.assertEqual((package / "canonical/model.onnx_data").read_bytes(),
                             (source / "weights_a").read_bytes() + (source / "weights_b").read_bytes())


if __name__ == "__main__":
    unittest.main()
