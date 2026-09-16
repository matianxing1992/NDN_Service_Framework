#!/usr/bin/env python3
"""Seal ONNX sources, offline Cargo sources and official Rust installer inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ONNX_REVISION = 'b8baa8446686496da4cc8fda09f2b6fe65c2a02c'
RUST = {
    'cargo-1.90.0-x86_64-unknown-linux-gnu.tar.xz': '9853db03d68578a30972e2755c89c66aec035fec641cf8f3a7117c81eec2578d',
    'rust-std-1.90.0-x86_64-unknown-linux-gnu.tar.xz': '663f4ab7945b392d5e5294dec1b050a66820a20e86f084ec37eeb0f2f7ff5569',
    'rustc-1.90.0-x86_64-unknown-linux-gnu.tar.xz': '48c2a42de9e92fcae8c24568f5fe40d5734696a6f80e83cc6d46eef1a78f13c9',
}


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return 'sha256:' + h.hexdigest()


def prepare(args):
    source, crate = args.onnx_source.resolve(), args.crate.resolve()
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != ONNX_REVISION:
        raise ValueError('ONNX_REVISION_MISMATCH')
    for filename, expected in RUST.items():
        if digest(args.rust_archives / filename) != 'sha256:' + expected:
            raise ValueError('RUST_DIST_DIGEST:' + filename)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    try:
        result = materialize(args, source, crate, output)
    except Exception as error:
        (output / 'prepare-record.json').write_text(json.dumps({'status': 'FAIL', 'error': str(error)}) + '\n')
        raise
    (output / 'prepare-record.json').write_text(json.dumps({'status': 'SOURCE_READY'}) + '\n')
    return result


def materialize(args, source, crate, output):
    revision = ONNX_REVISION
    with (output / 'onnx.tar').open('wb') as stream:
        subprocess.run(['git', '-C', str(source), 'archive', '--format=tar', revision],
                       stdout=stream, check=True)
    for filename in RUST:
        shutil.copyfile(args.rust_archives / filename, output / filename)
    env = dict(os.environ, CARGO_HOME=str(args.cargo_home.resolve()))
    env['PATH'] = str(args.rust_prefix.resolve() / 'bin') + ':/usr/bin:/bin'
    before = {name: digest(crate / name) for name in ['Cargo.toml', 'Cargo.lock']}
    with (output / 'vendor.log').open('w') as log:
        subprocess.run([str(args.rust_prefix.resolve() / 'bin/cargo'), 'vendor', '--locked',
                        '--offline', '--manifest-path', str(crate / 'Cargo.toml'),
                        str(output / 'vendor')], env=env, stdout=log, stderr=log, check=True)
    if before != {name: digest(crate / name) for name in before}:
        raise ValueError('CARGO_SOURCE_CHANGED')
    with tarfile.open(output / 'vendor.tar', 'w') as archive:
        for path in sorted((output / 'vendor').rglob('*')):
            if path.is_symlink() or not (path.is_file() or path.is_dir()):
                raise ValueError('VENDOR_SPECIAL_FILE')
            if not path.is_file():
                continue
            info = archive.gettarinfo(str(path), str(path.relative_to(output / 'vendor')))
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ''
            with path.open('rb') as stream:
                archive.addfile(info, stream)
    files = {name: digest(output / name) for name in ['onnx.tar', 'vendor.tar', *RUST]}
    record = {'schema': 'ndnsf-native-build-inputs-v1', 'onnxRevision': revision,
              'rustVersion': '1.90.0', 'crate': before, 'files': files}
    (output / 'native-inputs.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['onnx-source', 'rust-archives', 'rust-prefix', 'cargo-home', 'crate', 'output']:
        parser.add_argument('--' + name, required=True, type=Path)
    print(json.dumps(prepare(parser.parse_args()), sort_keys=True))


if __name__ == '__main__':
    main()
