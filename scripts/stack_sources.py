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
import shlex
import subprocess
import sys
import tarfile
import tempfile

REPOSITORY = Path(__file__).resolve().parents[1]

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
        revision = source.setdefault('build_recipe_revision', 1)
        if not isinstance(revision, int) or isinstance(revision, bool) or revision < 1:
            raise ValueError(f'{name}: build_recipe_revision must be a positive integer')
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
        if 'patch_series' in data['sources'][name]:
            if name not in ('NAC-ABE', 'openabe'):
                raise ValueError(f'{name}: no patched source recipe is defined')
            patch_series(data['sources'][name])
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


def patch_series(source, root=None):
    """Validate the locked series and return immutable patch bytes in order."""
    root = (REPOSITORY if root is None else Path(root)).resolve()
    descriptor = source.get('patch_series')
    if not isinstance(descriptor, dict):
        raise ValueError('patched source requires a locked patch_series')
    relative_name = descriptor.get('path')
    if not isinstance(relative_name, str) or not relative_name:
        raise ValueError('patch series must have a repository-relative path')
    relative = Path(relative_name)
    if relative.is_absolute() or '..' in relative.parts or not relative.parts:
        raise ValueError('patch series must be repository-relative')
    path = (root / relative).resolve()
    if root not in path.parents:
        raise ValueError('patch series escapes the repository')
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != descriptor.get('sha256'):
        raise ValueError('patch series SHA256 differs from source lock')
    series = json.loads(raw)
    if series.get('schema') != 1 or series.get('upstreamCommit') != source['commit']:
        raise ValueError('patch series schema/upstream commit differs from source lock')
    entries = series.get('patches')
    if not isinstance(entries, list) or not entries:
        raise ValueError('patch series must contain ordered patches')
    patches = []
    seen = set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError('patch entry must declare file and SHA256')
        filename = entry.get('file', '')
        if not isinstance(filename, str) or not filename or Path(filename).name != filename or filename in ('.', '..') or filename in seen:
            raise ValueError('patch filename must be a unique basename')
        patch = (path.parent / filename).resolve()
        if patch.parent != path.parent:
            raise ValueError('patch escapes its series directory')
        content = patch.read_bytes()
        if hashlib.sha256(content).hexdigest() != entry.get('sha256'):
            raise ValueError(f'patch SHA256 differs from series: {filename}')
        seen.add(filename)
        patches.append((filename, content))
    return patches


def sdk_prefix(data, name, prefix):
    source = data['sources'][name]
    if name != 'NAC-ABE':
        raise ValueError('only NAC-ABE has a parallel patched SDK')
    patch_series(source)
    identity = source['commit'] + '-' + source['patch_series']['sha256']
    return Path(prefix) / 'ndnsf-sdk/nac-abe' / identity


def probe_nac_api(prefix):
    """Link the required symbol using the explicitly selected installed SDK."""
    env = dict(os.environ)
    for key in ('PKG_CONFIG_LIBDIR', 'CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH',
                'LIBRARY_PATH', 'LD_LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
        env.pop(key, None)
    selected = subprocess.check_output(
        ['/usr/bin/pkg-config', '--variable=prefix', 'libnac-abe'], text=True, env=env).strip()
    if Path(selected).resolve() != Path(prefix).resolve():
        raise ValueError('pkg-config does not select the locked NAC SDK prefix')
    flags = shlex.split(subprocess.check_output(
        ['/usr/bin/pkg-config', '--cflags', '--libs', 'libnac-abe'], text=True, env=env))
    source = '''#include <nac-abe/consumer.hpp>
using Consumer = ndn::nacabe::Consumer;
using RequiredApi = decltype(&Consumer::consumeWithCkAuthorization);
RequiredApi volatile requiredApi = &Consumer::consumeWithCkAuthorization;
int main() { return requiredApi == nullptr; }
'''
    with tempfile.TemporaryDirectory(prefix='ndnsf-nac-api-') as work:
        subprocess.run(['/usr/bin/g++', '-std=c++17', '-x', 'c++', '-', '-o',
                        str(Path(work) / 'probe'), *flags,
                        '-Wl,-rpath,' + str(Path(prefix) / 'lib')], input=source,
                       text=True, env=env, check=True)
        verify_nac_runtime(prefix, [Path(work) / 'probe'], require_nac=True)


def verify_nac_runtime(prefix, objects, require_nac=False):
    expected = (Path(prefix) / 'lib/libnac-abe.so').resolve()
    env = dict(os.environ)
    env.pop('LD_LIBRARY_PATH', None)
    env.pop('LD_PRELOAD', None)
    for path in objects:
        result = subprocess.run(['/usr/bin/ldd', '-r', str(path)], text=True,
                                capture_output=True, env=env)
        report = result.stdout + result.stderr
        if result.returncode or 'not found' in report or 'undefined symbol:' in report:
            raise ValueError(f'incomplete runtime closure for {path}:\n{report}')
        matches = re.findall(r'^\s*libnac-abe\.so(?:\.\S+)?\s+=>\s+(\S+)', report, re.M)
        if require_nac and not matches:
            raise ValueError(f'runtime does not load required NAC SDK: {path}')
        if any(Path(value).resolve() != expected for value in matches):
            raise ValueError(f'runtime loads a different NAC ABI for {path}:\n{report}')


def materialize(source, name, checkout, base):
    """Patch an isolated archive; never change the pinned checkout."""
    patches = patch_series(source)
    checkout = Path(checkout).absolute()
    if checkout.is_symlink() or not (checkout / '.git').is_dir():
        raise ValueError('materialization requires a standalone pinned checkout')
    if git(checkout, 'rev-parse', 'HEAD') != source['commit'] or git(checkout, 'remote', 'get-url', 'origin') != source['url']:
        raise ValueError('materialization checkout differs from source lock')
    if git(checkout, 'status', '--porcelain', '--untracked-files=normal'):
        raise ValueError('dirty source tree; materialization leaves it unchanged')
    base = Path(base)
    base.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=name + '-patched-', dir=base))
    target = work / 'source'
    target.mkdir()
    try:
        archive = work / 'source.tar'
        git(checkout, 'archive', '--format=tar', '--output=' + str(archive.absolute()), source['commit'])
        with tarfile.open(archive) as stream:
            for member in stream.getmembers():
                relative = Path(member.name)
                if relative.is_absolute() or '..' in relative.parts or not (member.isfile() or member.isdir()):
                    raise ValueError('source archive contains unsupported path or link')
            stream.extractall(target)
        archive.unlink()
        # A private Git boundary prevents discovery of the application's parent
        # repository when BUILD_DIR itself is inside that repository.
        git(target, 'init', '--quiet')
        patch_dir = work / 'patches'
        patch_dir.mkdir()
        for filename, content in patches:
            patch = patch_dir / filename
            patch.write_bytes(content)
            git(target, 'apply', '--check', str(patch.absolute()))
            git(target, 'apply', str(patch.absolute()))
        return target
    except (ValueError, OSError, tarfile.TarError, subprocess.CalledProcessError) as error:
        raise ValueError(f'patched source failed; diagnostic tree preserved at {work}: {error}') from error


