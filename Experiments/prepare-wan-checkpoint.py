#!/usr/bin/env python3
"""Convert a digest-bound Wan PyTorch state dict into a streamed FP32 SafeTensors archive."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import tempfile

import torch


MAX_HEADER_BYTES = 4 << 20
READ_WINDOW_BYTES = 1 << 20


def fingerprint(source):
    metadata = source.stat()
    if not stat.S_ISREG(metadata.st_mode):
        raise ValueError('checkpoint must be a regular file')
    return metadata.st_dev, metadata.st_ino, metadata.st_size, metadata.st_mtime_ns, metadata.st_ctime_ns


def verify_digest(source, expected):
    digest = hashlib.sha256()
    with source.open('rb') as stream:
        for chunk in iter(lambda: stream.read(READ_WINDOW_BYTES), b''):
            digest.update(chunk)
    if digest.hexdigest() != expected:
        raise ValueError('PyTorch checkpoint SHA-256 mismatch')


def make_header(weights):
    if not isinstance(weights, dict) or not weights:
        raise ValueError('Wan checkpoint must be a non-empty tensor state dict')
    if any(not isinstance(name, str) or not name or '\0' in name or name == '__metadata__'
           for name in weights):
        raise ValueError('Wan checkpoint contains an invalid tensor name')
    header = {}
    offset = 0
    for name, tensor in sorted(weights.items()):
        if (not isinstance(tensor, torch.Tensor) or tensor.dtype not in
                (torch.bfloat16, torch.float16, torch.float32) or
                tensor.layout != torch.strided or not tensor.is_contiguous() or
                tensor.ndim > 5 or tensor.numel() == 0):
            raise ValueError('unsupported Wan checkpoint tensor: ' + name)
        end = offset + tensor.numel() * 4
        header[name] = {'dtype': 'F32', 'shape': list(tensor.shape),
                        'data_offsets': [offset, end]}
        offset = end
    wire = json.dumps(header, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    wire += b' ' * (-len(wire) % 8)
    if len(wire) > MAX_HEADER_BYTES:
        raise ValueError('Wan checkpoint metadata exceeds the native header limit')
    return wire


def convert(source, expected, destination):
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(destination)
    if sys.byteorder != 'little':
        raise ValueError('Wan FP32 checkpoint preparation requires a little-endian host')
    original = fingerprint(source)
    verify_digest(source, expected)
    if fingerprint(source) != original:
        raise ValueError('checkpoint changed during digest verification')
    state = torch.load(source, map_location='cpu', mmap=True, weights_only=True)
    header = make_header(state)
    stream = tempfile.NamedTemporaryFile(
        prefix='.wan-checkpoint-', dir=destination.parent, delete=False)
    temporary = Path(stream.name)
    try:
        with stream:
            stream.write(struct.pack('<Q', len(header)))
            stream.write(header)
            for _, tensor in sorted(state.items()):
                flat = tensor.detach().view(-1)
                for offset in range(0, flat.numel(), READ_WINDOW_BYTES // 4):
                    chunk = flat[offset:offset + READ_WINDOW_BYTES // 4].to(torch.float32)
                    stream.write(memoryview(chunk.numpy()).cast('B'))
        if fingerprint(source) != original:
            raise ValueError('checkpoint changed during conversion')
        os.link(temporary, destination)
    finally:
        temporary.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--source-sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch('[0-9a-f]{64}', args.source_sha256):
        parser.error('--source-sha256 must be a 64-character lowercase digest')
    if args.output.suffix != '.safetensors':
        parser.error('--output must be a .safetensors file')
    convert(args.input, args.source_sha256, args.output)


if __name__ == '__main__':
    main()
