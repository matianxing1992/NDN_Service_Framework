#!/usr/bin/env python3
"""Repack sealed Python-only library changes; retain exact native artifacts."""
import argparse
import base64
import csv
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import sys
import tarfile

HERE = Path(__file__).resolve().parent
PACKAGES = ('pythonWrapper/ndnsf/', 'NDNSF-DistributedRepo/pythonWrapper/py_repoclient/')


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return 'sha256:' + h.hexdigest()


def changes(old, new):
    if any(s.get('sourceSelection') != 'base-libraries-v1' for s in (old, new)):
        raise ValueError('REPACK_BASE_SELECTION')
    if set(old['dependencies']) != set(new['dependencies']):
        raise ValueError('REPACK_DEPENDENCIES')
    for name, previous in old['dependencies'].items():
        current = new['dependencies'][name]
        if (previous['sourceRevision'] != current['sourceRevision']
                or previous['files'] != current['files']
                or previous['archive']['sha256'] != current['archive']['sha256']):
            raise ValueError('REPACK_DEPENDENCY_CHANGED:' + name)
    before = {r['path']: r for r in old['files']}
    after = {r['path']: r for r in new['files']}
    if set(before) != set(after):
        raise ValueError('REPACK_FILE_SET_CHANGED')
    result = []
    for name, row in after.items():
        if row == before[name]:
            continue
        prefix = next((p for p in PACKAGES if name.startswith(p)), None)
        if (prefix is None or not name.endswith('.py') or '..' in Path(name).parts
                or not re.fullmatch(r'[A-Za-z0-9_./-]+', name)):
            raise ValueError('REPACK_NON_PYTHON_CHANGE:' + name)
        result.append(dict(path=name, installed=name.split('pythonWrapper/', 1)[1],
                           oldSha256=before[name]['sha256'], sha256=row['sha256'], bytes=row['bytes']))
    if not result:
        raise ValueError('REPACK_NO_CHANGE')
    return result


def render(args):
    base, old_root, new_root, output = [p.resolve() for p in
                                       (args.base, args.previous_source, args.source, args.output)]
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', args.base_sha256) or digest(base) != args.base_sha256:
        raise ValueError('REPACK_PARENT_SIF')
    loader = importlib.util.spec_from_file_location('seal_validator', HERE / 'validate-local-sif-source.py')
    validator = importlib.util.module_from_spec(loader)
    loader.loader.exec_module(validator)
    for root in (old_root, new_root):
        validator.validate(root / 'source-seal.json')
    old, new = [json.loads((r / 'source-seal.json').read_text()) for r in (old_root, new_root)]
    rows = changes(old, new)
    for path in (base, output, HERE):
        if not re.fullmatch(r'[A-Za-z0-9_./-]+', str(path)):
            raise ValueError('REPACK_DEFINITION_PATH')
    output.mkdir(mode=0o700)
    descriptor = dict(schema='spec183-base-python-repack-v1', parentSifSha256=args.base_sha256,
                      previousSealSha256=digest(old_root / 'source-seal.json'),
                      sourceSealSha256=digest(new_root / 'source-seal.json'), files=rows)
    (output / 'source-seal.json').write_bytes((new_root / 'source-seal.json').read_bytes())
    with tarfile.open(new_root / 'workspace.tar') as archive:
        for i, row in enumerate(rows):
            member = archive.getmember(row['path'])
            if not member.isfile():
                raise ValueError('REPACK_PAYLOAD_TYPE')
            payload = archive.extractfile(member).read()
            target = output / ('patch-' + str(i) + '.py')
            target.write_bytes(payload)
            if len(payload) != row['bytes'] or digest(target) != row['sha256']:
                raise ValueError('REPACK_PAYLOAD_DIGEST')
    (output / 'repack.json').write_text(json.dumps(descriptor, sort_keys=True, indent=2) + '\n')
    definition = f'''Bootstrap: localimage
From: {base}

%files
    {output} /base-python-update
    {HERE}/repack-base-python.py /base-python-update/apply.py

%post
    set -eu
    /opt/venv/bin/python /base-python-update/apply.py apply /base-python-update
    rm -rf /base-python-update

%test
    /opt/venv/bin/python /opt/ndnsf-di/current/manifest/verify-base-runtime.py verify
'''
    (output / 'runtime.def').write_text(definition)
    print(json.dumps(dict(status='RENDERED', changedPythonFiles=len(rows), nativeBuildRequired=False)))


