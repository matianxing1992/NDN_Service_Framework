#!/usr/bin/env python3
"""Prepare opt-in CPU source-network references for real Wan22 DiT or UMT5."""

import argparse
import ast
import hashlib
import importlib.metadata
import importlib.util
import json
from pathlib import Path
import sys
import types

import numpy as np
import torch
from safetensors.torch import load_file


RECIPE = Path(__file__).resolve().parent / 'recipes/wan2.2-ti2v-5b-metadata.json'


def verify(path, row):
    if path.stat().st_size != row['bytes']:
        raise ValueError('source size mismatch: ' + str(path))
    checksum = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1 << 20), b''):
            checksum.update(chunk)
    if checksum.hexdigest() != row['sha256']:
        raise ValueError('source digest mismatch: ' + str(path))


def source_module(root, name, omit_cuda_wrapper=False):
    path = root / 'reference-source' / (name + '.py')
    qualified = 'wan22_cpu_reference.' + name
    spec = importlib.util.spec_from_file_location(qualified, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[qualified] = module
    if omit_cuda_wrapper:
        tree = ast.parse(path.read_text(), filename=str(path))
        # The unused high-level wrapper evaluates cuda.current_device at import.
        # Keep the encoder classes and arithmetic unchanged, without executing it.
        omitted = [node for node in tree.body
                   if isinstance(node, ast.ClassDef) and node.name == 'T5EncoderModel']
        if len(omitted) != 1:
            raise ValueError('unexpected official T5 wrapper structure')
        tree.body = [node for node in tree.body if node is not omitted[0]]
        exec(compile(tree, str(path), 'exec'), module.__dict__)
    else:
        spec.loader.exec_module(module)
    return module


def cpu_attention(q, k, v, q_lens=None, k_lens=None, dropout_p=0., softmax_scale=None,
                  q_scale=None, causal=False, window_size=(-1, -1), **unused):
    if q.device.type != 'cpu' or q.dtype != torch.float32 or q_lens is not None or \
            dropout_p != 0 or window_size != (-1, -1):
        raise ValueError('unsupported CPU reference attention configuration')
    if q_scale is not None:
        q = q * q_scale
    mask = None
    if k_lens is not None:
        mask = (torch.arange(k.shape[1]).unsqueeze(0) < k_lens.unsqueeze(1))[:, None, None, :]
    with torch.nn.attention.sdpa_kernel(torch.nn.attention.SDPBackend.MATH):
        result = torch.nn.functional.scaled_dot_product_attention(
            q.transpose(1, 2), k.transpose(1, 2), v.transpose(1, 2),
            attn_mask=mask, dropout_p=0., is_causal=causal, scale=softmax_scale)
    return result.transpose(1, 2).contiguous()


def dit_reference(root, recipe):
    for row in recipe['sources']:
        if row['file'] in recipe['shards'] or row['file'] == 'config.json':
            verify(root / row['file'], row)
    module = source_module(root, 'model')
    module.flash_attention = cpu_attention
    original_rope = module.rope_params

    def cpu_rope(*args, **kwargs):
        with torch.device('cpu'):
            return original_rope(*args, **kwargs)

    module.rope_params = cpu_rope
    config = recipe['model_config']
    keys = ('model_type', 'dim', 'ffn_dim', 'freq_dim', 'in_dim', 'out_dim',
            'num_heads', 'num_layers', 'text_len', 'eps')
    with torch.device('meta'):
        model = module.WanModel(**{key: config[key] for key in keys})
    weights = {}
    for name in recipe['shards']:
        shard = load_file(str(root / name), device='cpu')
        if any(name in weights for name in shard):
            raise ValueError('duplicate reference checkpoint parameter')
        weights.update(shard)
    model.load_state_dict(weights, strict=True, assign=True)
    del weights
    model.eval().requires_grad_(False)
    latent = torch.linspace(-0.25, 0.25, 48 * 3 * 2 * 2, dtype=torch.float32).reshape(1, 48, 3, 2, 2)
    context = torch.linspace(-0.1, 0.1, 512 * 4096, dtype=torch.float32).reshape(1, 512, 4096)
    timestep = torch.tensor([500], dtype=torch.int64)
    valid = torch.zeros((1, 1, 1, 128), dtype=torch.bool)
    valid[:, :, :, :3] = True
    with torch.inference_mode():
        result = model([latent[0]], timestep, [context[0]], 128)[0].unsqueeze(0)
    return [('latent', latent), ('context', context), ('timestep', timestep),
            ('video-valid', valid)], 'prediction', result, [1, 48, 3, 2, 2]


def umt5_reference(root, recipe):
    original = next(row for row in recipe['sources']
                    if row['file'] == 'models_t5_umt5-xxl-enc-bf16.pth')
    verify(root / original['file'], original)
    module = source_module(root, 't5', omit_cuda_wrapper=True)
    model = module.umt5_xxl(encoder_only=True, return_tokenizer=False,
                            dtype=torch.float32, device='meta')
    state = torch.load(root / original['file'], map_location='cpu', mmap=True, weights_only=True)
    model.load_state_dict({name: value.float() for name, value in state.items()},
                           strict=True, assign=True)
    del state
    model.eval().requires_grad_(False)
    ids = torch.zeros((1, 512), dtype=torch.int64)
    ids[0, :3] = torch.tensor([23, 17, 1])
    mask = torch.zeros((1, 512), dtype=torch.bool)
    mask[0, :3] = True
    with torch.inference_mode():
        result = model(ids, mask)[:, :3].contiguous()
    return [('token-ids', ids), ('key-mask', mask)], 'text-context', result, [1, 3, 4096]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', choices=('dit', 'umt5'), required=True)
    parser.add_argument('--asset-root', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    root = args.asset_root.resolve(strict=True)
    recipe = json.loads(RECIPE.read_bytes())
    for row in recipe['network_reference']['sources']:
        verify(root / row['local_file'], row)
    if torch.__version__.split('+')[0] != '2.6.0' or np.__version__ != '1.26.4':
        raise ValueError('use the pinned wan2.2-reference-tools-py311 environment')
    expected_packages = {'diffusers': '0.33.0', 'transformers': '4.51.3',
                         'safetensors': '0.5.3', 'huggingface-hub': '0.30.2'}
    packages = {name: importlib.metadata.version(name) for name in expected_packages}
    if packages != expected_packages:
        raise ValueError('reference package versions differ from the pinned tool environment')
    torch.set_num_threads(2)
    torch.use_deterministic_algorithms(True)
    package = types.ModuleType('wan22_cpu_reference')
    package.__path__ = [str(root / 'reference-source')]
    sys.modules[package.__name__] = package
    inputs, name, result, shape = (dit_reference if args.component == 'dit'
                                   else umt5_reference)(root, recipe)
    if list(result.shape) != shape or result.dtype != torch.float32 or not torch.isfinite(result).all():
        raise ValueError('source reference returned incomplete or nonfinite output')
    args.output.mkdir()

    def write(name, tensor):
        dtype, numpy_type = {torch.float32: ('float32', '<f4'), torch.int64: ('int64', '<i8'),
                             torch.bool: ('bool', '|u1')}[tensor.dtype]
        wire = tensor.contiguous().numpy().astype(numpy_type, copy=False).tobytes()
        file = name + '.raw'
        (args.output / file).write_bytes(wire)
        return dict(name=name, file=file, dtype=dtype, shape=list(tensor.shape),
                    digest='sha256:' + hashlib.sha256(wire).hexdigest())

    config = dict(schema='ndnsf-di-wan-checkpoint-oracle-v1',
                  model_file=str(args.model.resolve(strict=True)),
                  inputs=[write(name, tensor) for name, tensor in inputs],
                  reference=write(name, result), absolute_tolerance=2e-4, relative_tolerance=2e-4,
                  provenance=dict(recipe_sha256=hashlib.sha256(RECIPE.read_bytes()).hexdigest(),
                                  source=recipe['network_reference'], pytorch=torch.__version__,
                                  packages=packages,
                                  cpu_attention='PyTorch SDPA MATH FP32, original key lengths preserved',
                                  t5_import='unused CUDA wrapper omitted; encoder arithmetic unchanged',
                                  driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                                  scope=args.component + ', real weights, bounded input, CPU reference'))
    (args.output / 'oracle.json').write_text(json.dumps(config, indent=2) + '\n')
    print(args.output / 'oracle.json')


if __name__ == '__main__':
    main()
