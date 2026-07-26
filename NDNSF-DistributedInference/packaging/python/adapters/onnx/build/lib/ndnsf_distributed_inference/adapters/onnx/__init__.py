"""ONNX adapter registration; graph/runtime dependencies remain lazy."""


def load_graph():
    from . import graph
    return graph


def load_executor():
    """Load ONNX Runtime-dependent execution helpers only when requested."""
    from . import executor
    return executor


__all__ = ["load_executor", "load_graph", "register"]

def register(registry, adapter):
    registry.register(adapter); return adapter

__all__ = ["register"]
