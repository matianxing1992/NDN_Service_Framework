"""Guard that the deployed native YOLO merge carries no Python runtime import.

The model-backed equivalence cases were retired with the two-stage Python YOLO
inference path; the C++ merge contract itself stays covered by
`distributed-inference-native-yolo-merge.t.cpp` in `spec181-native-plan-closure`.
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_deployed_native_merge_source_has_no_python_yolo_runtime_import():

    source = (ROOT / "NDNSF-DistributedInference/cpp/adapters/yolo"
              / "NativeYoloMergeRunner.cpp").read_text(encoding="utf-8")
    assert "import yolo" not in source
    assert "python" not in source.lower() or "no python" in source.lower()
