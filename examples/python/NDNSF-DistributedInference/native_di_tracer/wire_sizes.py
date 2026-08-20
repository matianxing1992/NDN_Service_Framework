"""Canonical wire-size helpers shared by the NativeTracer plan generator."""

from __future__ import annotations


def encoded_tensor_bundle_size(
    name: str, shape: tuple[int, ...], payload_bytes: int
) -> int:
    """Return the exact one-tensor TensorBundleCodec wire size."""
    if not name or payload_bytes < 0 or any(d < 0 for d in shape):
        raise ValueError("invalid tensor wire-size inputs")
    # "NDITB001" + tensor count, then name length/name, element type, rank,
    # dimensions, payload length, and payload. Each scalar is fixed width.
    return (
        8
        + 4
        + 4
        + len(name)
        + 4
        + 4
        + 8 * len(shape)
        + 8
        + payload_bytes
    )


def padded_tensor_payload_bytes(pad_bytes: int) -> int:
    """Match the float32 padding tensor rounding in the C++ runner."""
    if pad_bytes < 0:
        raise ValueError("padding bytes must be non-negative")
    return ((pad_bytes + 3) // 4) * 4
