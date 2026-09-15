#!/usr/bin/env python3
"""Check delivered SIF bytes against an existing build receipt, never repin."""
import argparse
import hashlib
import json
from pathlib import Path


def verify(sif, record):
    receipt = json.loads(Path(record).read_text())
    expected = receipt['sif']
    digest = hashlib.sha256()
    with Path(sif).open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(block)
    actual = 'sha256:' + digest.hexdigest()
    ok = (receipt.get('status') == 'PASS'
          and actual == expected['sha256']
          and Path(sif).stat().st_size == expected['bytes'])
    return {'status': 'PASS' if ok else 'REJECTED',
            'expectedSha256': expected['sha256'], 'actualSha256': actual,
            'record': str(record), 'sif': str(sif),
            'reason': None if ok else 'SIF_BUILD_RECEIPT_MISMATCH'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sif', required=True, type=Path)
    parser.add_argument('--record', required=True, type=Path)
    args = parser.parse_args()
    result = verify(args.sif, args.record)
    print(json.dumps(result, sort_keys=True))
    raise SystemExit(0 if result['status'] == 'PASS' else 1)
