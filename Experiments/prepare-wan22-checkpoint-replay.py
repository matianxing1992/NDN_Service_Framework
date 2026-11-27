#!/usr/bin/env python3
"""Prepare raw inputs for the native CUDA checkpoint oracle from a qualified clip.

This host-side tool performs no model inference. It independently checks the
recorded TensorBundle commitment before exporting raw tensors; execution and
the numerical assertion remain in di-wan-checkpoint-oracle.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys


def digest(data):
    return 'sha256:' + hashlib.sha256(data).hexdigest()


def bounded_file(path, limit):
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= limit:
        raise ValueError('invalid replay input file: ' + str(path))
    return path.read_bytes()


def tensor_header(name, dtype, shape, size):
    encoded = name.encode('ascii')
    return (struct.pack('<I', len(encoded)) + encoded +
            struct.pack('<II' + 'q' * len(shape) + 'Q', dtype, len(shape), *shape, size))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-clip', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--phase', action='append', required=True)
    parser.add_argument('--model-file', type=Path, required=True,
                        help='absolute graph path admitted on the later GPU replay node')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not args.model_file.is_absolute() or sys.byteorder != 'little':
        raise ValueError('replay requires an absolute GPU graph path and little-endian host')
    clip = args.native_clip.resolve(strict=True)
    manifest_wire = bounded_file(clip / 'manifest.json', 1 << 20)
    manifest = json.loads(manifest_wire)
    reference_wire = bounded_file(args.reference, 1 << 20)
    reference = json.loads(reference_wire)
    if (manifest['schema'] != 'ndnsf-di-wan-rgb24-v1' or
            reference['native_clip_manifest_digest'] != digest(manifest_wire)):
        raise ValueError('reference is not bound to the original native clip')
    generation = reference['generation']
    steps = generation['steps']
    if (not 0 < steps <= 1000 or len(args.phase) > 2 * steps or
            len(set(args.phase)) != len(args.phase)):
        raise ValueError('invalid or duplicate replay phase cover')
    trace = manifest['fixture_trace']
    if len(trace) != 3 + 3 * steps or len(reference['tensors']) != len(trace):
        raise ValueError('incomplete native or independent trajectory')
    trace_rows = {row['phase']: (i, row) for i, row in enumerate(trace)}
    expected_rows = {row['phase']: row for row in reference['tensors']}
    children = {row['phase']: row for row in manifest['children']}
    if len(trace_rows) != len(trace) or len(expected_rows) != len(trace):
        raise ValueError('duplicate trajectory phases')
    if len(children) != 2 * steps + 3:
        raise ValueError('incomplete native child bindings')

    def recorded(phase):
        index, row = trace_rows[phase]
        expected = expected_rows[phase]
        shape, name = expected['shape'], expected['name']
        if (not shape or len(shape) > 8 or
                any(not isinstance(value, int) or value <= 0 for value in shape)):
            raise ValueError('invalid native checkpoint shape')
        if expected['dtype'] != 'float32' or row['file'] != f'trace-{index}.bundle':
            raise ValueError('unsupported native checkpoint layout')
        count = math.prod(shape)
        if count * 4 > 64 << 20:
            raise ValueError('DiT replay checkpoint exceeds 64 MiB')
        header = b'NDITB001' + struct.pack('<I', 1) + tensor_header(name, 1, shape, count * 4)
        wire = bounded_file(clip / row['file'], (64 << 20) + (1 << 20))
        if (digest(wire) != row['digest'] or not wire.startswith(header) or
                len(wire) != len(header) + count * 4):
            raise ValueError('native checkpoint identity differs: ' + phase)
        if phase in children and any(row[key] != children[phase][key]
                                     for key in ('request_id', 'plan_digest')):
            raise ValueError('native checkpoint child binding differs: ' + phase)
        return wire[len(header):], shape

    catalog_wire = bounded_file(args.catalog, 16 << 20)
    source = json.loads(catalog_wire)['source']
    graph = bounded_file(Path(source['file']), 16 << 20)
    if len(graph) != source['bytes'] or digest(graph) != source['digest']:
        raise ValueError('source graph differs from the original catalog')
    output = args.output.absolute()
    if output.exists():
        raise ValueError('replay output must be a new directory')
    files = {'model.onnx': graph}
    receipts = []
    latent_shape = [1] + generation['latent_shape']
    _, frames, height, width = generation['latent_shape']
    valid_tokens = frames * (height // 2) * (width // 2)
    padded_tokens = generation['padded_video_tokens']
    if not 0 < valid_tokens <= padded_tokens:
        raise ValueError('invalid recorded video token cover')
    files['video-valid.raw'] = bytes([1]) * valid_tokens + bytes(padded_tokens - valid_tokens)
    for phase in args.phase:
        match = re.fullmatch(r'denoise/(\d+)/(positive|negative)', phase)
        if not match or phase not in children:
            raise ValueError('unknown DiT replay phase: ' + phase)
        step, branch = int(match[1]), match[2]
        if not 0 < step < steps:
            raise ValueError('replay requires a prior recorded latent, after step zero')
        actual, prediction_shape = recorded(phase)
        latent, shape = recorded(f'latent/{step - 1}')
        context, context_shape = recorded('text/' + branch)
        if shape != latent_shape or prediction_shape != latent_shape:
            raise ValueError('DiT replay latent shape differs')
        timestep = trace_rows[f'latent/{step}'][1]['timestep']
        name = phase.replace('/', '-')
        latent_file, context_file = f'latent-step-{step}.raw', f'text-{branch}.raw'
        files[latent_file], files[context_file] = latent, context
        files[name + '-timestep.raw'] = struct.pack('<q', timestep)
        inputs = [("latent", 1, latent_shape, latent_file, 'float32'),
                  ("context", 1, context_shape, context_file, 'float32'),
                  ("timestep", 3, [1], name + '-timestep.raw', 'int64'),
                  ("video-valid", 4, [1, 1, 1, padded_tokens], 'video-valid.raw', 'bool')]
        # This is an independent receipt check, not a production wire producer.
        commitment = b'NDITB001' + struct.pack('<I', len(inputs))
        for port, dtype, shape, filename, _ in inputs:
            data = files[filename]
            commitment += tensor_header(port, dtype, shape, len(data)) + data
        if digest(commitment) != children[phase]['input_digest']:
            raise ValueError('reconstructed inputs differ from native child commitment: ' + phase)
        row = expected_rows[phase]
        if not re.fullmatch(r'\d+\.f32', row['file']):
            raise ValueError('unsupported independent reference filename')
        expected = bounded_file(args.reference.parent / row['file'], 64 << 20)
        if digest(expected) != row['digest'] or len(expected) != len(actual):
            raise ValueError('independent stage identity differs: ' + phase)
        native_values, reference_values = array.array('f'), array.array('f')
        native_values.frombytes(actual)
        reference_values.frombytes(expected)
        failures = 0
        for value, wanted in zip(native_values, reference_values):
            if not math.isfinite(value) or not math.isfinite(wanted):
                raise ValueError('nonfinite original stage: ' + phase)
            failures += abs(value - wanted) > (
                reference['absolute_tolerance'] + reference['relative_tolerance'] * abs(wanted))
        files[name + '-reference.raw'] = expected
        files[name + '-distributed.raw'] = actual
        config = dict(schema='ndnsf-di-wan-checkpoint-oracle-v1',
                      model_file=str(args.model_file), model_graph_digest=source['digest'],
                      absolute_tolerance=reference['absolute_tolerance'],
                      relative_tolerance=reference['relative_tolerance'],
                      inputs=[dict(name=port, dtype=dtype, shape=shape, file=filename,
                                   digest=digest(files[filename]))
                              for port, _, shape, filename, dtype in inputs],
                      reference=dict(name='prediction', shape=prediction_shape,
                                     file=name + '-reference.raw', digest=digest(expected)))
        files[name + '.json'] = json.dumps(config, sort_keys=True).encode()
        if sum(map(len, files.values())) > 512 << 20:
            raise ValueError('aggregate replay inputs exceed 512 MiB')
        receipts.append(dict(phase=phase, request_id=children[phase]['request_id'],
                             input_digest=digest(commitment), timestep=timestep,
                             cells=len(expected) // 4, original_distributed_excess=failures,
                             configuration=name + '.json'))
    output.mkdir(mode=0o700)
    for name, data in files.items():
        with (output / name).open('xb') as stream:
            stream.write(data)
    receipt = dict(scope='offline preparation only; no model inference or qualification',
                   native_clip_manifest_digest=digest(manifest_wire),
                   independent_reference_digest=digest(reference_wire),
                   source_catalog_digest=digest(catalog_wire),
                   initializer_digest=source['initializer_digest'],
                   initializer_bytes=source['initializer_bytes'],
                   source_graph_digest=source['digest'], phases=receipts,
                   files={name: dict(bytes=len(data), digest=digest(data))
                          for name, data in files.items()})
    (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(dict(output=str(output), phases=len(receipts),
                         original_distributed_excess=sum(r['original_distributed_excess']
                                                        for r in receipts))))


if __name__ == '__main__':
    main()
