#!/usr/bin/env python3
"""Prepare an opt-in host PyTorch reference for the native real-weight VAE probe."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

import numpy as np
import torch


RECIPE = Path(__file__).resolve().parent / 'recipes/wan2.2-ti2v-5b-metadata.json'


def verify(path, row):
    if path.stat().st_size != row['bytes']:
        raise ValueError('source size mismatch: ' + str(path))
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            checksum.update(chunk)
    if checksum.hexdigest() != row['sha256']:
        raise ValueError('source digest mismatch: ' + str(path))


def load_vae_model(root, recipe, device='cpu'):
    source = root / 'vae2_2-reference.py'
    verify(source, recipe['vae_reference'])
    checkpoint = root / 'Wan2.2_VAE.pth'
    verify(checkpoint, next(row for row in recipe['sources'] if row['file'] == checkpoint.name))
    if torch.__version__.split('+')[0] != '2.6.0' or np.__version__ != '1.26.4':
        raise ValueError('use the pinned wan2.1-checkpoint-tools-py311 environment')
    spec = importlib.util.spec_from_file_location('wan22_offline_reference', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    model = module._video_vae(pretrained_path=str(checkpoint), z_dim=48, dim=160, dec_dim=256,
                              dim_mult=[1, 2, 4, 4], temperal_downsample=[False, True, True],
                              device=device)
    model.eval().requires_grad_(False)
    normalization = recipe['vae_normalization']
    scale = [torch.tensor(normalization['latent_mean'], dtype=torch.float32, device=device),
             1 / torch.tensor(normalization['latent_std'], dtype=torch.float32, device=device)]
    return model, scale


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--asset-root', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    recipe = json.loads(RECIPE.read_bytes())
    root = args.asset_root.resolve(strict=True)
    torch.set_num_threads(2)
    torch.use_deterministic_algorithms(True)
    model, scale = load_vae_model(root, recipe)
    latent = torch.linspace(-0.25, 0.25, 48 * 3 * 2 * 2, dtype=torch.float32).reshape(1, 48, 3, 2, 2)
    with torch.inference_mode():
        result = model.decode(latent, scale).float().clamp(-1, 1)
    if list(result.shape) != [1, 3, 9, 32, 32] or not torch.isfinite(result).all():
        raise ValueError('official reference returned incomplete output')
    args.output.mkdir()

    def write(name, tensor):
        data = tensor.contiguous().numpy().astype('<f4', copy=False).tobytes()
        (args.output / name).write_bytes(data)
        return dict(file=name, shape=list(tensor.shape), digest='sha256:' + hashlib.sha256(data).hexdigest())

    input_row = dict(name='latent-clip', **write('latent.f32', latent))
    reference = dict(name='rgb', **write('rgb.f32', result))
    config = dict(schema='ndnsf-di-wan-checkpoint-oracle-v1', model_file=str(args.model.resolve(strict=True)),
                  inputs=[input_row], reference=reference, absolute_tolerance=2e-4, relative_tolerance=2e-4,
                  provenance=dict(recipe_sha256=hashlib.sha256(RECIPE.read_bytes()).hexdigest(),
                                  reference=recipe['vae_reference'], pytorch=torch.__version__,
                                  numpy=np.__version__, scope='real VAE weights, nine frames, FP32 source reference'))
    (args.output / 'oracle.json').write_text(json.dumps(config, indent=2) + '\n')
    print(args.output / 'oracle.json')


if __name__ == '__main__':
    main()
