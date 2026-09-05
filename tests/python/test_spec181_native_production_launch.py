"""A01 process selection regression; this does not execute MiniNDN."""
from test_spec180_yolo_minindn import _binding_inputs, load_runner


def test_protected_y_b_selects_native_production_chain(tmp_path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    inputs["protection_epoch"] = "spec180-yolo-protected-v1"
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    specs = module.MiniNdnCaseRuntime(binding, inputs).process_specs("providers")
    assert len(specs) == 4
    for process in specs:
        assert "di-native-provider" in process.command
        assert "provider.py" not in process.command
        assert process.ready_marker == "NDNSF_DI_NATIVE_PROVIDER_READY"
        assert process.runtime == "native"