def fingerprint(data, name, prefix, upstream_prefix=None):
    source = data['sources'][name]
    if 'patch_series' in source:
        patch_series(source)
    upstream_prefix = Path(prefix if upstream_prefix is None else upstream_prefix)
    library = Path(prefix) / 'lib' / LIBRARIES[name]
    compiler = subprocess.check_output(['/usr/bin/g++', '--version'], text=True)
    upstream = {}
    for dependency in source['requires']:
        upstream_receipt = (upstream_prefix / 'share/ndnsf/source-receipts' /
                            (dependency + '.json'))
        upstream[dependency] = {
            'library': digest(upstream_prefix / 'lib' / LIBRARIES[dependency]),
            # A compatible system-installed upstream may have no receipt from
            # this installer. Its library digest still binds the downstream
            # build to the exact upstream bytes without forcing provenance
            # fabrication or a redundant upstream rebuild.
            'receipt': digest(upstream_receipt) if upstream_receipt.is_file() else None,
        }
    result = {'schema': 1, 'source': source, 'prefix': str(prefix),
              'upstream_prefix': str(upstream_prefix),
              'compiler': compiler, 'compiler_sha256': digest('/usr/bin/g++'),
              'build_recipe_revision': source['build_recipe_revision'],
              'source_helper': digest(__file__), 'upstream': upstream,
              'library_realpath': str(library.resolve()), 'library_sha256': digest(library)}
    if name == 'ndn-svs':
        patch = Path(__file__).resolve().parents[1] / 'tools/ndn-svs/publication-latency.patch'
        result['patches'] = {'publication-latency.patch': digest(patch)}
    return result


def receipt(data, name, prefix, record=False, upstream_prefix=None):
    target = Path(prefix) / 'share/ndnsf/source-receipts' / (name + '.json')
    expected = fingerprint(data, name, prefix, upstream_prefix)
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
    parser.add_argument('action', choices=['plan', 'field', 'prepare', 'check', 'record', 'sdk-prefix', 'materialize', 'probe-api', 'check-runtime'])
    parser.add_argument('--lock', required=True)
    parser.add_argument('--name', choices=tuple(LIBRARIES))
    parser.add_argument('--field', choices=['url', 'commit', 'ref'])
    parser.add_argument('--deps-dir')
    parser.add_argument('--prefix', default='/usr/local')
    parser.add_argument('--upstream-prefix')
    parser.add_argument('--source-dir')
    parser.add_argument('--build-root')
    parser.add_argument('--runtime-object', nargs='+')
    parser.add_argument('--require-nac', action='store_true')
    args = parser.parse_args()
    try:
        data = load_lock(args.lock)
        if args.action == 'plan':
            for name in order(data):
                s = data['sources'][name]
                print(f"{name}: {s['url']} ref={s['ref']} commit={s['commit']} "
                      f"build_recipe_revision={s['build_recipe_revision']}")
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
        elif args.action == 'sdk-prefix':
            print(sdk_prefix(data, args.name, args.prefix))
        elif args.action == 'probe-api':
            if args.name != 'NAC-ABE':
                parser.error('probe-api is defined only for NAC-ABE')
            probe_nac_api(args.prefix)
        elif args.action == 'check-runtime':
            if not args.runtime_object:
                parser.error('--runtime-object is required')
            verify_nac_runtime(args.prefix, args.runtime_object, args.require_nac)
        elif args.action == 'materialize':
            if not args.source_dir or not args.build_root:
                parser.error('--source-dir and --build-root are required')
            print(materialize(data['sources'][args.name], args.name, args.source_dir, args.build_root))
        else:
            return 0 if receipt(data, args.name, args.prefix, args.action == 'record', args.upstream_prefix) else 1
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'source-lock: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
