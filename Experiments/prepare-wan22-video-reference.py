#!/usr/bin/env python3
"""Prepare an opt-in CPU trajectory oracle from official Wan2.2 networks and UniPC."""

import argparse
import gc
import hashlib
import importlib.metadata
import importlib.util
import json
from pathlib import Path
import struct
import time
import types

import numpy as np
import torch
from tokenizers import Tokenizer


HERE = Path(__file__).resolve().parent
DRIVER_SHA256 = '228f2fabf23014ed41ec6b8cd713d5be7371bb558501c71a62a0258b491a877b'
DRIVER_BYTES = 24031


def module_at(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    wire = path.read_bytes()
    exec(compile(wire, str(path), 'exec'), module.__dict__)
    return module, wire


def read_json(path):
    if path.stat().st_size > 1 << 20:
        raise ValueError('reference input configuration exceeds 1 MiB')
    return json.loads(path.read_bytes())


def sha256(wire):
    return 'sha256:' + hashlib.sha256(wire).hexdigest()


class CapturedClip:
    """Read the recorded inputs used for an independent, stage-by-stage replay."""

    def __init__(self, directory, initial_digest, steps):
        self.directory = directory.resolve(strict=True)
        path = self.directory / 'manifest.json'
        if path.stat().st_size > 1 << 20:
            raise ValueError('native clip manifest exceeds 1 MiB')
        wire = path.read_bytes()
        manifest = json.loads(wire)
        if (manifest['schema'] != 'ndnsf-di-wan-rgb24-v1' or
                manifest['initial_latent_digest'] != initial_digest or
                len(manifest['fixture_trace']) != 3 + 3 * steps):
            raise ValueError('native clip does not match the reference workload')
        self.digest = sha256(wire)
        self.trace = manifest['fixture_trace']

    def tensor(self, index, phase, name, shape, binding):
        row = self.trace[index]
        filename = f'trace-{index}.bundle'
        if (row['phase'] != phase or row['file'] != filename or
                any(row.get(key) != value for key, value in binding.items())):
            raise ValueError('native clip stage binding differs: ' + phase)
        # A checkpoint observation is one named FP32 tensor in the native bundle.
        # Construct its expected header from the independent shape and port identity.
        encoded_name = name.encode('ascii')
        byte_count = int(np.prod(shape)) * 4
        header = struct.pack('<8sII', b'NDITB001', 1, len(encoded_name)) + encoded_name
        header += struct.pack('<II' + 'q' * len(shape) + 'Q', 1, len(shape), *shape, byte_count)
        path = self.directory / filename
        if path.stat().st_size != len(header) + byte_count:
            raise ValueError('native clip tensor size differs: ' + phase)
        wire = path.read_bytes()
        if not wire.startswith(header) or sha256(wire) != row['digest']:
            raise ValueError('native clip tensor identity differs: ' + phase)
        value = torch.from_numpy(np.frombuffer(wire, dtype='<f4', offset=len(header)).copy())
        if not torch.isfinite(value).all():
            raise ValueError('native clip tensor contains nonfinite values: ' + phase)
        return value.reshape(shape)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--asset-root', type=Path, required=True)
    parser.add_argument('--workload', type=Path, required=True)
    parser.add_argument('--initial-latent', type=Path, required=True)
    parser.add_argument('--initial-receipt', type=Path, required=True)
    parser.add_argument('--native-clip', type=Path,
                        help='condition each official stage on the actual captured inputs')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    driver_wire = Path(__file__).read_bytes()
    workload = read_json(args.workload)
    workload_digest = sha256(args.workload.read_bytes())
    if workload['schema'] != 'ndnsf-di-wan-checkpoint-workload-v1':
        raise ValueError('unsupported checkpoint workload')
    generation = workload['generation']
    shape = generation['latent_shape']
    if (not isinstance(shape, list) or len(shape) != 4 or
            any(type(value) is not int for value in shape) or shape[0] != 48 or
            not 1 <= shape[1] <= 91 or any(not 2 <= value <= 20 or value % 2 for value in shape[2:]) or
            type(generation['steps']) is not int or not 1 <= generation['steps'] <= 50 or
            generation['shift'] != 5 or generation['guidance'] != 5 or
            generation['vae_spatial_stride'] != 16 or generation['text_width'] != 4096 or
            generation['frames_per_second'] != 24 or generation['tokenizer']['slots'] != 512 or
            type(generation['seed']) is not int or not 0 <= generation['seed'] < 1 << 64):
        raise ValueError('workload differs from the bounded Wan2.2 CPU reference profile')
    tokens = shape[1] * (shape[2] // 2) * (shape[3] // 2)
    padded = (tokens + 127) // 128 * 128
    if generation['padded_video_tokens'] != padded:
        raise ValueError('workload padding differs from the declared source graph')
    expected_shape = [1] + shape
    expected_bytes = int(np.prod(expected_shape)) * 4
    receipt = read_json(args.initial_receipt)
    if (receipt['shape'] != expected_shape or receipt['bytes'] != expected_bytes or
            receipt['seed'] != generation['seed'] or receipt['dtype'] != 'float32' or
            receipt['byte_order'] != 'little' or args.initial_latent.stat().st_size != expected_bytes):
        raise ValueError('initial latent receipt differs from the workload')
    initial = args.initial_latent.read_bytes()
    if sha256(initial) != receipt['digest']:
        raise ValueError('initial latent digest differs')
    sample = torch.from_numpy(np.frombuffer(initial, dtype='<f4').copy()).reshape(expected_shape)
    if not torch.isfinite(sample).all():
        raise ValueError('initial latent contains nonfinite values')
    capture = (CapturedClip(args.native_clip, receipt['digest'], generation['steps'])
               if args.native_clip is not None else None)
    network, network_wire = module_at('wan22_reference_network',
                                      HERE / 'prepare-wan22-network-reference.py')
    vae, vae_wire = module_at('wan22_reference_vae', HERE / 'prepare-wan22-vae-reference.py')
    sampler, sampler_wire = module_at('wan22_reference_sampler_pin',
                                     HERE.parent / 'tests/fixtures/wan-video/prepare-sampler-reference.py')
    recipe_wire = network.RECIPE.read_bytes()
    recipe = json.loads(recipe_wire)
    root = args.asset_root.resolve(strict=True)
    packages = network.prepare_reference_environment(root, recipe)
    for name, version in {'tokenizers': '0.21.1', 'ftfy': '6.2.3', 'regex': '2024.11.6'}.items():
        packages[name] = importlib.metadata.version(name)
        if packages[name] != version:
            raise ValueError('reference tokenizer package differs: ' + name)
    network.verify(root / 'reference-source/textimage2video.py',
                   dict(bytes=DRIVER_BYTES, sha256=DRIVER_SHA256))
    solver_path = root / 'reference-source/fm_solvers_unipc.py'
    network.verify(solver_path, dict(bytes=sampler.SOURCE_BYTES, sha256=sampler.SOURCE_SHA256))
    solver = types.ModuleType('wan22_official_solver')
    exec(compile(solver_path.read_bytes(), str(solver_path), 'exec'), solver.__dict__)
    tokenizer_row = next(row for row in recipe['sources']
                         if row['file'] == 'google/umt5-xxl/tokenizer.json')
    tokenizer_path = root / tokenizer_row['file']
    network.verify(tokenizer_path, tokenizer_row)
    if generation['tokenizer']['digest'] != 'sha256:' + tokenizer_row['sha256']:
        raise ValueError('workload tokenizer differs from the pinned source')
    workload_tokenizer = Path(generation['tokenizer']['file']).resolve(strict=True)
    if workload_tokenizer != tokenizer_path:
        network.verify(workload_tokenizer, tokenizer_row)
    clean = network.source_module(root, 'tokenizers')
    tokenizer = Tokenizer.from_file(str(tokenizer_path))
    tokenizer.enable_truncation(max_length=512)
    tokenizer.enable_padding(length=512, pad_id=0, pad_token='<pad>')
    prompts = [generation['positive_prompt'], generation['negative_prompt']]
    if any(not isinstance(text, str) or len(text.encode('utf-8')) > 1 << 20 for text in prompts):
        raise ValueError('prompt is outside its byte bound')
    encoded = tokenizer.encode_batch([clean.whitespace_clean(clean.basic_clean(text))
                                      for text in prompts])
    inputs = []
    for entry in encoded:
        valid = sum(entry.attention_mask)
        if (len(entry.ids) != 512 or not 1 <= valid <= 512 or entry.ids[valid - 1] != 1 or
                entry.attention_mask != [1] * valid + [0] * (512 - valid)):
            raise ValueError('source tokenizer did not return padded EOS-terminated tokens')
        inputs.append(dict(ids=entry.ids, valid_tokens=valid))
    args.output.mkdir(mode=0o700)
    for name, wire in [('producer-driver.py', driver_wire), ('network-driver.py', network_wire),
                       ('vae-driver.py', vae_wire), ('sampler-driver.py', sampler_wire),
                       ('recipe.json', recipe_wire)]:
        with (args.output / name).open('xb') as output:
            output.write(wire)
    rows = []
    remaining = 2 << 30
    started, cpu_started = time.monotonic(), time.process_time()

    def record(phase, name, tensor, expected, **binding):
        nonlocal remaining
        if (tensor.dtype != torch.float32 or list(tensor.shape) != expected or
                not torch.isfinite(tensor).all()):
            raise ValueError('source returned incomplete or nonfinite tensor: ' + phase)
        size = tensor.numel() * tensor.element_size()
        if size > min(512 << 20, remaining):
            raise ValueError('trajectory reference exceeds its byte budget')
        remaining -= size
        wire = tensor.detach().contiguous().numpy().astype('<f4', copy=False).tobytes()
        filename = str(len(rows)) + '.f32'
        with (args.output / filename).open('xb') as output:
            output.write(wire)
        rows.append(dict(phase=phase, name=name, shape=expected, dtype='float32',
                         file=filename, digest=sha256(wire), **binding))
        print(phase, 'bytes=' + str(size), 'wall_s=' + str(time.monotonic() - started), flush=True)
        return (tensor if capture is None else
                capture.tensor(len(rows) - 1, phase, name, expected, binding))

    branches = ('positive', 'negative')
    contexts = []
    model = network.load_umt5_model(root, recipe)
    with torch.inference_mode():
        for branch, entry in zip(branches, inputs):
            ids = torch.tensor([entry['ids']], dtype=torch.int64)
            mask = torch.arange(512)[None, :] < entry['valid_tokens']
            context = model(ids, mask)[:, :entry['valid_tokens']].contiguous()
            contexts.append(record('text/' + branch, 'text-context', context,
                                   [1, entry['valid_tokens'], 4096]))
    del model
    gc.collect()
    model = network.load_dit_model(root, recipe)
    scheduler = solver.FlowUniPCMultistepScheduler(
        num_train_timesteps=1000, shift=1, use_dynamic_shifting=False)
    scheduler.set_timesteps(generation['steps'], device='cpu', shift=generation['shift'])
    with torch.inference_mode():
        for index, timestep in enumerate(scheduler.timesteps):
            predictions = []
            for branch, context in zip(branches, contexts):
                prediction = model([sample[0]], timestep.reshape(1), [context[0]], padded)[0][None]
                predictions.append(record('denoise/' + str(index) + '/' + branch,
                                          'prediction', prediction, expected_shape))
            flow = predictions[1] + generation['guidance'] * (predictions[0] - predictions[1])
            corrector_order = getattr(scheduler, 'this_order', 0)
            updated = scheduler.step(flow, timestep, sample, return_dict=False)[0]
            sample = record('latent/' + str(index), 'latent', updated, expected_shape,
                            timestep=int(timestep), predictor_order=scheduler.this_order,
                            corrector_order=corrector_order)
    del model, scheduler, predictions, flow
    gc.collect()
    model, scale = vae.load_vae_model(root, recipe)
    rgb_shape = [1, 3, 4 * (shape[1] - 1) + 1, shape[2] * 16, shape[3] * 16]
    with torch.inference_mode():
        decoded = model.decode(sample, scale).float()
        if list(decoded.shape) != rgb_shape or not torch.isfinite(decoded).all():
            raise ValueError('official VAE returned invalid values or shape before clamping')
        rgb = decoded.clamp(-1, 1)
        del decoded
    record('decode', 'rgb', rgb, rgb_shape)
    result = dict(schema='ndnsf-di-wan22-trajectory-reference-v1', generation=generation,
                  positive_tokens=inputs[0], negative_tokens=inputs[1], initial=receipt, tensors=rows,
                  absolute_tolerance=2e-4, relative_tolerance=2e-4,
                  provenance=dict(recipe=recipe, recipe_digest=sha256(recipe_wire),
                      workload_digest=workload_digest, packages=packages,
                      driver_digest=sha256(driver_wire), network_driver_digest=sha256(network_wire),
                      vae_driver_digest=sha256(vae_wire), sampler_driver_digest=sha256(sampler_wire),
                      sampler=dict(url=sampler.SOURCE_URL, sha256=sampler.SOURCE_SHA256),
                      generation_driver=dict(revision=sampler.REVISION, sha256=DRIVER_SHA256),
                      cpu_adaptation='FP32 source networks, SDPA MATH, frozen C++ initial input, pad128',
                      wall_seconds=time.monotonic() - started,
                      cpu_seconds=time.process_time() - cpu_started))
    if capture is not None:
        result['schema'] = 'ndnsf-di-wan22-conditioned-trajectory-reference-v1'
        result['native_clip_manifest_digest'] = capture.digest
    with (args.output / 'reference.json').open('x', encoding='utf-8') as output:
        json.dump(result, output, sort_keys=True, indent=2)
        output.write('\n')
    print(args.output / 'reference.json', flush=True)


if __name__ == '__main__':
    main()