def apply(root):
    manifest = Path('/opt/ndnsf-di/current/manifest')
    desc = json.loads((root / 'repack.json').read_text())
    if digest(manifest / 'base-source-seal.json') != desc['previousSealSha256']:
        raise ValueError('REPACK_INSTALLED_SOURCE_MISMATCH')
    if digest(root / 'source-seal.json') != desc['sourceSealSha256']:
        raise ValueError('REPACK_NEW_SOURCE_MISMATCH')
    previous = json.loads((manifest / 'base-source-seal.json').read_text())
    current = json.loads((root / 'source-seal.json').read_text())
    if changes(previous, current) != desc['files']:
        raise ValueError('REPACK_CHANGE_DESCRIPTOR')
    loader = importlib.util.spec_from_file_location('base_verify', manifest / 'verify-base-runtime.py')
    verifier = importlib.util.module_from_spec(loader)
    loader.loader.exec_module(verifier)
    before = verifier.inspect()
    if before != json.loads((manifest / 'base-runtime.json').read_text()):
        raise ValueError('REPACK_PARENT_RUNTIME_CHANGED')
    site = Path('/opt/venv/lib/python3.10/site-packages')
    for i, row in enumerate(desc['files']):
        target, payload = site / row['installed'], root / ('patch-' + str(i) + '.py')
        if target.is_symlink() or digest(target) != row['oldSha256'] or digest(payload) != row['sha256']:
            raise ValueError('REPACK_INSTALLED_PYTHON_CHANGED')
        wire = payload.read_bytes()
        compile(wire, str(target), 'exec')
        target.write_bytes(wire)
        for cache in (target.parent / '__pycache__').glob(target.stem + '.*.pyc'):
            cache.unlink()
        matches = 0
        for record in site.glob('*.dist-info/RECORD'):
            entries = list(csv.reader(io.StringIO(record.read_text())))
            touched = False
            for entry in entries:
                if entry[0] == row['installed']:
                    entry[1:] = ['sha256=' + base64.urlsafe_b64encode(hashlib.sha256(wire).digest()).decode().rstrip('='), str(len(wire))]
                    matches += 1
                    touched = True
            if touched:
                with record.open('w', newline='') as stream:
                    csv.writer(stream).writerows(entries)
        if matches != 1:
            raise ValueError('REPACK_WHEEL_RECORD')
    (manifest / 'base-source-seal.json').write_bytes((root / 'source-seal.json').read_bytes())
    after = verifier.inspect()
    if after['artifacts'] != before['artifacts']:
        raise ValueError('REPACK_NATIVE_CHANGED')
    (manifest / 'base-runtime.json').write_text(json.dumps(after, sort_keys=True, indent=2) + '\n')
    (manifest / 'base-python-repack.json').write_text(json.dumps(desc, sort_keys=True, indent=2) + '\n')
    print(json.dumps(dict(status='PASS', scope='PYTHON_REPACK_ONLY', nativeArtifactsUnchanged=len(after['artifacts']))))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    build = sub.add_parser('render')
    for flag in ('base', 'previous-source', 'source', 'output'):
        build.add_argument('--' + flag, type=Path, required=True)
    build.add_argument('--base-sha256', required=True)
    install = sub.add_parser('apply')
    install.add_argument('root', type=Path)
    args = parser.parse_args()
    render(args) if args.action == 'render' else apply(args.root)
