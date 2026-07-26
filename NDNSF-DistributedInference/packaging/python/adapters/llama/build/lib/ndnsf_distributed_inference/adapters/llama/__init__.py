"""Llama adapter registration without eager runtime import."""

def load_runtime():
    from . import runtime
    return runtime

__all__ = ["load_runtime"]
