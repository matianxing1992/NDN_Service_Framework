#!/usr/bin/env python3
"""Cross-check host prompt preparation and produce a SentencePiece oracle for native UMT5."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import sentencepiece


ROOT = Path(__file__).resolve().parents[3]
TOKENIZER = 'google/umt5-xxl/tokenizer.json'
SENTENCEPIECE = 'google/umt5-xxl/spiece.model'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--asset-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    recipe = json.loads((ROOT / 'Experiments/recipes/wan2.1-t2v-14b-metadata.json').read_text())
    sources = {entry['file']: entry for entry in recipe['sources']}
    for filename in (TOKENIZER, SENTENCEPIECE):
        path = args.asset_root / filename
        source = sources[filename]
        if (path.stat().st_size != source['bytes'] or
                hashlib.sha256(path.read_bytes()).hexdigest() != source['sha256']):
            raise ValueError('wrong asset identity: ' + filename)

    spm = sentencepiece.SentencePieceProcessor(model_file=str(args.asset_root / SENTENCEPIECE))
    if spm.eos_id() != 1 or spm.pad_id() != 0:
        raise ValueError('unexpected Wan UMT5 EOS or PAD')
    cases = [
        (16, {'positive': '  cafÃ©\u2003&amp;amp;\u00a0雪  ', 'negative': '\t\u2028'},
             {'positive': 'café & 雪', 'negative': ''}),
        (3, {'positive': 'ＡＢＣ &amp;amp; 松树\n snow', 'negative': 'a quiet lake'},
            {'positive': 'ABC & 松树 snow', 'negative': 'a quiet lake'}),
    ]
    output = []
    for slots, raw, expected in cases:
        generation = {
            'tokenizer': {'file': TOKENIZER, 'digest': 'sha256:' + sources[TOKENIZER]['sha256'],
                          'slots': slots},
            **{f'{branch}_prompt': text for branch, text in raw.items()}
        }
        with tempfile.TemporaryDirectory(prefix='wan-prompts-') as temporary:
            directory = Path(temporary)
            source, cleaned = directory / 'raw.json', directory / 'clean.json'
            source.write_text(json.dumps({'wan_generation': generation}), encoding='utf-8')
            subprocess.run([sys.executable, str(ROOT / 'Experiments/prepare-wan-prompts.py'),
                            '--input', str(source), '--output', str(cleaned)], check=True)
            native = json.loads(cleaned.read_text(encoding='utf-8'))['wan_generation']
        row = {'slots': slots}
        for branch, text in expected.items():
            if native[f'{branch}_prompt'] != text:
                raise ValueError('Wan prompt cleaning differs from reference: ' + branch)
            ids = spm.encode(text, out_type=int)[:slots - 1] + [spm.eos_id()]
            row[branch] = {'text': text, 'valid': len(ids),
                           'ids': ids + [spm.pad_id()] * (slots - len(ids))}
        output.append(row)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump({'tokenizer_digest': generation['tokenizer']['digest'], 'cases': output},
                  stream, ensure_ascii=False)
        stream.write('\n')


if __name__ == '__main__':
    main()
