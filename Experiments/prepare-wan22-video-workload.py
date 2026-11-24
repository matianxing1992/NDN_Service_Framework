#!/usr/bin/env python3
"""Bind explicitly prepared Wan2.2 catalogs to the maintained MiniNDN launcher."""

import argparse
import json
from pathlib import Path


RECIPE = Path(__file__).resolve().parent / 'recipes/wan2.2-ti2v-5b-metadata.json'
COMPONENTS = ('umt5', 'dit', 'vae')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for component in COMPONENTS:
        parser.add_argument('--' + component + '-catalog', type=Path, required=True)
    parser.add_argument('--asset-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--namespace', default='/spec271/wan22')
    parser.add_argument('--mode', choices=('single', 'tp', 'pp', 'pp-tp', 'tp-pp'), required=True)
    parser.add_argument('--frames', type=int, default=361)
    parser.add_argument('--width', type=int, default=320)
    parser.add_argument('--height', type=int, default=320)
    parser.add_argument('--steps', type=int, default=50)
    args = parser.parse_args()
    if (args.frames <= 0 or args.frames % 4 != 1 or args.steps <= 0 or
            min(args.width, args.height) <= 0 or args.width % 32 or args.height % 32):
        parser.error('require frames=4n+1, positive steps and spatial multiples of 32')
    recipe = json.loads(RECIPE.read_bytes())
    latent = [48, (args.frames - 1) // 4 + 1, args.height // 16, args.width // 16]
    tokens = latent[1] * (latent[2] // 2) * (latent[3] // 2)
    padded = (tokens + 127) // 128 * 128
    layout = {}
    for component in COMPONENTS:
        path = getattr(args, component + '_catalog').resolve(strict=True)
        catalog = json.loads(path.read_bytes())
        facts = catalog['preparation_facts']
        if facts['schema'] != 'ndnsf-di-wan-preparation-facts-v1':
            raise ValueError('catalog is missing native preparation facts')
        if (facts['source_digest'] != catalog['source']['digest'] or
                facts['initializer_digest'] != catalog['source']['initializer_digest']):
            raise ValueError('catalog preparation facts differ from its source')
        if not catalog['source']['data_name'].startswith(args.namespace + '/requester/'):
            raise ValueError('catalog Producer differs from the experiment requester')
        if catalog['model']['source_revision'] != recipe['revision']:
            raise ValueError('catalog model revision differs from the pinned recipe')
        template = catalog['splitter']['step_template_v2']
        stages = [stage['unit_role_ids'] for stage in template['stage_layout']]
        expected_degrees = {'single': [1], 'tp': [2], 'pp': [1, 1],
                            'pp-tp': [1, 2], 'tp-pp': [2, 1]}[args.mode]
        if component != 'dit' and args.mode in ('pp-tp', 'tp-pp'):
            expected_degrees = [1, 1]
        if list(map(len, stages)) != expected_degrees:
            raise ValueError(component + ' catalog layout differs from the requested mode')
        expected_inputs = {
            'umt5': {'token-ids': ('int64', [1, 512]), 'key-mask': ('bool', [1, 512])},
            'dit': {'latent': ('float32', [1] + latent),
                    'context': ('float32', [1, 'valid_tokens', 4096]),
                    'timestep': ('int64', [1]), 'video-valid': ('bool', [1, 1, 1, padded])},
            'vae': {'latent-clip': ('float32', [1] + latent)}}[component]
        inputs = {}
        for step in template['steps']:
            for slot in step['input_slots']:
                if slot['source'] == 'EPOCH_INPUT':
                    value = (slot['dtype'], slot['shape'])
                    if slot['slot_id'] in inputs and inputs[slot['slot_id']] != value:
                        raise ValueError('catalog has conflicting epoch input shapes')
                    inputs[slot['slot_id']] = value
        if inputs != expected_inputs:
            raise ValueError(component + ' catalog geometry differs from the workload')
        layout[component] = dict(catalog=str(path), candidate_digest=facts['candidate_digest'],
                                stages=stages)
    tokenizer = next(row for row in recipe['sources']
                     if row['file'] == 'google/umt5-xxl/tokenizer.json')
    tokenizer_file = args.asset_root.resolve(strict=True) / tokenizer['file']
    if tokenizer_file.stat().st_size != tokenizer['bytes']:
        raise ValueError('tokenizer length differs from the pinned recipe')
    generation = dict(latent_shape=latent, padded_video_tokens=padded,
        text_width=4096, vae_spatial_stride=16, frames_per_second=24, steps=args.steps,
        shift=5.0, guidance=5.0, seed=0,
        positive_prompt='NDN distributed inference', negative_prompt='blurry',
        tokenizer=dict(file=str(tokenizer_file), digest='sha256:' + tokenizer['sha256'], slots=512))
    workload = dict(schema='ndnsf-di-wan-checkpoint-workload-v1', namespace=args.namespace,
        mode=args.mode, generation=generation, modes={args.mode: layout})
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(workload, output, sort_keys=True, indent=2)
        output.write('\n')


if __name__ == '__main__':
    main()
