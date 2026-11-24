#!/usr/bin/env python3
"""Regenerate the tiny native sampler oracle with the pinned official Wan2.2 solver."""

import argparse
import hashlib
import importlib.metadata
import importlib.util
import json
from pathlib import Path

import numpy as np
import torch


REVISION = '1ea34ff48f87168174e12956e200b1d908b1c5ff'
SOURCE_SHA256 = '0dec8c7ed17f6f2049275c6848113314da6ccec1c8db5bdc89df43c05c6038d9'
SOURCE_BYTES = 32558
SOURCE_URL = ('https://raw.githubusercontent.com/Wan-Video/Wan2.2/' + REVISION +
              '/wan/utils/fm_solvers_unipc.py')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    source = args.source.read_bytes()
    if len(source) != SOURCE_BYTES or hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        raise ValueError('official Wan2.2 sampler source identity differs')
    if (torch.__version__.split('+')[0] != '2.6.0' or np.__version__ != '1.26.4' or
            importlib.metadata.version('diffusers') != '0.33.0'):
        raise ValueError('use the pinned wan2.2-reference-tools-py311 environment')
    spec = importlib.util.spec_from_file_location('wan22_sampler_reference', args.source)
    module = importlib.util.module_from_spec(spec)
    # Execute the exact bytes verified above, including the official scheduler arithmetic.
    exec(compile(source, str(args.source), 'exec'), module.__dict__)
    torch.set_num_threads(1)
    torch.use_deterministic_algorithms(True)
    initial = [0.625, -1.25, 3.0]
    positive = [[1, -0.25, 1.5], [-0.75, 0.5, -3], [0.5, 1, -0.25], [-0.75, 0.25, 1.5]]
    negative = [[0.5, 0.25, 1], [0.25, -1, -1], [0.125, -1.5, 0.5], [0.5, -0.5, -1.5]]
    scheduler = module.FlowUniPCMultistepScheduler(
        num_train_timesteps=1000, shift=1, use_dynamic_shifting=False)
    scheduler.set_timesteps(50, device='cpu', shift=5)
    sample = torch.tensor([initial], dtype=torch.float32)
    rows = []
    with torch.inference_mode():
        for index, timestep in enumerate(scheduler.timesteps):
            conditioned = torch.tensor([positive[index % 4]], dtype=torch.float32)
            unconditioned = torch.tensor([negative[index % 4]], dtype=torch.float32)
            prediction = unconditioned + 5 * (conditioned - unconditioned)
            sample = scheduler.step(prediction, timestep, sample, return_dict=False)[0]
            if list(sample.shape) != [1, 3] or not torch.isfinite(sample).all():
                raise ValueError('official sampler returned incomplete or nonfinite values')
            rows.append(dict(timestep=int(timestep), positive=positive[index % 4],
                             negative=negative[index % 4], latent=sample[0].tolist()))
    result = dict(schema='ndnsf-di-wan22-sampler-reference-v1',
                  source=dict(url=SOURCE_URL, revision=REVISION, sha256=SOURCE_SHA256,
                              bytes=SOURCE_BYTES),
                  tools=dict(torch=torch.__version__, numpy=np.__version__, diffusers='0.33.0'),
                  initial=initial, guidance=5.0, shift=5.0, steps=rows)
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(result, output, sort_keys=True, indent=2)
        output.write('\n')


if __name__ == '__main__':
    main()
