from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINDINGS = ROOT / "pythonWrapper/src/ndnsf/di_bindings.cpp"
MODULE = ROOT / "pythonWrapper/src/ndnsf/_ndnsf.cpp"
SETUP = ROOT / "pythonWrapper/setup.py"
REQUESTER = ROOT / "examples/DI_NativeRequester.cpp"


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
        # A Python callback is exposed only as a terminal observer on an
        # already-created native handle; planning and execution remain native.
        self.assertIn("py::function callback", source)
        self.assertIn('.def("observe"', source)
        self.assertNotIn("subprocess", source)

    def test_binding_source_has_no_python_strategy_trampoline(self):
        source = BINDINGS.read_text(encoding="utf-8")
        self.assertNotIn("subprocess", source)
        self.assertNotIn("py::eval", source)


    def test_native_requester_cli_uses_prepared_runtime_owner(self):
        source = REQUESTER.read_text(encoding="utf-8")
        # Spec185 moved the executable to the prepared-runtime owner.  Keep
        # this inherited test focused on the public native route rather than
        # requiring the retired hand-built runtime-parser path.
        self.assertIn("Runtime::open", source)
        self.assertIn("user.prepare", source)
        self.assertIn("user.request(prepared", source)
        self.assertNotIn("NativeRequestRuntime runtime;", source)
        self.assertIn("options.applicationRequestId", source)

    def test_native_provider_target_has_relocatable_origin_runpath(self):
        source = (ROOT / "examples/wscript").read_text(encoding="utf-8")
        target_start = source.index("bld.program(name='di-native-provider'")
        target_end = source.index(
            "bld.program(name='di-native-fault-provider'", target_start)
        target = source[target_start:target_end]
        self.assertIn(
            "linkflags=['-pthread', '-Wl,-rpath,$ORIGIN/..']", target)


if __name__ == "__main__":
    unittest.main()


def test_public_runtime_surface_has_no_retired_composition_types():
    from ndnsf import _ndnsf
    from ndnsf.service import ServiceUser
    from ndnsf_distributed_inference import api

    assert api.Runtime is _ndnsf.Runtime
    assert api.RequestHandle is _ndnsf.RequestHandle
    assert api.ProviderConfig is _ndnsf.NativeProviderConfig
    for name in ("NativeInferenceClient", "NativeRequestOptions", "NativeRequestRuntime",
                 "NativeCanonicalPreparationCatalog", "NativeConversationCoordinator"):
        assert not hasattr(_ndnsf, name), name
    for name in ("load_native_request_catalog", "native_preparation", "native_runtime_from_config",
                 "native_grant_client_from_config", "native_inference_client_configured",
                 "native_conversation_coordinator_from_config"):
        assert not hasattr(ServiceUser, name), name
    for name in ("native_inference_client", "native_preparation", "native_grant_client_from_config",
                 "native_inference_client_configured", "native_conversation_coordinator_from_config"):
        assert not hasattr(_ndnsf.NativeServiceUser, name), name
