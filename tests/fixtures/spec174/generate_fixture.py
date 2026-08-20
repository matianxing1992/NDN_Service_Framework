#!/usr/bin/env python3
"""Generate the deterministic Spec174 CPU/NDN fixture profile.

The profile deliberately keeps the graph small and inspectable.  It is a
fixture/oracle generator, not a second runtime or placement implementation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


SCHEMA = "spec174-fixture-v1"
WIDTH = 4


def sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def write_add_model(path: Path, add: np.ndarray, graph_name: str,
                    input_name: str = "x", output_name: str = "y") -> None:
    value = np.asarray(add, dtype=np.float32).reshape(1, -1)
    input_info = helper.make_tensor_value_info(
        input_name, TensorProto.FLOAT, [1, int(value.shape[1])]
    )
    output_info = helper.make_tensor_value_info(
        output_name, TensorProto.FLOAT, [1, int(value.shape[1])]
    )
    initializer = numpy_helper.from_array(value, name="constant_add")
    node = helper.make_node("Add", [input_name, "constant_add"], [output_name])
    graph = helper.make_graph(
        [node], graph_name, [input_info], [output_info], initializer=[initializer]
    )
    model = helper.make_model(
        graph, producer_name="ndnsf-spec174-fixture",
        opset_imports=[helper.make_opsetid("", 17)],
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, path)


def write_fixture(root: Path) -> dict[str, object]:
    root.mkdir(parents=True, exist_ok=True)
    canonical = root / "canonical"
    stages = root / "pipeline"
    tensor = root / "tensor"
    hybrid = root / "hybrid"

    input_value = (np.arange(WIDTH, dtype=np.float32).reshape(1, WIDTH) / 4.0)
    np.save(root / "input.npy", input_value, allow_pickle=False)
    oracle = input_value + 10.0
    np.save(root / "oracle.npy", oracle, allow_pickle=False)

    write_add_model(canonical / "unsplit.onnx", np.full(WIDTH, 10.0), "spec174-unsplit")
    for index, add in enumerate((1.0, 2.0, 3.0, 4.0)):
        write_add_model(stages / f"stage-{index}.onnx", np.full(WIDTH, add),
                        f"spec174-stage-{index}")
    write_add_model(tensor / "rank-0.onnx", np.full(WIDTH, 5.0), "spec174-rank-0")
    write_add_model(tensor / "rank-1.onnx", np.full(WIDTH, 5.0), "spec174-rank-1")
    write_add_model(hybrid / "stage-0.onnx", np.full(WIDTH, 2.0), "spec174-hybrid-0")
    write_add_model(hybrid / "stage-1-rank-0.onnx", np.full(WIDTH, 4.0), "spec174-hybrid-1-0")
    write_add_model(hybrid / "stage-1-rank-1.onnx", np.full(WIDTH, 4.0), "spec174-hybrid-1-1")
    write_add_model(hybrid / "stage-2.onnx", np.full(WIDTH, 4.0), "spec174-hybrid-2")

    model_paths = sorted(path for path in root.rglob("*.onnx"))
    fault_schedule = [
        {"case": "loss", "eventIndex": 7, "object": "segment/0"},
        {"case": "repair", "eventIndex": 8, "object": "segment/0"},
        {"case": "reorder", "eventIndex": 9, "object": "segment/1"},
        {"case": "duplicate", "eventIndex": 10, "object": "segment/1"},
        {"case": "conflicting-duplicate", "eventIndex": 11, "object": "segment/1"},
        {"case": "corruption", "eventIndex": 12, "object": "segment/2"},
        {"case": "stale-attempt", "eventIndex": 13, "object": "attempt/old"},
        {"case": "replay", "eventIndex": 14, "object": "token/old"},
        {"case": "cancellation", "eventIndex": 15, "object": "request/cancel"},
        {"case": "missing-rank", "eventIndex": 16, "object": "rank/1"},
    ]
    manifest = {
        "schemaVersion": SCHEMA,
        "seed": 174,
        "input": {"path": "input.npy", "shape": [1, WIDTH], "dtype": "float32"},
        "oracle": {"path": "oracle.npy", "operation": "input_plus_10"},
        "nameSchema": "ndnsf-di-tensor-data-v1",
        "deadlinesMs": {"ack": 1000, "selection": 1000, "request": 5000},
        "providers": [
            {"id": "/provider/0", "position": [0, 0]},
            {"id": "/provider/1", "position": [1, 0]},
            {"id": "/provider/2", "position": [0, 1]},
            {"id": "/provider/3", "position": [1, 1]},
        ],
        "pipeline": {
            "roles": [
                {"role": "/stage/0", "provider": "/provider/0", "model": "pipeline/stage-0.onnx"},
                {"role": "/stage/1", "provider": "/provider/1", "model": "pipeline/stage-1.onnx"},
                {"role": "/stage/2", "provider": "/provider/2", "model": "pipeline/stage-2.onnx"},
                {"role": "/stage/3", "provider": "/provider/3", "model": "pipeline/stage-3.onnx"},
            ],
            "oneProviderPerRole": True,
        },
        "tensorGroup": {
            "group": "/group/0", "worldSize": 2,
            "ranks": [
                {"role": "/stage/1/tp/0", "provider": "/provider/1", "model": "tensor/rank-0.onnx"},
                {"role": "/stage/1/tp/1", "provider": "/provider/2", "model": "tensor/rank-1.onnx"},
            ],
        },
        "hybrid": {"cuts": [1, 2, 1], "providers": ["/provider/0", "/provider/1", "/provider/2", "/provider/3"]},
        "faultSchedule": fault_schedule,
        "models": [{"path": path.relative_to(root).as_posix(), "sha256": sha256(path)} for path in model_paths],
    }
    (root / "fixture-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    manifest = write_fixture(args.output.resolve())
    print(json.dumps({
        "status": "PASS", "schemaVersion": SCHEMA,
        "modelCount": len(manifest["models"]),
        "faultCount": len(manifest["faultSchedule"]),
        "manifest": str((args.output.resolve() / "fixture-manifest.json")),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
