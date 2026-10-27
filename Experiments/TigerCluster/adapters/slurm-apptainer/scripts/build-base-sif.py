#!/usr/bin/env python3
"""Build a pinned stable base locally; each invocation owns a fresh output directory."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
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
    if getattr(args, 'repair_application_metadata', False):
        if any(getattr(args, name, None) for name in (
                'repair_framework_headers', 'complete_nac_abe', 'complete_tokenizer_bridge',
                'dependency_bundle', 'wheel', 'nac_source_archive')):
            raise ValueError('BASE_COMPLETION_MODE_CONFLICT')
        return reseal_base(args, metadata_only=True)
    if getattr(args, 'complete_nac_abe', None):
        if getattr(args, 'repair_framework_headers', False) or getattr(args, 'complete_tokenizer_bridge', None):
            raise ValueError('BASE_COMPLETION_MODE_CONFLICT')
        return reseal_base(args, nac_bundle=args.complete_nac_abe)
    if getattr(args, 'complete_tokenizer_bridge', None):
        if getattr(args, 'repair_framework_headers', False):
            raise ValueError('BASE_COMPLETION_MODE_CONFLICT')
        return reseal_base(args, args.complete_tokenizer_bridge)
    if getattr(args, 'repair_framework_headers', False):
        return repair_framework_headers(args)
    if getattr(args, 'dependency_bundle', None):
        return build_dependency_sdk(args)
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


def repair_framework_headers(args):
    """Reseal one dependency base without rebuilding or replacing its SDK."""
    return reseal_base(args)


def tokenizer_inputs(bundle, inputs, parent_sha):
    """Extract only the four sealed bridge sources, never the repository tree."""
    spec = importlib.util.spec_from_file_location('tokenizer_handoff',
                                                 HERE / 'prepare-development-handoff.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    bundle = Path(bundle).absolute()
    helper.verify(bundle)
    lock = helper.load_lock(bundle / 'dependency-lock.json')
    if lock['baseSif']['sha256'] != 'sha256:' + parent_sha:
        raise ValueError('TOKENIZER_PARENT_IDENTITY')
    seal = json.loads((bundle / 'source/source-seal.json').read_text())
    rows = {row['path']: row for row in seal['files']}
    prefix = helper.TOKENIZER_PREFIX
    needed = set(helper.TOKENIZER_FILES)
    destination = inputs / 'tokenizer'
    found = set()
    with tarfile.open(bundle / 'source/workspace.tar') as archive:
        for member in archive:
            if not member.name.startswith(prefix):
                continue
            name = member.name[len(prefix):]
            if name not in needed:
                continue
            if name in found or not member.isfile() or member.size > 1024 * 1024:
                raise ValueError('TOKENIZER_SOURCE_MEMBER:' + name)
            data = archive.extractfile(member).read()
            row = rows.get(member.name, {})
            if (row.get('bytes') != len(data) or row.get('sha256') !=
                    'sha256:' + hashlib.sha256(data).hexdigest()):
                raise ValueError('TOKENIZER_SOURCE_IDENTITY:' + name)
            target = destination / 'crate' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            found.add(name)
    if found != needed:
        raise ValueError('TOKENIZER_SOURCE_MISSING')
    shutil.copy2(bundle / 'native/native-inputs.json', destination / 'native-inputs.json')
    root = HERE.parents[4]
    probes = destination / 'probes'
    probes.mkdir()
    fixtures = root / 'tests/fixtures/spec182/dependency-probes/tokenizer'
    for name in ('probe.cpp', 'vectors.json', 'stable-vectors.json'):
        shutil.copy2(regular_path(fixtures / name), probes / name)
    shutil.copy2(regular_path(root / 'tests/installed-api/tokenizer-stable-probe.cpp'),
                 probes / 'stable-probe.cpp')
    files = {str(p.relative_to(destination)): digest(p)
             for p in sorted(destination.rglob('*')) if p.is_file()}
    (destination / 'inputs.json').write_text(json.dumps({
        'files': files, 'sourceSealDigest': seal['sealDigest'],
        'baseSdkSha256': lock['baseSdk']['sha256']}, indent=2) + '\n')


def nac_inputs(bundle, inputs, parent_sha, source_archive=None):
    """Seal only the existing host NAC lock/patch helper and its exact raw archive."""
    spec = importlib.util.spec_from_file_location('nac_handoff', HERE / 'prepare-development-handoff.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    bundle = Path(bundle).absolute()
    helper.verify(bundle)
    lock = helper.load_lock(bundle / 'dependency-lock.json')
    if lock['baseSif']['sha256'] != 'sha256:' + parent_sha:
        raise ValueError('NAC_PARENT_IDENTITY')
    seal = json.loads((bundle / 'source/source-seal.json').read_text())
    rows = {row['path']: row for row in seal['files']}
    needed = {'packaging/host-dependencies.lock.json', 'scripts/stack_sources.py',
              'patches/nac-abe/series.json', 'patches/openabe/series.json'}
    needed.update(name for name in rows
                  if name.startswith(('patches/nac-abe/', 'patches/openabe/')) and
                  name.endswith('.patch'))
    destination = inputs / 'nac'
    found = set()
    with tarfile.open(bundle / 'source/workspace.tar') as archive:
        for member in archive:
            if member.name not in needed:
                continue
            if member.name in found or not member.isfile() or member.size > 1024 * 1024:
                raise ValueError('NAC_SOURCE_MEMBER:' + member.name)
            data = archive.extractfile(member).read()
            row = rows.get(member.name, {})
            if (row.get('bytes') != len(data) or row.get('sha256') !=
                    'sha256:' + hashlib.sha256(data).hexdigest()):
                raise ValueError('NAC_SOURCE_IDENTITY:' + member.name)
            target = destination / 'project' / member.name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            found.add(member.name)
    if found != needed:
        raise ValueError('NAC_SOURCE_MISSING')
    raw = regular_path(source_archive if source_archive else bundle / 'source/nacAbe.tar')
    if 'baseSdk' in lock:
        parent = json.loads((bundle / 'base-sdk.json').read_text())['inputs']
        expected = parent['files']['source/nacAbe.tar']
        repository = parent['repositories']['nacAbe']
    else:
        expected = json.loads((bundle / 'handoff.json').read_text())['files']['source/nacAbe.tar']
        if not expected.startswith('sha256:'):
            raise ValueError('NAC_UPSTREAM_ARCHIVE_IDENTITY')
        expected = expected[7:]
        repository = lock['repositories']['nacAbe']
    if digest(raw) != expected:
        raise ValueError('NAC_UPSTREAM_ARCHIVE')
    shutil.copy2(raw, destination / 'upstream.tar')
    files = {str(path.relative_to(destination)): digest(path)
             for path in destination.rglob('*') if path.is_file()}
    (destination / 'inputs.json').write_text(json.dumps({
        'files': files, 'sourceSealDigest': seal['sealDigest'],
        'baseSdkSha256': lock.get('baseSdk', {}).get('sha256')}, indent=2) + '\n')
    spec = importlib.util.spec_from_file_location('nac_sdk_contract', HERE / 'dependency-sdk.py')
    sdk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sdk)
    sdk.nac_contract(destination, repository, expected)


def reseal_base(args, tokenizer_bundle=None, nac_bundle=None, metadata_only=False):
    """Share parent identity/resource handling across explicit base maintenance actions."""
    if args.dependency_bundle or args.wheel:
        raise ValueError('BASE_HEADER_REPAIR_INPUT_CONFLICT')
    if os.geteuid() != 0:
        raise ValueError('BASE_REPAIR_ROOT_NAMESPACE_REQUIRED')
    parent = regular_path(args.base)
    lock_path = regular_path(args.lock)
    lock = json.loads(lock_path.read_text())
    if (parent.stat().st_size != lock['parentBytes'] or
            digest(parent) != lock['parentSha256']):
        raise ValueError('BASE_PARENT_IDENTITY')
    identity = (parent.stat().st_dev, parent.stat().st_ino)
    work_value = os.environ.get('NDNSF_DI_WORKSPACE')
    if not work_value:
        raise ValueError('BASE_REPAIR_DISK_WORKSPACE_REQUIRED')
    work = regular_path(work_value, exists=False)
    if not work.is_dir() or shutil.disk_usage(work).free < 16 * 1024**3:
        raise ValueError('BASE_BUILD_SPACE_BELOW_16_GIB')
    filesystem = subprocess.check_output(
        ['findmnt', '-T', str(work), '-n', '-o', 'FSTYPE'], text=True).strip()
    if filesystem not in ('xfs', 'ext4', 'btrfs', 'zfs', 'nfs', 'nfs4'):
        raise ValueError('BASE_REPAIR_REQUIRES_DISK:' + filesystem)
    out = regular_path(args.output, exists=False)
    if out.exists() or not out.parent.is_dir():
        raise ValueError('BASE_OUTPUT_EXISTS_OR_PARENT_MISSING')
    apptainer = Path(shutil.which(args.apptainer) or args.apptainer).resolve(strict=True)
    version = subprocess.check_output([str(apptainer), 'version'], text=True).strip()
    match = re.search(r'\b([0-9]+\.[0-9]+\.[0-9]+)', version)
    if args.expected_apptainer != '1.5.3' or not match or match.group(1) != '1.5.3':
        raise ValueError('BASE_APPTAINER_VERSION:' + version)
    out.mkdir()
    inputs = out / 'inputs'
    inputs.mkdir()
    for source in (lock_path, HERE / 'base-runtime.py'):
        shutil.copy2(source, inputs / ('parent.lock.json' if source == lock_path else source.name))
    mode = 'nac-completion' if nac_bundle else ('tokenizer-completion' if tokenizer_bundle else 'header-repair')
    if metadata_only:
        mode = 'application-metadata-repair'
    if tokenizer_bundle:
        tokenizer_inputs(tokenizer_bundle, inputs, lock['parentSha256'])
        shutil.copy2(HERE / 'dependency-sdk.py', inputs / 'dependency-sdk.py')
    if nac_bundle:
        nac_inputs(nac_bundle, inputs, lock['parentSha256'], getattr(args, 'nac_source_archive', None))
        shutil.copy2(HERE / 'dependency-sdk.py', inputs / 'dependency-sdk.py')
    template_name = 'header-repair' if metadata_only else mode
    template = HERE.parent / 'templates' / (template_name + '.def.in')
    definition = out / 'base.def'
    definition.write_text(template.read_text().replace('@BASE_SIF@', str(parent))
                          .replace('@INPUT@', str(inputs))
                          .replace('@REPAIR_ACTION@', 'repair-application-metadata' if metadata_only
                                   else 'repair-framework-headers'))
    scope = ('BASE_NAC_PATCH_COMPLETION' if nac_bundle else
             ('BASE_TOKENIZER_BRIDGE_COMPLETION' if tokenizer_bundle else 'BASE_FRAMEWORK_HEADER_REPAIR'))
    if metadata_only:
        scope = 'BASE_APPLICATION_METADATA_REPAIR'
    record = {'status': 'RUNNING', 'scope': scope,
              'parentSha256': lock['parentSha256'], 'parentBytes': lock['parentBytes'],
              'apptainerVersion': version, 'apptainerSha256': digest(apptainer),
              'definitionSha256': digest(definition),
              'inputs': {str(p.relative_to(out)): digest(p)
                         for p in inputs.rglob('*') if p.is_file()},
              'mksquashfsArgs': '-processors 2 -mem 2G'}
    record['workspaceFilesystem'] = filesystem
    record_path = out / 'build-record.json'
    record_path.write_text(json.dumps(record, indent=2) + '\n')
    try:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('APPTAINER', 'SINGULARITY', 'PYTHON', 'LD_'))}
        env['PATH'] = '/usr/bin:/bin:/usr/sbin:/sbin'
        for name in ('tmp', 'cache'):
            (work / (mode + '-' + name)).mkdir()
        env.update(APPTAINER_TMPDIR=str(work / (mode + '-tmp')),
                   APPTAINER_CACHEDIR=str(work / (mode + '-cache')))
        image = out / 'base.sif'
        with (out / 'build.log').open('x') as log:
            # The caller already owns a root-mapped namespace. Requesting
            # fakeroot again would require unsupported subordinate-ID helpers.
            subprocess.run([str(apptainer), 'build', '--no-cleanup',
                            '--mksquashfs-args', record['mksquashfsArgs'],
                            str(image), str(definition)], env=env,
                           stdout=log, stderr=log, check=True)
            command = [str(apptainer), 'exec', '--cleanenv', '--containall',
                       '--no-home', '--no-mount', 'cwd,hostfs,bind-paths', str(image)]
            for python, verifier in (('/opt/venv/bin/python', 'base-runtime.py'),
                                     ('/usr/bin/python3', 'dependency-sdk.py')):
                subprocess.run(command + [python, '/opt/ndn-base/manifest/' + verifier, 'verify'],
                               env=env, stdout=log, stderr=log, check=True)
            cleanup = subprocess.check_output(command + [
                '/bin/cat', '/opt/ndn-base/manifest/' + (
                    'nac-abe-completion.json' if nac_bundle else
                    ('tokenizer-bridge-completion.json' if tokenizer_bundle
                     else ('application-metadata-repair.json' if metadata_only
                           else 'framework-header-repair.json')))],
                env=env, stderr=log, text=True)
            if tokenizer_bundle or nac_bundle:
                sdk = subprocess.check_output(command + [
                    '/bin/cat', '/opt/ndn-base/manifest/dependency-sdk.json'],
                    env=env, stderr=log, text=True)
                (out / 'dependency-sdk.json').write_text(sdk)
                record['dependencySdkSha256'] = digest(out / 'dependency-sdk.json')
        if (digest(parent) != lock['parentSha256'] or
                (parent.stat().st_dev, parent.stat().st_ino) != identity or
                digest(apptainer) != record['apptainerSha256'] or
                subprocess.check_output([str(apptainer), 'version'], text=True).strip() != version):
            raise ValueError('BASE_REPAIR_INPUT_CHANGED')
        for relative, expected in record['inputs'].items():
            if digest(out / relative) != expected:
                raise ValueError('BASE_INPUT_CHANGED:' + relative)
        if digest(definition) != record['definitionSha256']:
            raise ValueError('BASE_DEFINITION_CHANGED')
        record.update(status='PASS', cleanup=json.loads(cleanup),
                      sifSha256=digest(image), sifBytes=image.stat().st_size)
    except Exception as error:
        record.update(status='FAIL', error=str(error))
        raise
    finally:
        record_path.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record))


def build_dependency_sdk(args):
    """Extend a verified base; the external source bundle never becomes NDNSF code in base."""
    parent = regular_path(args.base)
    bundle = Path(args.dependency_bundle).absolute()
    regular_path(bundle / 'handoff.json')
    out = regular_path(args.output, exists=False)
    if out.exists() or not out.parent.is_dir():
        raise ValueError('BASE_OUTPUT_EXISTS_OR_PARENT_MISSING')
    spec = importlib.util.spec_from_file_location('sdk_handoff', HERE / 'prepare-development-handoff.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    helper.verify(bundle)
    lock = helper.load_lock(bundle / 'dependency-lock.json')
    parent_digest = digest(parent)
    if 'sha256:' + parent_digest != lock['baseSif']['sha256']:
        raise ValueError('SDK_PARENT_IDENTITY')
    apptainer = Path(shutil.which(args.apptainer) or args.apptainer).resolve(strict=True)
    version = subprocess.check_output([str(apptainer), 'version'], text=True).strip()
    if version != '1.5.3' or args.expected_apptainer != version:
        raise ValueError('BASE_APPTAINER_VERSION')
    if shutil.disk_usage(out.parent).free < 16 * 1024**3:
        raise ValueError('BASE_BUILD_SPACE_BELOW_16_GIB')
    out.mkdir()
    inputs = out / 'inputs'
    sdk = inputs / 'sdk'
    (sdk / 'source').mkdir(parents=True)
    (sdk / 'native').mkdir()
    (sdk / 'wheels').mkdir()
    record = {'status': 'RUNNING', 'scope': 'BASE_DEPENDENCY_SDK',
              'parentSha256': parent_digest, 'apptainerVersion': version,
              'apptainerSha256': digest(apptainer)}
    try:
        names = ['source/nacAbe.tar', 'source/ndn-svs.tar', 'source/ndnSd.tar']
        names += ['native/' + name for name in helper.NATIVE_FILES]
        names += ['wheels/' + row['filename'] for row in lock['wheels']]
        for name in names:
            shutil.copyfile(bundle / name, sdk / name)
        source_hashes = json.loads((bundle / 'handoff.json').read_text())['files']
        for name in names:
            if 'sha256:' + digest(sdk / name) != source_hashes[name]:
                raise ValueError('SDK_COPY_CHANGED:' + name)
        nac_inputs(bundle, sdk, parent_digest)
        names += [str(path.relative_to(sdk)) for path in (sdk / 'nac').rglob('*') if path.is_file()]
        (sdk / 'inputs.json').write_text(json.dumps({
            'files': {name: digest(sdk / name) for name in names},
            'repositories': {k: v for k, v in lock['repositories'].items() if k != 'ndnsf'},
            'nativeBuild': lock['nativeBuild']}, indent=2) + '\n')
        for name in ['dependency-sdk.py', 'base-runtime.py']:
            shutil.copy2(HERE / name, inputs / name)
        template = (HERE.parent / 'templates/dependency-sdk.def.in').read_text()
        definition = out / 'base.def'
        definition.write_text(template.replace('@BASE_SIF@', str(parent)).replace('@INPUT@', str(inputs)))
        record['inputs'] = {str(p.relative_to(out)): digest(p) for p in inputs.rglob('*') if p.is_file()}
        record['definitionSha256'] = digest(definition)
        (out / 'build-record.json').write_text(json.dumps(record, indent=2) + '\n')
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('APPTAINER', 'SINGULARITY', 'PYTHON', 'LD_'))}
        env['PATH'] = '/usr/bin:/bin:/usr/sbin:/sbin'
        for name in ['tmp', 'cache']:
            (out / name).mkdir()
        env.update(APPTAINER_TMPDIR=str(out / 'tmp'), APPTAINER_CACHEDIR=str(out / 'cache'))
        with (out / 'build.log').open('x') as log:
            subprocess.run([str(apptainer), 'build', '--fakeroot', '--no-cleanup',
                '--mksquashfs-args', '-processors 4', str(out / 'base.sif'), str(definition)],
                env=env, stdout=log, stderr=log, check=True)
            result = subprocess.check_output([str(apptainer), 'exec', '--cleanenv', '--containall',
                str(out / 'base.sif'), '/usr/bin/python3',
                '/opt/ndn-base/manifest/dependency-sdk.py', 'verify'], env=env, stderr=log, text=True)
        (out / 'smoke.json').write_text(result)
        smoke = json.loads(result)
        if smoke.get('status') != 'PASS' or smoke.get('scope') != 'BASE_DEPENDENCY_SDK':
            raise ValueError('SDK_SMOKE_FAILED')
        if digest(parent) != parent_digest or digest(apptainer) != record['apptainerSha256']:
            raise ValueError('SDK_BUILD_INPUT_CHANGED')
        for relative, expected in record['inputs'].items():
            if digest(out / relative) != expected:
                raise ValueError('SDK_INPUT_CHANGED:' + relative)
        if digest(definition) != record['definitionSha256']:
            raise ValueError('SDK_DEFINITION_CHANGED')
        record.update(status='PASS', sifSha256=digest(out / 'base.sif'),
                      sifBytes=(out / 'base.sif').stat().st_size, smoke=smoke)
    except Exception as error:
        record.update(status='FAIL', error=str(error))
        raise
    finally:
        (out / 'build-record.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--lock', type=Path, default=DEFAULT_LOCK)
    parser.add_argument('--wheel', type=Path)
    parser.add_argument('--repair-framework-headers', action='store_true',
                        help='Reseal the pinned parent after removing only framework headers')
    parser.add_argument('--repair-application-metadata', action='store_true',
                        help='Reseal the pinned parent after removing only NDNSF Python metadata')
    parser.add_argument('--complete-tokenizer-bridge', type=Path,
                        help='Complete only the missing tokenizer SDK from a sealed runtime handoff')
    parser.add_argument('--complete-nac-abe', type=Path,
                        help='Complete only the existing locked NAC patch stack from a sealed handoff')
    parser.add_argument('--nac-source-archive', type=Path,
                        help='Exact upstream NAC archive bound by the existing SDK manifest')
    parser.add_argument('--dependency-bundle', type=Path,
                        help='Extend the locked existing base with all sealed external dependencies')
    parser.add_argument('--apptainer', default='apptainer')
    parser.add_argument('--expected-apptainer', required=True)
    build(parser.parse_args())
