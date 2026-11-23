#!/usr/bin/env python3
"""Write input for the maintained native Wan checkpoint preparer (host only)."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


RECIPE = Path(__file__).resolve().parent / 'recipes/wan2.2-ti2v-5b-metadata.json'
COMPONENTS = ('umt5', 'dit', 'vae')


def digest(value):
    if not isinstance(value, bytes):
        value = json.dumps(value, sort_keys=True, separators=(',', ':')).encode()
    return 'sha256:' + hashlib.sha256(value).hexdigest()


def float32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def make_request(args):
    recipe = json.loads(RECIPE.read_bytes())
    root = args.asset_root.resolve(strict=True)
    if args.frames <= 0 or args.frames % 4 != 1:
        raise ValueError('frames must be positive and 4n+1')
    if min(args.width, args.height) <= 0 or args.width % 32 or args.height % 32:
        raise ValueError('width and height must be positive multiples of 32')
    latent_frames, height, width = (args.frames - 1) // 4 + 1, args.height // 16, args.width // 16
    tokens = latent_frames * (height // 2) * (width // 2)
    padded = (tokens + 127) // 128 * 128
    component = args.component
    if args.layout == 'single':
        stages = [dict(id=component, roles=['/' + component + '/single'])]
    elif args.layout == 'tp':
        stages = [dict(id=component, roles=['/' + component + '/rank-0',
                                            '/' + component + '/rank-1'])]
    else:
        degrees = (1, 1)
        if component == 'dit':
            degrees = {'pp': (1, 1), 'pp-tp': (1, 2), 'tp-pp': (2, 1)}[args.layout]
        stages = [dict(id=component + '/' + name,
                       roles=['/' + component + '/' + name + '/rank-' + str(i)
                              for i in range(degree)])
                  for name, degree in zip(('front', 'back'), degrees)]
    sources = {row['file']: row for row in recipe['sources']}

    def identity(row):
        path = root / row['file']
        if not path.is_file() or path.stat().st_size != row['bytes']:
            raise ValueError('missing or incomplete asset: ' + str(path))
        # The native opener verifies this commitment before reading tensor data.
        return dict(file=str(path), bytes=row['bytes'], digest='sha256:' + row['sha256'])

    index = None
    if component == 'dit':
        shards = {name: identity(sources[name]) for name in recipe['shards']}
        index = identity(sources['diffusion_pytorch_model.safetensors.index.json'])
        geometry = dict(grid=dict(frames=latent_frames, height=height // 2, width=width // 2,
                                  padded_tokens=padded), heads=24, text_tokens=512,
                        video_key_tile=128, text_key_tile=128, prefix_blocks=15,
                        score_bytes=512 << 20, rotary_bytes=256 << 20)
    elif component == 'vae':
        row = recipe['converted_vae']
        shards = {row['file']: identity(row)}
        normalization = recipe['vae_normalization']
        geometry = dict(height=height, width=width, latent_frames=latent_frames,
                        key_tile=math.gcd(height * width, 128), score_bytes=64 << 20,
                        latent_mean=normalization['latent_mean'],
                        latent_inverse_std=[float32(1 / float32(v))
                                            for v in normalization['latent_std']])
    else:
        row = recipe['converted_umt5']
        shards = {row['file']: identity(row)}
        geometry = dict(text_tokens=512, score_bytes=64 << 20, prefix_layers=12)
    committed_shards = {name: {key: row[key] for key in ('bytes', 'digest')}
                        for name, row in shards.items()}
    committed_index = None if index is None else {key: index[key] for key in ('bytes', 'digest')}
    binding = dict(recipe=digest(RECIPE.read_bytes()), component=component, geometry=geometry,
                   shards=committed_shards, index=committed_index)
    content = digest(binding)
    checkout = Path(__file__).resolve().parents[1]
    closure = [checkout / 'NDNSF-DistributedInference/cpp', checkout / 'ndn-service-framework']
    source_identity = digest({str(path.relative_to(checkout)): digest(path.read_bytes())
                             for directory in closure for path in sorted(directory.rglob('*'))
                             if path.suffix in ('.hpp', '.cpp')})
    preparation = digest(dict(model=binding, stages=stages, max_exact_bundle_bytes=32 << 20,
                              source=source_identity, generator=digest(Path(__file__).read_bytes())))
    adapter = dict(name='wan', version='1', abi='wan-onnx-source-v1',
                   state_digest=source_identity, model_formats=['onnx'], tasks=['video-generation'],
                   backends=['onnxruntime'], precisions=['fp32'], graph_inspectable=True,
                   splittable=True, deterministic_analysis=True)
    for field in ('input', 'options', 'result', 'graph', 'split', 'state'):
        adapter[field + '_schema_digest'] = digest(['wan-native-v1', field, source_identity])
    model = dict(model_name='Wan2.2-TI2V-5B/' + component, content_digest=content,
                 semantics_digest=digest(binding), graph_digest=content, model_format='onnx',
                 precision='fp32', source_revision=recipe['revision'], adapter=adapter)
    resources = {role: dict(backends=['onnxruntime'], weight_bytes=32 << 30,
                           workspace_bytes=32 << 30, kv_bytes=0, activation_bytes=2 << 30,
                           transient_bytes=4 << 30, safety_margin=1.0)
                 for stage in stages for role in stage['roles']}
    namespace = args.requester_prefix.rstrip('/')
    if not namespace.startswith('/') or namespace == '':
        raise ValueError('requester prefix must be an absolute Producer name')
    catalog = dict(schema='ndnsf-di-native-request-catalog-v1', model=model,
                   source=dict(data_name=namespace + '/model-source/' + component + '/' + preparation[7:],
                               model_manifest_digest=digest(['manifest', preparation])),
                   recipe=dict(artifact_profile_digest=digest(['fp32', source_identity]),
                               assembler_descriptor_digest=source_identity,
                               backend_abi='onnxruntime-cpu-v1', precision='fp32', quantization='none',
                               layout='native', padding='right', protection_epoch=args.epoch,
                               max_source_bytes=64 << 30, max_assembled_bytes=64 << 30,
                               max_nodes=1048576),
                   publication=dict(artifact_root=namespace + '/prepared-models'),
                   input_format='OPAQUE', max_payload_bytes=512 << 20,
                   splitter=dict(kind='ONNX_SOURCE', service_name=args.service,
                                 resources_by_role=resources))
    request = dict(schema='ndnsf-di-wan-checkpoint-preparation-v1', component=component,
                   stages=stages, shards=shards, geometry=geometry, catalog=catalog,
                   max_exact_bundle_bytes=32 << 20, timeout_ms=86400000)
    if index is not None:
        request['index'] = index
    return request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--asset-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--component', choices=COMPONENTS, required=True)
    parser.add_argument('--layout', choices=('single', 'tp', 'pp', 'pp-tp', 'tp-pp'), required=True)
    parser.add_argument('--frames', type=int, default=361)
    parser.add_argument('--height', type=int, required=True)
    parser.add_argument('--width', type=int, required=True)
    parser.add_argument('--requester-prefix', required=True)
    parser.add_argument('--service', required=True)
    parser.add_argument('--epoch', default='epoch-1')
    args = parser.parse_args()
    request = make_request(args)
    with args.output.open('x') as output:
        json.dump(request, output, sort_keys=True, indent=2)
        output.write('\n')


if __name__ == '__main__':
    main()
