#!/usr/bin/env python3
"""Inspect pinned Wan PyTorch metadata over bounded HTTP ranges."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
from urllib.request import Request, urlopen

import torch
from torch._subclasses.fake_tensor import FakeTensorMode

RECIPE = (Path(__file__).resolve().parents[3] /
          'Experiments/recipes/wan2.1-t2v-14b-metadata.json')


class MetadataRange(io.RawIOBase):
    def __init__(self, url):
        super().__init__()
        self.url = url
        self.total = 0
        self.size = None
        self.position = 0
        self.read(4)
        self.position = 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.position

    def seek(self, offset, whence=io.SEEK_SET):
        if whence == io.SEEK_CUR:
            offset += self.position
        elif whence == io.SEEK_END:
            offset += self.size
        if offset < 0:
            raise ValueError('invalid seek')
        self.position = offset
        return offset

    def read(self, size=-1):
        if self.size is not None:
            size = min(size, self.size - self.position)
        if size == 0:
            return b''
        if size < 0 or size > 256*1024 or self.total + size > 512*1024:
            raise ValueError('requested bytes exceed metadata budget: ' + str(size))
        start = self.position
        end = start + size - 1
        query = Request(self.url, headers={'Range': f'bytes={start}-{end}'})
        with urlopen(query, timeout=30) as response:
            raw_range = response.headers.get('Content-Range') or ''
            match = re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)', raw_range)
            if response.status != 206 or not match or [int(n) for n in match.groups()[:2]] != [start, end]:
                raise ValueError('server did not return exact metadata range: ' + raw_range)
            actual = int(match.group(3))
            if self.size not in (None, actual):
                raise ValueError('upstream size changed')
            self.size = actual
            result = response.read(size + 1)
        if len(result) != size:
            raise ValueError('short metadata response')
        self.position += size
        self.total += size
        return result

    def readinto(self, buffer):
        data = self.read(len(buffer))
        buffer[:len(data)] = data
        return len(data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    recipe = json.loads(RECIPE.read_text())
    base = f"https://huggingface.co/{recipe['repository']}/resolve/{recipe['revision']}/"
    for model, expected in recipe['pytorch_mapped_metadata'].items():
        with MetadataRange(base + model + '?download=true') as source, FakeTensorMode():
            if source.size != expected['full_bytes']:
                raise ValueError('upstream archive size differs from pinned recipe')
            state = torch.load(source, map_location='meta', weights_only=True)
            if not all(value.device.type == 'meta' for value in state.values()):
                raise ValueError('checkpoint unexpectedly returned resident weights')
            rows = {key: {'dtype': str(value.dtype), 'shape': list(value.shape)}
                    for key, value in state.items()}
            encoded = json.dumps(rows, sort_keys=True, separators=(',', ':')) + '\n'
            if hashlib.sha256(encoded.encode()).hexdigest() != expected['metadata_sha256']:
                raise ValueError('checkpoint metadata digest differs from pinned recipe')
            with (args.output / (model + '.metadata.json')).open('x') as output:
                output.write(encoded)
            print(model, source.total, 'metadata bytes read', len(rows), 'tensors')


if __name__ == '__main__':
    main()
