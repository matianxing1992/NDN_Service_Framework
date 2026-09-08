#!/usr/bin/env python3
"""Render the local layered base build after checking pinned inputs.

This prepares a base/SDK image only; application and experiment gates remain open.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil

HERE = Path(__file__).resolve().parent


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return 'sha256:' + h.hexdigest()


def render(source, base, wheels, lock_path, output):
    source, base, wheels, output = map(lambda p: Path(p).resolve(), (source, base, wheels, output))
    for path in (source, base, wheels, output, HERE):
        if any(c.isspace() for c in str(path)):
            raise ValueError('BASE_DEFINITION_PATH_WHITESPACE')
    spec = importlib.util.spec_from_file_location('source_validator', HERE / 'validate-local-sif-source.py')
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)
    validator.validate(source / 'source-seal.json')
    seal = json.loads((source / 'source-seal.json').read_text())
    assert seal['sourceSelection'] == 'base-libraries-v1', 'BASE_SOURCE_SELECTION'
    lock = json.loads(Path(lock_path).read_text())
    for name in ('nacAbe', 'ndnSvs', 'ndnSd'):
        assert seal['dependencies'][name]['sourceRevision'] == lock['repositories'][name]['revision'], 'BASE_DEPENDENCY_REVISION:' + name
    assert base.stat().st_size == lock['baseSif']['bytes'], 'BASE_INPUT_SIZE'
    assert digest(base) == lock['baseSif']['sha256'], 'BASE_INPUT_DIGEST'
    for row in lock['wheels']:
        assert digest(wheels / row['filename']) == row['sha256'], 'BASE_WHEEL_DIGEST'
    # One expanded 6.5 GiB base, SDK/build workspace, final compressed SIF.
    assert shutil.disk_usage(output.parent).free >= 12 * 1024**3, 'BASE_BUILD_SPACE_BELOW_12_GIB'
    template = (HERE.parent / 'templates/library-runtime.def.in').read_text()
    for key, value in {'BASE_SIF': base, 'SOURCE': source, 'WHEELS': wheels,
                       'SCRIPTS': HERE, 'SEAL_SHA256': digest(source / 'source-seal.json')[7:]}.items():
        template = template.replace('@' + key + '@', str(value))
    assert '@' not in template, 'BASE_TEMPLATE_UNRESOLVED'
    with output.open('x') as stream:
        stream.write(template)
    return {'status': 'RENDERED', 'scope': 'BASE_BUILD_ONLY', 'definition': str(output),
            'sourceSealDigest': seal['sealDigest'], 'definitionSha256': digest(output)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'base', 'wheels', 'lock', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(render(args.source, args.base, args.wheels, args.lock, args.output)))
