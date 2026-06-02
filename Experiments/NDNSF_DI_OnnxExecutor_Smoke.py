#!/usr/bin/env python3
"""Smoke-test the dependency-driven ONNX executor with a toy fan-in/fan-out DAG."""

from __future__ import annotations

import argparse
from concurrent.futures import Future
from pathlib import Path
import tempfile

import numpy as np
import torch

from ndnsf_distributed_inference import (
    DependencyEdge,
    ProviderRuntimeContext,
    RoleDependencyView,
    execute_onnx_dependency_chunk,
)


class _Ref:
    def __init__(self, payload: bytes):
        self.payload = payload


class _FakeNdnsfContext:
    def __init__(self):
        self.large_objects: dict[str, bytes] = {}
        self.refs: dict[tuple[str, str], bytes] = {}
        self._next_large = 0

    def publish_large(self, key_scope: str, topic: str, payload: bytes,
                      *args, **kwargs) -> str:
        name = f"/fake/large/{self._next_large}"
        self._next_large += 1
        self.large_objects[name] = payload
        return name

    def publish(self, key_scope: str, topic: str, payload: bytes) -> None:
        self.refs[(key_scope, topic)] = payload

    def wait_one(self, key_scope: str, topic: str, timeout_ms: int):
        payload = self.refs.get((key_scope, topic))
        if payload is None:
            return None
        return _Ref(payload)

    def fetch_large(self, data_name: str, key_scope: str, timeout_ms: int):
        return self.large_objects.get(data_name)


class _SynchronousPrefetcher:
    def __init__(self, ndnsf: _FakeNdnsfContext):
        self.ndnsf = ndnsf

    def prefetch_large(self, edge: DependencyEdge, topic_suffix: str = "",
                       *, ref_timeout_ms: int = 10000,
                       fetch_timeout_ms: int = 10000) -> Future:
        future: Future = Future()
        ref = self.ndnsf.wait_one(edge.key_scope, edge.topic(topic_suffix),
                                  ref_timeout_ms)
        if ref is None:
            future.set_exception(
                TimeoutError(f"missing ref {edge.key_scope} {edge.topic(topic_suffix)}"))
            return future
        payload = self.ndnsf.fetch_large(ref.payload.decode(), edge.key_scope,
                                        fetch_timeout_ms)
        if payload is None:
            future.set_exception(
                TimeoutError(f"missing large object {ref.payload.decode()}"))
            return future
        future.set_result(payload)
        return future


class Source(torch.nn.Module):
    def forward(self, x):
        return x + 1.0, x * 2.0


class Left(torch.nn.Module):
    def forward(self, a):
        return a * 3.0


class Right(torch.nn.Module):
    def forward(self, b):
        return b + 5.0


class Join(torch.nn.Module):
    def forward(self, left, right):
        return left + right


def _export(path: Path, module: torch.nn.Module, inputs, input_names, output_names) -> None:
    torch.onnx.export(
        module.eval(),
        inputs,
        str(path),
        input_names=input_names,
        output_names=output_names,
        opset_version=17,
        do_constant_folding=True,
    )


def _ctx(role: str, view: RoleDependencyView,
         ndnsf: _FakeNdnsfContext) -> ProviderRuntimeContext:
    return ProviderRuntimeContext(
        ndnsf=ndnsf,
        execution=object(),
        request=b"",
        role=role,
        dependencies=view,
        prefetcher=_SynchronousPrefetcher(ndnsf),
    )


def run_smoke(work_dir: Path) -> None:
    work_dir.mkdir(parents=True, exist_ok=True)
    x = torch.tensor([[1.0, 2.0, 3.0]], dtype=torch.float32)
    _export(work_dir / "source.onnx", Source(), x, ["x"], ["a", "b"])
    _export(work_dir / "left.onnx", Left(), x, ["a"], ["left"])
    _export(work_dir / "right.onnx", Right(), x, ["b"], ["right"])
    _export(work_dir / "join.onnx", Join(), (x, x), ["left", "right"], ["y"])

    source_to_lr = DependencyEdge(
        producers=["/Source"],
        consumers=["/Left", "/Right"],
        key_scope="source-fanout",
        topic_prefix="/activation",
        tensors=["a", "b"],
    )
    left_to_join = DependencyEdge(
        producers=["/Left"],
        consumers=["/Join"],
        key_scope="left-to-join",
        topic_prefix="/activation",
        tensors=["left"],
    )
    right_to_join = DependencyEdge(
        producers=["/Right"],
        consumers=["/Join"],
        key_scope="right-to-join",
        topic_prefix="/activation",
        tensors=["right"],
    )

    ndnsf = _FakeNdnsfContext()
    source = _ctx("/Source", RoleDependencyView(
        role="/Source",
        outputs=[source_to_lr],
    ), ndnsf)
    left = _ctx("/Left", RoleDependencyView(
        role="/Left",
        inputs=[source_to_lr],
        outputs=[left_to_join],
    ), ndnsf)
    right = _ctx("/Right", RoleDependencyView(
        role="/Right",
        inputs=[source_to_lr],
        outputs=[right_to_join],
    ), ndnsf)
    join = _ctx("/Join", RoleDependencyView(
        role="/Join",
        inputs=[left_to_join, right_to_join],
    ), ndnsf)

    execute_onnx_dependency_chunk(source, work_dir / "source.onnx",
                                  initial_values={"x": x.numpy()})
    execute_onnx_dependency_chunk(left, work_dir / "left.onnx")
    execute_onnx_dependency_chunk(right, work_dir / "right.onnx")
    result = execute_onnx_dependency_chunk(join, work_dir / "join.onnx")

    expected = ((x.numpy() + 1.0) * 3.0) + ((x.numpy() * 2.0) + 5.0)
    actual = result.value("y")
    max_diff = float(np.max(np.abs(actual - expected)))
    if max_diff > 1e-6:
        raise RuntimeError(f"fan-in/fan-out result mismatch: max_diff={max_diff}")
    print(f"ONNX_EXECUTOR_FANIN_FANOUT_OK shape={actual.shape} max_diff={max_diff:.8f}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Smoke-test NDNSF-DI ONNX executor fan-in/fan-out support")
    parser.add_argument("--work-dir", default="")
    args = parser.parse_args()
    if args.work_dir:
        run_smoke(Path(args.work_dir))
    else:
        with tempfile.TemporaryDirectory(prefix="ndnsf-di-onnx-executor-") as tmp:
            run_smoke(Path(tmp))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
