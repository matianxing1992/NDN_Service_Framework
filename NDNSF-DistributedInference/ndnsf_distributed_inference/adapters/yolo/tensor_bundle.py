"""NumPy-only decoder shared by the YOLO application and offline reanalysis."""
from __future__ import annotations

import struct
import numpy as np


def decode_tensor_bundle(payload: bytes) -> dict[str, np.ndarray]:
    if not payload.startswith(b'NDITB001'):
        raise ValueError('payload is not an NDNSF-DI native tensor bundle')
    offset = 8
    def read(fmt):
        nonlocal offset
        size = struct.calcsize(fmt)
        if offset + size > len(payload):
            raise ValueError('truncated NDNSF-DI native tensor bundle')
        value = struct.unpack_from(fmt, payload, offset)[0]
        offset += size
        return value
    count = read('<I')
    if count > 256:
        raise ValueError('native tensor count exceeds limit')
    tensors = {}
    for _ in range(count):
        name_size = read('<I')
        if not 1 <= name_size <= 1024 or offset + name_size > len(payload):
            raise ValueError('invalid native tensor name')
        name = payload[offset:offset + name_size].decode('utf-8')
        offset += name_size
        if name in tensors:
            raise ValueError('duplicate native tensor name')
        element_type, rank = read('<I'), read('<I')
        if element_type != 1 or rank > 16:
            raise ValueError('unsupported native tensor type or rank')
        shape = [read('<q') for _ in range(rank)]
        if any(dimension < 0 for dimension in shape):
            raise ValueError('negative native tensor dimension')
        size = read('<Q')
        if offset + size > len(payload):
            raise ValueError('truncated native tensor payload')
        array = np.frombuffer(payload[offset:offset + size], dtype='<f4').astype(np.float32, copy=True)
        offset += size
        if shape:
            array = array.reshape(tuple(shape))
        tensors[name] = array
    if offset != len(payload):
        raise ValueError('NDNSF-DI native tensor bundle has trailing bytes')
    return tensors
