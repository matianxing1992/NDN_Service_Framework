#!/usr/bin/env python3
"""Compile changing YOLO apps locally inside the exact base/SDK SIF.

Cache identity follows the base/toolchain command, not an application commit.
This produces a candidate bundle; experiment qualification is separate.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2]))
from runtime.application import verify_application
FLAGS = '-O1 -g0 -B/usr/bin/ -DBOOST_PHOENIX_DONT_USE_PREPROCESSED_FILES'
TARGETS = ('App_ServiceController', 'di-native-provider', 'di-native-fault-provider')
MAX_JOBS = 4


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return 'sha256:' + h.hexdigest()


def reusable_application(root, seal, base_sha256):
    """Reuse compiled bytes only for an identical sealed source and toolchain."""
    root = root.resolve()
    body = verify_application(root, manifest_sha256=digest(root/'application-manifest.json'),
                              base_sif_sha256=base_sha256)
    if (body['sourceSealDigest'] != seal['sealDigest']
            or body['sourceRevision'] != seal['sourceRevision']
            or body['buildIdentity']['flags'] != FLAGS):
        raise ValueError('APP_REPACKAGE_SOURCE_OR_FLAGS_CHANGED')
    return body


def compatible_build_cache(cache, application, base_sha256, *, previous_base_sha256=None):
    """Reuse Waf state, never skip configure/build, for matching base and flags."""
    application = application.resolve()
    body = verify_application(application,
        manifest_sha256=digest(application/'application-manifest.json'),
        base_sif_sha256=previous_base_sha256 or base_sha256)
    identity = body['buildIdentity']
    if identity['flags'] != FLAGS:
        raise ValueError('APP_CACHE_FLAGS_CHANGED')
    work = cache/body['buildKey']
    marker = work/'cache-identity.json'
    if (any(p.is_symlink() for p in (marker, work, *work.parents))
            or not marker.is_file() or json.loads(marker.read_text()) != identity):
        raise ValueError('APP_CACHE_IDENTITY')
    return work, identity


def python_repack_parent(record, source_seal_sha256):
    """Accept only the installed base repacker's narrow compatibility record.

    The caller hashes the actual SIF and obtains this record from that image.
    Native reuse still runs the new base verifier, configure, and Waf.
    """
    import re
    if (not isinstance(record, dict) or set(record) != {'schema', 'parentSifSha256',
            'previousSealSha256', 'sourceSealSha256', 'files'}
            or record['schema'] != 'spec183-base-python-repack-v1'
            or record['sourceSealSha256'] != source_seal_sha256
            or not isinstance(record['files'], list) or not record['files']):
        raise ValueError('APP_BASE_REPACK_RECORD')
    for key in ('parentSifSha256', 'previousSealSha256', 'sourceSealSha256'):
        if not re.fullmatch(r'sha256:[0-9a-f]{64}', str(record[key])):
            raise ValueError('APP_BASE_REPACK_DIGEST')
    seen = set()
    for row in record['files']:
        if (not isinstance(row, dict) or set(row) != {'path', 'installed',
                'oldSha256', 'sha256', 'bytes'}):
            raise ValueError('APP_BASE_REPACK_FILE')
        name = row.get('path', '')
        if (not name.startswith(('pythonWrapper/ndnsf/',
                                 'NDNSF-DistributedRepo/pythonWrapper/py_repoclient/'))
                or not name.endswith('.py') or '..' in Path(name).parts):
            raise ValueError('APP_BASE_REPACK_NON_PYTHON')
        if (name in seen or row['installed'] != name.split('pythonWrapper/', 1)[1]
                or type(row['bytes']) is not int or row['bytes'] < 0
                or any(not re.fullmatch(r'sha256:[0-9a-f]{64}', str(row[key]))
                       for key in ('oldSha256', 'sha256'))):
            raise ValueError('APP_BASE_REPACK_FILE')
        seen.add(name)
    return record['parentSifSha256']


def publish_cache(cache, work, identity, key):
    """Keep the latest application's buildKey usable for the next increment."""
    destination = cache/key
    if destination != work and destination.exists():
        raise ValueError('APP_CACHE_DESTINATION_EXISTS')
    (work/'cache-identity.json').write_text(json.dumps(identity, sort_keys=True)+'\n')
    if destination != work:
        work.rename(destination)


