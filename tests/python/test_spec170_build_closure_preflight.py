from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "packaging/ndnsf-di-container/oci/scripts/preflight-gpu-build.py"


def load_preflight():
    spec = importlib.util.spec_from_file_location("spec170_gpu_preflight", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


preflight = load_preflight()


class Spec170BuildClosurePreflightTests(unittest.TestCase):
    def _workspace(self, use: str) -> Path:
        root = Path(tempfile.mkdtemp(prefix="spec170-preflight-"))
        (root / "examples").mkdir()
        (root / "NDNSF-DistributedInference/cpp/adapters/onnx").mkdir(
            parents=True
        )
        (root / "examples/DI_NativePlanOnnxSmoke.cpp").touch()
        (root / "examples/DI_NativeOnnxRuntimeSmoke.cpp").touch()
        (root / "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp").touch()
        (root / "examples/wscript").write_text(
            "\n".join(
                [
                    "bld.program(name='di-native-plan-onnx-smoke', source=['DI_NativePlanOnnxSmoke.cpp'], use='%s')" % use,
                    "bld.program(name='di-native-onnxruntime-smoke', source=['DI_NativeOnnxRuntimeSmoke.cpp'], use='%s')" % use,
                    "bld.program(name='di-native-provider', source=['DI_NativeProviderExecutable.cpp'], use='%s')" % use,
                    "bld.program(name='di-native-fault-provider', source=['DI_NativeFaultProviderExecutable.cpp'], use='%s')" % use,
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        return root

    def test_both_onnx_smoke_targets_require_framework_use_closure(self) -> None:
        root = self._workspace("BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL")
        census = preflight.validate_waf_target_closure(root)
        self.assertEqual(len(census), 4)

    def test_missing_ndn_svs_dependency_is_fail_closed(self) -> None:
        root = self._workspace("BOOST NDN_CXX ONNXRUNTIME DL")
        with self.assertRaisesRegex(
            preflight.PreflightError,
            r"PREFLIGHT_TARGET_USE_MISSING:.*:NDN_SVS",
        ):
            preflight.validate_waf_target_closure(root)

    def test_stale_python_build_tree_is_fail_closed(self) -> None:
        root = self._workspace("BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL")
        build = root / "NDNSF-DistributedInference/packaging/python/app/build"
        build.mkdir(parents=True)
        with self.assertRaisesRegex(
            preflight.PreflightError, "PREFLIGHT_STALE_PYTHON_BUILD_DIRS"
        ):
            preflight.validate_python_build_outputs(root)


if __name__ == "__main__":
    unittest.main()
