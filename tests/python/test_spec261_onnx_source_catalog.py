"""The maintained ONNX_SOURCE catalog generator must read source facts.

A catalog that restates Conv geometry by hand can drift from the graph and
still validate; this generator takes the inspected facts instead.
"""

import importlib.util
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Experiments"))

spec = importlib.util.spec_from_file_location(
    "native_yolo_requester", ROOT / "Experiments/native_yolo_requester.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

CONV_FACTS = {
    "index": 3,
    "op": "Conv",
    "name": "/model/model.1/conv/Conv",
    "inputs": ["/model/model.0/act/Mul_output_0", "onnx::Conv_1568", "onnx::Conv_1569"],
    "outputs": ["/model/model.1/conv/Conv_output_0"],
    "attributes": [
        {"name": "dilations", "type": 7, "ints": [1, 1]},
        {"name": "group", "type": 2, "int": 1},
        {"name": "kernel_shape", "type": 7, "ints": [3, 3]},
        {"name": "pads", "type": 7, "ints": [1, 1, 1, 1]},
        {"name": "strides", "type": 7, "ints": [2, 2]},
    ],
    "weight_dims": [32, 16, 3, 3],
}


SHAPES = {
    "/model/model.0/act/Mul_output_0": [1, 16, 320, 320],
    "/model/model.1/conv/Conv_output_0": [1, 32, 160, 160],
}


class OnnxSourceCatalogTest(unittest.TestCase):
    def test_splitter_carries_inspected_conv_facts(self):
        splitter = generator.native_onnx_source_splitter(
            boundary_node="onnx-node-3",
            roles=["Prefix", "Rank0", "Rank1", "Suffix"],
            conv=CONV_FACTS, shapes=SHAPES)
        self.assertEqual(splitter["kind"], "ONNX_SOURCE")
        self.assertEqual(splitter["activation_shape"], [1, 16, 320, 320])
        self.assertEqual(splitter["partial_shape"], [1, 32, 160, 160])
        self.assertEqual(splitter["selection_mode"], "hybrid")
        self.assertEqual(splitter["input_channels"], 16)
        self.assertEqual(splitter["conv_input_tensor"], CONV_FACTS["inputs"][0])
        self.assertEqual(splitter["conv_weight_initializer"], CONV_FACTS["inputs"][1])
        self.assertEqual(splitter["conv_bias_initializer"], CONV_FACTS["inputs"][2])
        self.assertEqual(splitter["conv_output_tensor"], CONV_FACTS["outputs"][0])
        self.assertEqual(splitter["conv_pads"], [1, 1, 1, 1])
        self.assertEqual(splitter["conv_strides"], [2, 2])
        self.assertEqual(splitter["conv_dilations"], [1, 1])

    def test_missing_bias_operand_is_not_invented(self):
        facts = json.loads(json.dumps(CONV_FACTS))
        facts["inputs"] = facts["inputs"][:2]
        splitter = generator.native_onnx_source_splitter(
            boundary_node="onnx-node-3",
            roles=["Prefix", "Rank0", "Rank1", "Suffix"],
            conv=facts, shapes=SHAPES)
        self.assertEqual(splitter["conv_bias_initializer"], "")

    def test_non_conv_facts_are_rejected(self):
        facts = json.loads(json.dumps(CONV_FACTS))
        facts["op"] = "Relu"
        with self.assertRaises(RuntimeError):
            generator.native_onnx_source_splitter(
                boundary_node="onnx-node-3",
                roles=["Prefix", "Suffix"], conv=facts, shapes=SHAPES)

    def test_program_resolver_prefers_the_installed_prefix(self):
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as root:
            prefix = Path(root) / "prefix"
            (prefix / "bin").mkdir(parents=True)
            program = prefix / "bin" / "native-source-inspect"
            program.write_text("#!/bin/sh\n", encoding="utf-8")
            program.chmod(0o755)
            previous = os.environ.get("NDNSF_TEST_RUNTIME_PREFIX")
            os.environ["NDNSF_TEST_RUNTIME_PREFIX"] = str(prefix)
            try:
                resolved = generator.native_program(ROOT, "native-source-inspect")
            finally:
                if previous is None:
                    os.environ.pop("NDNSF_TEST_RUNTIME_PREFIX", None)
                else:
                    os.environ["NDNSF_TEST_RUNTIME_PREFIX"] = previous
            self.assertEqual(resolved, program)


    def test_missing_tensor_shape_is_rejected(self):
        with self.assertRaises(RuntimeError):
            generator.native_onnx_source_splitter(
                boundary_node="onnx-node-3",
                roles=["Prefix", "Rank0", "Rank1", "Suffix"],
                conv=CONV_FACTS, shapes={})

    def test_non_rank4_shape_is_rejected(self):
        shapes = dict(SHAPES)
        shapes["/model/model.1/conv/Conv_output_0"] = [32, 160, 160]
        with self.assertRaises(RuntimeError):
            generator.native_onnx_source_splitter(
                boundary_node="onnx-node-3",
                roles=["Prefix", "Rank0", "Rank1", "Suffix"],
                conv=CONV_FACTS, shapes=shapes)


if __name__ == "__main__":
    unittest.main()
