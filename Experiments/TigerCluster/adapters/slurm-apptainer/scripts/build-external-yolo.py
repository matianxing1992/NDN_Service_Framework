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


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return 'sha256:' + h.hexdigest()


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
    base_seal = json.loads(subprocess.check_output([
        str(args.apptainer), 'exec', '--cleanenv', str(base), 'cat',
        '/opt/ndnsf-di/current/manifest/base-source-seal.json'], text=True))
    source_files = {row['path']: row for row in seal['files']}
    for row in base_seal['files']:
        assert source_files.get(row['path']) == row, 'APP_CHANGED_BASE_SOURCE:' + row['path']
    build_identity = {'baseSifSha256': args.base_sha256, 'flags': FLAGS,
                      'builderSha256': digest(__file__), 'targets': TARGETS}
    key = hashlib.sha256(json.dumps(build_identity, sort_keys=True).encode()).hexdigest()
    work = cache / key
    work.mkdir(parents=True, exist_ok=True)
    # Only the cache created by this owner may be refreshed or pruned.
    marker = work / 'cache-identity.json'
    if marker.exists():
        assert json.loads(marker.read_text()) == json.loads(json.dumps(build_identity)), 'APP_CACHE_IDENTITY'
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
    subprocess.run(command + ['/opt/venv/bin/python',
                   '/opt/ndnsf-di/current/manifest/verify-base-runtime.py', 'verify'], check=True)
    subprocess.run(command + ['./waf', 'configure', '--out=/build', '--with-examples',
                   '--disable-local-dependency-prefix', '--nac-abe-prefix=/opt/ndnsf-di/current',
                   '--prefix=/opt/ndnsf-di/current', '--libdir=/opt/ndnsf-di/current/lib',
                   '--boost-includes=/usr/include', '--boost-libs=/usr/lib/x86_64-linux-gnu'], check=True)
    subprocess.run(command + ['./waf', '-j2', '--targets=' + ','.join(TARGETS)], check=True)
    partial = output.with_name(output.name + '.partial')
    partial.mkdir(parents=True, exist_ok=False)
    (partial / 'bin').mkdir()
    for target in TARGETS:
        shutil.copy2(build / 'examples' / target, partial / 'bin' / target)
    for name in ('NDNSF-DistributedInference/ndnsf_distributed_inference',
                 'examples/python/NDNSF-DistributedInference/yolo_2x2'):
        shutil.copytree(repo / name, partial / 'repo' / name,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    for name in ('Experiments/NDNSF_DI_YoloAckDriven_Minindn.py',
                 'Experiments/minindn_network_resources.py', 'examples/trust-schema.conf'):
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
    verify_application(partial, manifest_sha256=digest(partial / 'application-manifest.json'),
                       base_sif_sha256=args.base_sha256)
    partial.rename(output)
    print(json.dumps({'status': 'BUILT', 'scope': manifest['scope'], 'bundle': str(output)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'base', 'cache', 'output', 'apptainer'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--base-sha256', required=True)
    run(parser.parse_args())
