#!/usr/bin/env python3
"""Pinned source acquisition and conservative installed-library receipts.

No shell evaluation of manifest values. plan/check are offline and read-only.
Each commit gets a separate checkout; existing developer trees are never reset.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LIBRARIES = {
    'ndn-cxx': 'libndn-cxx.so', 'ndn-svs': 'libndn-svs.so',
    'NDNSD': 'libndnsd.so', 'openabe': 'libopenabe.so', 'NAC-ABE': 'libnac-abe.so',
}


def load_lock(path):
    data = json.loads(Path(path).read_text())
    if data.get('schema') != 1 or set(data.get('sources', {})) != set(LIBRARIES):
        raise ValueError('lock must declare schema 1 and exactly the five supported source dependencies')
    for name, source in data['sources'].items():
        url = source.get('url', '')
        if not isinstance(url, str) or not url.startswith(('https://', 'ssh://', 'git@')) or any(c.isspace() for c in url):
            raise ValueError(f'{name}: expected an HTTPS/SSH repository URL')
        if not re.fullmatch(r'[0-9a-f]{40}', source.get('commit', '')):
            raise ValueError(f'{name}: a full 40-character commit is required, not a moving branch')
        if not isinstance(source.get('ref'), str) or not source['ref'] or any(c.isspace() for c in source['ref']):
            raise ValueError(f'{name}: missing/invalid descriptive branch or tag')
        deps = source.get('requires')
        if not isinstance(deps, list) or any(d not in LIBRARIES or d == name for d in deps):
            raise ValueError(f'{name}: invalid prerequisite list')
    order(data)  # Reject cycles before acquisition or installation.
    # Build recipes implement this graph; a fork changing it needs recipe review.
    required = {'ndn-cxx': set(), 'openabe': set(),
                'ndn-svs': {'ndn-cxx'}, 'NDNSD': {'ndn-cxx', 'ndn-svs'},
                'NAC-ABE': {'ndn-cxx', 'openabe'}}
    for name, deps in required.items():
        if deps != set(data['sources'][name]['requires']):
            raise ValueError(f'{name}: unsupported native prerequisite graph')
    return data


def order(data):
    pending = dict(data['sources'])
    result = []
    while pending:
        ready = [n for n, s in pending.items() if set(s['requires']).issubset(result)]
        if not ready:
            raise ValueError('dependency cycle in source lock')
        for name in ready:
            result.append(name)
            del pending[name]
    return result


def git(directory, *args):
    return subprocess.check_output(['git', '-C', str(directory), *args], text=True,
                                   env=dict(os.environ, GIT_TERMINAL_PROMPT='0')).strip()


def prepare(source, name, base):
    base = Path(base).absolute()
    directory = base / (name + '-' + source['commit'])
    if directory.is_symlink():
        raise ValueError(f'refusing symlink source directory: {directory}')
    if directory.exists():
        if not (directory / '.git').is_dir():
            raise ValueError(f'not an installer-owned standalone checkout: {directory}')
        if git(directory, 'remote', 'get-url', 'origin') != source['url']:
            raise ValueError(f'wrong remote in {directory}; existing tree left unchanged')
        if git(directory, 'status', '--porcelain', '--untracked-files=normal'):
            raise ValueError(f'dirty source tree: {directory}; preserve or move it explicitly')
        if git(directory, 'rev-parse', 'HEAD') != source['commit']:
            raise ValueError(f'wrong commit in {directory}; existing tree left unchanged')
    else:
        base.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'clone', '--no-checkout', '--', source['url'], str(directory)],
                       check=True, stdout=sys.stderr,
                       env=dict(os.environ, GIT_TERMINAL_PROMPT='0'))
        # Fetch by full object id, not the moving ref. Never fall back to HEAD.
        git(directory, 'fetch', '--no-tags', 'origin', source['commit'])
        git(directory, 'checkout', '--detach', source['commit'])
    git(directory, 'submodule', 'update', '--init', '--recursive')
    return directory


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def fingerprint(data, name, prefix):
    source = data['sources'][name]
    library = Path(prefix) / 'lib' / LIBRARIES[name]
    compiler = subprocess.check_output(['/usr/bin/g++', '--version'], text=True)
    upstream = {}
    for dependency in source['requires']:
        upstream[dependency] = {
            'library': digest(Path(prefix) / 'lib' / LIBRARIES[dependency]),
            'receipt': digest(Path(prefix) / 'share/ndnsf/source-receipts' / (dependency + '.json')),
        }
    return {'schema': 1, 'source': source, 'prefix': str(prefix),
            'compiler': compiler, 'compiler_sha256': digest('/usr/bin/g++'),
            'installer': digest(ROOT / 'install_ndnsf_stack.sh'),
            'source_helper': digest(__file__), 'upstream': upstream,
            'library_realpath': str(library.resolve()), 'library_sha256': digest(library)}


def receipt(data, name, prefix, record=False):
    target = Path(prefix) / 'share/ndnsf/source-receipts' / (name + '.json')
    expected = fingerprint(data, name, prefix)
    if record:
        target.parent.mkdir(parents=True, exist_ok=True)
        temp = target.with_suffix('.json.' + str(os.getpid()))
        try:
            with temp.open('x') as output:
                json.dump(expected, output, indent=2, sort_keys=True)
                output.write('\n')
            temp.chmod(0o644)
            temp.replace(target)
        finally:
            if temp.exists():
                temp.unlink()
        return True
    return target.is_file() and json.loads(target.read_text()) == expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['plan', 'field', 'prepare', 'check', 'record'])
    parser.add_argument('--lock', required=True)
    parser.add_argument('--name', choices=tuple(LIBRARIES))
    parser.add_argument('--field', choices=['url', 'commit', 'ref'])
    parser.add_argument('--deps-dir')
    parser.add_argument('--prefix', default='/usr/local')
    args = parser.parse_args()
    try:
        data = load_lock(args.lock)
        if args.action == 'plan':
            for name in order(data):
                s = data['sources'][name]
                print(f"{name}: {s['url']} ref={s['ref']} commit={s['commit']}")
            return 0
        if not args.name:
            parser.error('--name is required')
        if args.action == 'field':
            if not args.field:
                parser.error('--field is required')
            print(data['sources'][args.name][args.field])
        elif args.action == 'prepare':
            if not args.deps_dir:
                parser.error('--deps-dir is required')
            print(prepare(data['sources'][args.name], args.name, args.deps_dir))
        else:
            return 0 if receipt(data, args.name, args.prefix, args.action == 'record') else 1
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'source-lock: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
