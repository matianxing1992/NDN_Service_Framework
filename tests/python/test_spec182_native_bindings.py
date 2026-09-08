from __future__ import annotations

from pathlib import Path
import unittest
import sys


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

    def test_native_runtime_composition_types_are_exported(self):
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        from ndnsf import _ndnsf

        runtime = _ndnsf.NativeRequestRuntime()
        runtime.contract = _ndnsf.NativeRequestContract()
        runtime.security = _ndnsf.NativeSecurityPolicySnapshot()
        runtime.budget = _ndnsf.NativeCandidateBudget()
        runtime.state_mapping = _ndnsf.NativeStateTensorMapping()
        self.assertIsInstance(runtime, _ndnsf.NativeRequestRuntime)
        for name in (
                "NativeRequestCatalog", "NativeRequestPreparation",
                "NativeCanonicalPreparationCatalog", "NativeOfferAdmission",
                "NativeAuthenticatedGrantClient", "native_request_runtime_from_json"):
            self.assertTrue(hasattr(_ndnsf, name), name)

    def test_runtime_config_facade_is_a_thin_native_pass_through(self):
        source = (ROOT / "pythonWrapper/ndnsf/service.py").read_text(encoding="utf-8")
        self.assertIn("def native_runtime_from_config", source)
        self.assertIn("_ndnsf.native_request_runtime_from_json", source)
        self.assertIn("NativeRequestRuntime nativeRequestRuntimeFromJson",
                      (ROOT / "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.cpp").read_text(encoding="utf-8"))

    def test_catalog_loader_keeps_source_validation_native(self):
        sys.path.insert(0, str(ROOT / "pythonWrapper"))
        from ndnsf import _ndnsf

        with self.assertRaisesRegex(ValueError, "unsupported native request catalog schema"):
            _ndnsf.NativeRequestCatalog.load('{"schema":"invalid"}', b"")

        # The binding accepts bytes at the Python boundary; source parsing and
        # digest/format validation remain in NativeRequestCatalog::load.
        self.assertIn("NativeRequestCatalog::load", BINDINGS.read_text(encoding="utf-8"))
        native_user = MODULE.read_text(encoding="utf-8")
        self.assertIn("native_inference_client_configured", native_user)
        self.assertIn("native_grant_client_from_config", native_user)

    def test_native_grant_config_binds_requester_to_service_user(self):
        source = MODULE.read_text(encoding="utf-8")
        self.assertIn("requester != m_userIdentity", source)
        self.assertIn(
            "native grant requester identity must match the ServiceUser identity",
            source)


if __name__ == "__main__":
    unittest.main()