def run(args):
    source, base, cache, output = (getattr(args, name).resolve()
                                  for name in ('source', 'base', 'cache', 'output'))
    assert not output.exists(), 'APP_OUTPUT_EXISTS'
    assert digest(base) == args.base_sha256, 'APP_BASE_DIGEST'
    spec = importlib.util.spec_from_file_location('source_validator', HERE / 'validate-local-sif-source.py')
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)
    validator.validate(source / 'source-seal.json')
    seal = json.loads((source / 'source-seal.json').read_text())
    assert seal.get('sourceSelection', 'legacy-complete') == 'legacy-complete', 'APP_SOURCE_SELECTION'
    base_seal_wire = subprocess.check_output([
        str(args.apptainer), 'exec', '--cleanenv', str(base), 'cat',
        '/opt/ndnsf-di/current/manifest/base-source-seal.json'])
    base_seal = json.loads(base_seal_wire)
    source_files = {row['path']: row for row in seal['files']}
    for row in base_seal['files']:
        assert source_files.get(row['path']) == row, 'APP_CHANGED_BASE_SOURCE:' + row['path']
    jobs = int(args.jobs)
    if not 1 <= jobs <= MAX_JOBS:
        raise ValueError('APP_BUILD_JOBS_OUT_OF_RANGE')
    build_identity = {'baseSifSha256': args.base_sha256, 'flags': FLAGS,
                      'builderSha256': digest(__file__), 'targets': TARGETS,
                      'jobs': jobs}
    reuse = getattr(args, 'reuse_application', None)
    if reuse is not None:
        # Preserve the original compiler provenance; this run only packages
        # already verified binary bytes with files from the same source seal.
        build_identity = reusable_application(reuse, seal, args.base_sha256)['buildIdentity']
    key = hashlib.sha256(json.dumps(build_identity, sort_keys=True).encode()).hexdigest()
    work = cache / key
    cache_identity = build_identity
    cache_from = getattr(args, 'build_cache_from', None)
    previous_base = None
    if getattr(args, 'python_repacked_base', False):
        if cache_from is None or reuse is not None:
            raise ValueError('APP_BASE_REPACK_REQUIRES_INCREMENTAL_BUILD')
        record = json.loads(subprocess.check_output([
            str(args.apptainer), 'exec', '--cleanenv', str(base), 'cat',
            '/opt/ndnsf-di/current/manifest/base-python-repack.json']))
        previous_base = python_repack_parent(record,
            'sha256:' + hashlib.sha256(base_seal_wire).hexdigest())
    if cache_from is not None:
        if reuse is not None:
            raise ValueError('APP_CACHE_AND_REPACKAGE_EXCLUSIVE')
        work, cache_identity = compatible_build_cache(cache, cache_from, args.base_sha256,
                                                      previous_base_sha256=previous_base)
        if work != cache/key and (cache/key).exists():
            raise ValueError('APP_CACHE_DESTINATION_EXISTS')
    work.mkdir(parents=True, exist_ok=True)
    # Only the cache created by this owner may be refreshed or pruned.
    marker = work / 'cache-identity.json'
    if marker.exists():
        assert json.loads(marker.read_text()) == json.loads(json.dumps(cache_identity)), 'APP_CACHE_IDENTITY'
    else:
        assert not any(work.iterdir()), 'APP_CACHE_UNOWNED'
        marker.write_text(json.dumps(build_identity, sort_keys=True) + '\n')
    repo, build = work / 'repo', work / 'build'
    repo.mkdir(exist_ok=True)
    build.mkdir(exist_ok=True)
    previous_path = work / 'source-seal.json'
    names = {row['path'] for row in seal['files']}
    if previous_path.exists():
        previous = json.loads(previous_path.read_text())
        for name in {row['path'] for row in previous['files']} - names:
            path = (repo / name).resolve()
            assert repo in path.parents, 'APP_CACHE_PATH_ESCAPE'
            path.unlink(missing_ok=True)
    with tarfile.open(source / 'workspace.tar') as archive:
        for member in archive.getmembers():
            assert member.isfile() and member.name in names, 'APP_ARCHIVE_MEMBER'
            target = repo / member.name
            expected = source_files[member.name]
            assert not target.is_symlink(), 'APP_CACHE_SYMLINK'
            assert repo in target.resolve().parents, 'APP_CACHE_PATH_ESCAPE'
            if not target.exists() or digest(target) != expected['sha256']:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as stream, target.open('wb') as dest:
                    shutil.copyfileobj(stream, dest)
                target.chmod(member.mode & 0o777)
    shutil.copyfile(source / 'source-seal.json', previous_path)
    command = [str(args.apptainer), 'exec', '--cleanenv', '--bind', f'{repo}:/src',
               '--bind', f'{build}:/build', '--pwd', '/src',
               '--env', 'CC=/usr/bin/gcc', '--env', 'CXX=/usr/bin/g++',
               '--env', 'CXXFLAGS=' + FLAGS,
               '--env', 'LDFLAGS=-B/usr/bin/ -Wl,-rpath,/opt/ndnsf-di/current/lib',
               '--env', 'CPLUS_INCLUDE_PATH=/opt/ndnsf-di/current/include', str(base)]
    if reuse is None:
        compile_application(command, jobs)
    partial = output.with_name(output.name + '.partial')
    partial.mkdir(parents=True, exist_ok=False)
    (partial / 'bin').mkdir()
    binary_root = reuse.resolve()/'bin' if reuse is not None else build/'examples'
    for target in TARGETS:
        shutil.copy2(binary_root / target, partial / 'bin' / target)
    for name in ('NDNSF-DistributedInference/ndnsf_distributed_inference',
                 'examples/python/NDNSF-DistributedInference/yolo_2x2'):
        shutil.copytree(repo / name, partial / 'repo' / name,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    for name in ('Experiments/NDNSF_DI_YoloAckDriven_Minindn.py',
                 'Experiments/NDNSF_DI_Yolo2x2_Minindn.py',
                 'Experiments/NDNSF_NewAPI_Minindn_Perf.py',
                 'Experiments/minindn_network_resources.py', 'examples/trust-schema.conf',
                 'tests/fixtures/spec180/yolo26n/fixed-fixture.ppm'):
        dest = partial / 'repo' / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / name, dest)
    rows = [{'path': str(p.relative_to(partial)), 'bytes': p.stat().st_size, 'sha256': digest(p)}
            for p in sorted(partial.rglob('*')) if p.is_file()]
    manifest = {'schemaVersion': 'spec183-external-application-v1', 'layout': 'layered-v1',
                'scope': 'BUILT_APPLICATION_CANDIDATE', 'baseSifSha256': args.base_sha256,
                'sourceSealDigest': seal['sealDigest'], 'sourceRevision': seal['sourceRevision'],
                'buildIdentity': build_identity, 'buildKey': key, 'files': rows}
    (partial / 'application-manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    for path in partial.rglob('*'):
        if path.is_file():
            path.chmod(0o555 if path.parent == partial / 'bin' else 0o444)
    verify_application(partial, manifest_sha256=digest(partial / 'application-manifest.json'),
                       base_sif_sha256=args.base_sha256)
    for path in partial.rglob('*'):
        if path.is_dir():
            path.chmod(0o555)
    partial.chmod(0o555)
    partial.rename(output)
    if cache_from is not None:
        publish_cache(cache, work, build_identity, key)
    print(json.dumps({'status': 'BUILT', 'scope': manifest['scope'], 'bundle': str(output),
                      'buildInvoked': reuse is None}))


def compile_application(command, jobs):
    subprocess.run(command + ['/opt/venv/bin/python',
                   '/opt/ndnsf-di/current/manifest/verify-base-runtime.py', 'verify'], check=True)
    subprocess.run(command + ['./waf', 'configure', '--out=/build', '--with-examples',
                   '--external-application-only',
                   '--application-component=di',
                   '--disable-local-dependency-prefix', '--nac-abe-prefix=/opt/ndnsf-di/current',
                   '--prefix=/opt/ndnsf-di/current', '--libdir=/opt/ndnsf-di/current/lib',
                   '--boost-includes=/usr/include', '--boost-libs=/usr/lib/x86_64-linux-gnu'], check=True)
    subprocess.run(command + ['./waf', f'-j{jobs}', '--targets=' + ','.join(TARGETS)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'base', 'cache', 'output', 'apptainer'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--base-sha256', required=True)
    parser.add_argument('--jobs', type=int, default=4,
                        help='Waf parallelism (1-4); lower values avoid GCC9 ICEs')
    parser.add_argument('--reuse-application', type=Path,
                        help='Repackage verified binaries only when source seal/base/flags are unchanged')
    parser.add_argument('--build-cache-from', type=Path,
                        help='Use a verified prior application build cache; configure and Waf still run')
    parser.add_argument('--python-repacked-base', action='store_true',
                        help='Allow a cache from the installed Python-repack parent; still configure/build')
    run(parser.parse_args())
