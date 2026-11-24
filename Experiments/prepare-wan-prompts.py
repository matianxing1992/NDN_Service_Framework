#!/usr/bin/env python3
"""Apply Wan2.1 prompt cleaning to a native or managed Requester config on the host."""

import argparse
import html
import json
import os
from pathlib import Path

import ftfy
import regex


def clean_prompt(text):
    if not isinstance(text, str) or len(text.encode('utf-8')) > 1024 * 1024:
        raise ValueError('Wan prompt must be UTF-8 text up to 1 MiB')
    cleaned = html.unescape(html.unescape(ftfy.fix_text(text))).strip()
    return regex.sub(r'\s+', ' ', cleaned).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    if args.input.parent.resolve() != args.output.parent.resolve():
        parser.error('input and output must share a directory to preserve relative config paths')
    config = json.loads(args.input.read_text(encoding='utf-8'))
    generation = config.get('requester', config)['wan_generation']
    if not isinstance(generation.get('tokenizer'), dict):
        parser.error('Wan generation requires a digest-bound tokenizer configuration')
    if 'positive_tokens' in generation or 'negative_tokens' in generation:
        parser.error('Wan raw prompts conflict with explicit token IDs')
    for field in ('positive_prompt', 'negative_prompt'):
        generation[field] = clean_prompt(generation[field])

    encoded = json.dumps(config, ensure_ascii=False, sort_keys=True, indent=2) + '\n'
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as output:
            output.write(encoded)
    except BaseException:
        args.output.unlink()
        raise


if __name__ == '__main__':
    main()
