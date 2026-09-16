#!/usr/bin/env python3
"""Build a pinned stable base locally; each invocation owns a fresh output directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request

HERE = Path(__file__).resolve().parent
DEFAULT_LOCK = HERE.parents[2] / 'base-runtime.lock.json'


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def regular_path(value, exists=True):
    path = Path(os.path.abspath(value))
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError('BASE_PATH_SYMLINK:' + str(path))
    if not re.fullmatch(r'[A-Za-z0-9_./-]+', str(path)):
        raise ValueError('BASE_DEFINITION_UNSAFE_PATH:' + str(path))
    if exists and not path.is_file():
        raise ValueError('BASE_INPUT_NOT_REGULAR:' + str(path))
    return path


def build(args):
    parent = regular_path(args.base)
    lock_path = regular_path(args.lock)
    out = regular_path(args.output, exists=False)
    if out.exists():
        raise ValueError('BASE_OUTPUT_EXISTS')
    if not out.parent.is_dir():
        raise ValueError('BASE_OUTPUT_PARENT_MISSING')
    lock = json.loads(lock_path.read_text())
    parent_identity = (parent.stat().st_dev, parent.stat().st_ino)
    if parent.stat().st_size != lock['parentBytes'] or digest(parent) != lock['parentSha256']:
        raise ValueError('BASE_PARENT_IDENTITY')
    apptainer = Path(shutil.which(args.apptainer) or args.apptainer).resolve(strict=True)
    version = subprocess.check_output([str(apptainer), 'version'], text=True).strip()
    if args.expected_apptainer != '1.5.3' or version != args.expected_apptainer:
        raise ValueError('BASE_APPTAINER_VERSION:' + version)
    if shutil.disk_usage(out.parent).free < 16 * 1024**3:
        raise ValueError('BASE_BUILD_SPACE_BELOW_16_GIB')
    out.mkdir()
    inputs = out / 'inputs'
    (inputs / 'wheels').mkdir(parents=True)
    record = {'status': 'RUNNING', 'scope': 'BASE_SMOKE_ONLY',
              'parentSha256': lock['parentSha256'], 'apptainerVersion': version,
              'apptainerSha256': digest(apptainer)}
    record_path = out / 'build-record.json'
    try:
        shutil.copy2(lock_path, inputs / 'base-runtime.lock.json')
        for name in ('base-runtime.py', 'build-base-libraries.sh'):
            shutil.copy2(HERE / name, inputs / name)
        wheel = inputs / 'wheels' / lock['numpy']['filename']
        if args.wheel:
            shutil.copy2(regular_path(args.wheel), wheel)
        else:
            with urllib.request.urlopen(lock['numpy']['url'], timeout=60) as source, wheel.open('xb') as dest:
                shutil.copyfileobj(source, dest)
        if digest(wheel) != lock['numpy']['sha256']:
            raise ValueError('NUMPY_WHEEL_DIGEST')
        template = (HERE.parent / 'templates/library-runtime.def.in').read_text()
        definition = out / 'base.def'
        definition.write_text(template.replace('@BASE_SIF@', str(parent)).replace('@INPUT@', str(inputs)))
        record['inputs'] = {str(p.relative_to(out)): digest(p)
                            for p in inputs.rglob('*') if p.is_file()}
        record['definitionSha256'] = digest(definition)
        record_path.write_text(json.dumps(record, indent=2) + '\n')
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('APPTAINER', 'SINGULARITY', 'PYTHON', 'LD_'))}
        env['PATH'] = '/usr/bin:/bin:/usr/sbin:/sbin'
        for name in ('tmp', 'cache'):
            (out / name).mkdir()
        env['APPTAINER_TMPDIR'] = str(out / 'tmp')
        env['APPTAINER_CACHEDIR'] = str(out / 'cache')
        with (out / 'build.log').open('x') as log:
            # Reject a corrupt/extraction-incompatible parent before any build.
            subprocess.run([str(apptainer), 'exec', '--cleanenv', '--containall',
                            str(parent), '/bin/true'], env=env, stdout=log, stderr=log, check=True)
            subprocess.run([str(apptainer), 'build', '--fakeroot', '--mksquashfs-args',
                            '-processors 4', str(out / 'base.sif'), str(definition)],
                           env=env, stdout=log, stderr=log, check=True)
            smoke = subprocess.check_output([str(apptainer), 'exec', '--cleanenv', '--containall',
                                             str(out / 'base.sif'), '/opt/venv/bin/python',
                                             '/opt/ndn-base/manifest/base-runtime.py', 'verify'],
                                            env=env, stderr=log, text=True)
        (out / 'smoke.json').write_text(smoke)
        result = json.loads(smoke)
        if result.get('status') != 'PASS' or result.get('scope') != 'BASE_SMOKE_ONLY':
            raise ValueError('BASE_SMOKE_FAILED')
        if digest(parent) != lock['parentSha256']:
            raise ValueError('BASE_PARENT_CHANGED_DURING_BUILD')
        if (parent.stat().st_dev, parent.stat().st_ino) != parent_identity:
            raise ValueError('BASE_PARENT_REPLACED_DURING_BUILD')
        if digest(apptainer) != record['apptainerSha256'] or subprocess.check_output(
                [str(apptainer), 'version'], text=True).strip() != version:
            raise ValueError('BASE_APPTAINER_CHANGED_DURING_BUILD')
        for relative, expected in record['inputs'].items():
            if digest(out / relative) != expected:
                raise ValueError('BASE_INPUT_CHANGED_DURING_BUILD:' + relative)
        if digest(definition) != record['definitionSha256']:
            raise ValueError('BASE_DEFINITION_CHANGED_DURING_BUILD')
        record.update(status='PASS', sifSha256=digest(out / 'base.sif'),
                      sifBytes=(out / 'base.sif').stat().st_size, smoke=result)
    except Exception as error:
        record.update(status='FAIL', error=str(error))
        raise
    finally:
        record_path.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--lock', type=Path, default=DEFAULT_LOCK)
    parser.add_argument('--wheel', type=Path)
    parser.add_argument('--apptainer', default='apptainer')
    parser.add_argument('--expected-apptainer', required=True)
    build(parser.parse_args())
