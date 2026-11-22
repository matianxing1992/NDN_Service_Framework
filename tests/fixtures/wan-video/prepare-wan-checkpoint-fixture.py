#!/usr/bin/env python3
"""Build a tiny mixed-precision PyTorch archive and check offline Wan conversion."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

import torch


ROOT = Path(__file__).resolve().parents[3]


def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(mode=0o700)
    weights = {
        'patch_embedding.weight': torch.tensor(
            [1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 3, 0, 0, 0, 0, -1], dtype=torch.bfloat16
        ).reshape(2, 2, 1, 2, 2),
        'patch_embedding.bias': torch.tensor([5, -2], dtype=torch.float16),
        'encoder.fixture': torch.full((270000,), 0.5, dtype=torch.bfloat16),
    }
    source = args.output / 'fixture.pth'
    target = args.output / 'fixture.safetensors'
    torch.save(weights, source)
    command = [sys.executable, str(ROOT / 'Experiments/prepare-wan-checkpoint.py'),
               '--input', str(source), '--source-sha256', digest(source), '--output', str(target)]
    subprocess.run(command, check=True)
    with target.open('rb') as stream:
        size, = struct.unpack('<Q', stream.read(8))
        header = json.loads(stream.read(size))
        begin, end = header['encoder.fixture']['data_offsets']
        assert end - begin == 270000 * 4
        for offset in (begin, begin + (1 << 20), end - 4):
            stream.seek(8 + size + offset)
            assert stream.read(4) == struct.pack('<f', 0.5)
    frozen = digest(target)
    rejected = subprocess.run(command, capture_output=True, text=True)
    assert rejected.returncode and 'FileExistsError' in rejected.stderr and digest(target) == frozen

    invalid = list(command)
    invalid[-3] = '0' * 64
    invalid[-1] = str(args.output / 'wrong-digest.safetensors')
    rejected = subprocess.run(invalid, capture_output=True, text=True)
    assert rejected.returncode and 'SHA-256 mismatch' in rejected.stderr
    assert not Path(invalid[-1]).exists()

    invalid_type = args.output / 'invalid-type.pth'
    torch.save({'fixture.int': torch.tensor([1], dtype=torch.int32)}, invalid_type)
    invalid[3] = str(invalid_type)
    invalid[-3] = digest(invalid_type)
    invalid[-1] = str(args.output / 'wrong-type.safetensors')
    rejected = subprocess.run(invalid, capture_output=True, text=True)
    assert rejected.returncode and 'unsupported Wan checkpoint tensor' in rejected.stderr
    assert not Path(invalid[-1]).exists()
    assert not list(args.output.glob('.wan-checkpoint-*'))
    print(target)


if __name__ == '__main__':
    main()
