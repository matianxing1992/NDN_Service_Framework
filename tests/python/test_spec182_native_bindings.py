from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINDINGS = ROOT / "pythonWrapper/src/ndnsf/di_bindings.cpp"
MODULE = ROOT / "pythonWrapper/src/ndnsf/_ndnsf.cpp"
SETUP = ROOT / "pythonWrapper/setup.py"


class Spec182NativeBindingsTest(unittest.TestCase):
    def test_binding_is_a_single_native_translation_unit_entry(self):
        source = BINDINGS.read_text(encoding="utf-8")
        module = MODULE.read_text(encoding="utf-8")
        setup = SETUP.read_text(encoding="utf-8")
        self.assertIn("bindDistributedInference(py::module_& module)", source)
        self.assertIn("bindDistributedInference(m);", module)
        self.assertIn('"src/ndnsf/di_bindings.cpp"', setup)
        self.assertIn('"ndnsf-distributed-inference"', setup)
        self.assertNotIn("NativeGrantVerifier.cpp", setup)
        # The DI binding must not introduce a Python callback strategy or a
        # second planner implementation.  Native callbacks remain an explicit
        # C++ API concern and are deliberately not exported here.
        self.assertNotIn("py::function", source)
        self.assertNotIn("subprocess", source)

    def test_binding_source_has_no_python_strategy_trampoline(self):
        source = BINDINGS.read_text(encoding="utf-8")
        self.assertNotIn("subprocess", source)
        self.assertNotIn("py::eval", source)

    def test_native_dto_names_are_explicitly_mapped(self):
        source = BINDINGS.read_text(encoding="utf-8")
        for field in (
                "model_name", "content_digest", "adapter_id", "payload",
                "transport_mode", "timeout_ms", "ack_timeout_ms"):
            self.assertIn(f'"{field}"', source)


if __name__ == "__main__":
    unittest.main()
