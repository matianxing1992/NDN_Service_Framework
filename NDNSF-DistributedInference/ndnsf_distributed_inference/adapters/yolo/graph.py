"""YOLO26n canonical ONNX graph port.

This module is deliberately an adapter boundary: it reads the offline
canonical package and exposes the existing model-neutral graph contract.  It
does not import PyTorch or Ultralytics and never chooses a Provider.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Any

from ..base import AdapterPortDescriptor
from ...splitter import AdapterDescriptor, ModelDescriptor, ModelGraphSnapshot
from ..onnx.graph import analyze_onnx_graph, to_model_graph_snapshot


@dataclass(frozen=True)
class Yolo26GraphAdapter:
    descriptor: AdapterPortDescriptor
    snapshot: ModelGraphSnapshot
    manifest: Mapping[str, Any]

    @property
    def graph_digest(self) -> str:
        return self.snapshot.graph_digest

    def inspect(self, model: ModelDescriptor) -> ModelGraphSnapshot:
        if model.model_name != "YOLO26n":
            raise ValueError("YOLO26n graph adapter received another model")
        model.validate_graph(self.snapshot)
        return self.snapshot


def load_yolo_graph(
    graph_path: str | Path,
    adapter_descriptor: AdapterDescriptor,
    *,
    manifest: Mapping[str, Any] | None = None,
) -> Yolo26GraphAdapter:
    """Load and validate one canonical graph without model-framework imports."""

    path = Path(graph_path)
    if not path.is_file():
        raise ValueError(f"canonical YOLO graph is missing: {path}")
    summary = analyze_onnx_graph(path)
    if (tuple(summary.inputs) != ("images",)
            or tuple(summary.outputs) != ("predictions",)
            or len(summary.nodes) < 4):
        raise ValueError("canonical YOLO graph has an ambiguous interface")
    snapshot = to_model_graph_snapshot(summary, adapter_descriptor)
    port = AdapterPortDescriptor(
        "yolo26n-onnx-graph", "1", adapter_descriptor.graph_schema_digest,
    )
    return Yolo26GraphAdapter(port, snapshot, manifest or {})
