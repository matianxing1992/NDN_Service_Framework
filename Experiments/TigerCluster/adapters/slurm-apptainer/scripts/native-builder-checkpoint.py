#!/usr/bin/env python3
"""Verify a retained container install and derive an execution-free builder stage.

This resumes packing only. The original normal definition, original builder
manifest and completion evidence remain mandatory; no new builder proof is made.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


class CheckpointError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise CheckpointError('BUILDER_CHECKPOINT_' + message)


def digest(path):
    checksum = hashlib.sha256()
    before = path.stat()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            checksum.update(chunk)
    after = path.stat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns,
             before.st_ctime_ns) == (after.st_dev, after.st_ino, after.st_size,
                                    after.st_mtime_ns, after.st_ctime_ns), 'FILE_CHANGED')
    return 'sha256:' + checksum.hexdigest()


def json_file(path):
    require(path.is_file() and not path.is_symlink(), 'RECEIPT_MISSING:' + str(path))
    return json.loads(path.read_text())


def private_directory(path):
    require(path.is_absolute() and path.resolve() == path, 'DIRECTORY_ALIAS')
    require('\n' not in str(path) and '\r' not in str(path), 'DIRECTORY_NEWLINE')
    info = path.stat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
            and not info.st_mode & 0o077, 'DIRECTORY_NOT_PRIVATE')
    filesystem = subprocess.check_output(
        ['findmnt', '-n', '-o', 'FSTYPE', '-T', str(path)], text=True).strip()
    require(filesystem and filesystem not in ('rootfs', 'ramfs', 'tmpfs'), 'DISK_REQUIRED')


def snapshot(root):
    """Bound the complete retained image, including modes and relative links."""
    require(root.resolve() == root, 'TREE_ALIAS')
    rows = {}
    mounts = []
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        encoded = line.split()[4]
        mounts.append(Path(re.sub(r'\\([0-7]{3})', lambda m: chr(int(m[1], 8)), encoded)))
    require(not any(point == root or root in point.parents for point in mounts), 'TREE_MOUNT')
    for path in [root, *sorted(root.rglob('*'))]:
        before = path.lstat()
        require(before.st_uid == os.getuid() and before.st_dev == root.stat().st_dev,
                'TREE_OWNER_OR_DEVICE')
        row = {'mode': stat.S_IMODE(before.st_mode)}
        if stat.S_ISLNK(before.st_mode):
            target = os.readlink(path)
            require(not os.path.isabs(target) and root in path.resolve().parents
                    and path.exists(), 'TREE_LINK_ESCAPE_OR_DANGLING')
            row.update(type='symlink', target=target)
        elif stat.S_ISDIR(before.st_mode):
            row.update(type='directory')
        else:
            require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1, 'TREE_SPECIAL_FILE')
            row.update(type='file', bytes=before.st_size, sha256=digest(path)[7:])
        after = path.lstat()
        require((before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                == (after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                'TREE_CHANGED')
        rows[str(path.relative_to(root))] = row
    return rows


def owned_file(root, relative):
    relative = Path(relative)
    require(not relative.is_absolute() and '..' not in relative.parts, 'RECEIPT_PATH_ESCAPE')
    path = root / relative
    require(root in path.resolve().parents, 'RECEIPT_LINK_ESCAPE')
    return path


def normal_boundary(definition):
    path = Path(__file__).resolve().parents[3] / 'lib/spec170_sif_build_boundary.py'
    spec = importlib.util.spec_from_file_location('normal_sif_boundary', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.validate_definition(definition)


def inspect(root, retention_path, definition, source_seal, sdk_sha256):
    """Compose original normal-boundary proof with the retained original receipts."""
    private_directory(root)
    require(retention_path.parent == root, 'RETENTION_OUTSIDE_ROOT')
    receipt = json_file(retention_path)
    require(receipt['status'] == 'RETAINED_VERIFIED_COPY_PENDING_ADMISSION'
            and receipt['scope'] == 'RETAINED_CONTAINER_STAGE_NOT_YET_CHECKPOINT_ADMISSION',
            'RETENTION_NOT_VERIFIED')
    require(str(receipt['priorBuildJob']).isdigit() and str(receipt['job']).isdigit(),
            'BUILD_JOB_ID_MISSING')
    require(receipt['destination'] == str(root / 'rootfs'), 'RETENTION_DESTINATION')
    boundary = normal_boundary(definition)
    require(boundary.get('runtimeLayout') == 'installed-v1', 'INSTALLED_LAYOUT_REQUIRED')
    require(digest(definition)[7:] == receipt['originalDefinitionSha256']
            and (root / 'original-runtime.def').read_bytes() == definition.read_bytes(),
            'ORIGINAL_DEFINITION_CHANGED')
    tree_path = root / 'stage-tree.json'
    require(digest(tree_path)[7:] == receipt['stageTreeSha256'], 'RETENTION_TREE_RECEIPT_CHANGED')
    image = root / 'rootfs'
    image_rows = snapshot(image)
    stage = image / 'opt/ndnsf-stage'
    rows = snapshot(stage)
    require(rows == json_file(tree_path), 'RETAINED_TREE_CHANGED')
    allowed = {'.', 'opt'} | {'opt/ndnsf-stage' + ('' if name == '.' else '/' + name)
                              for name in rows}
    require(set(image_rows) == allowed, 'UNDECLARED_IMAGE_INPUT')
    require(len(rows) == receipt['fileCount'] and sum(row.get('bytes', 0) for row in rows.values())
            == receipt['regularFileBytes'], 'RETENTION_COUNTS_CHANGED')
    manifest_path = stage / 'manifest/container-native-build.json'
    require(digest(manifest_path)[7:] == receipt['nativeManifestSha256'], 'NATIVE_MANIFEST_CHANGED')
    native = json_file(manifest_path)
    require(native['schemaVersion'] == 'spec170-container-native-build-v1'
            and native['status'] == 'PASS'
            and native['buildBoundary'] == 'container-runtime-in-sif'
            and native['runtimeLayout'] == 'installed-v1', 'NATIVE_PROVENANCE_MISSING')
    for field, name in (('sourceSealSha256', 'source-seal.json'),
                        ('dependencyLockSha256', 'dependency-lock.json'),
                        ('configureClosureSha256', 'container-configure-closure.json'),
                        ('wafInstallSha256', 'waf-install.json')):
        require(digest(stage / 'manifest' / name) == native[field], 'NATIVE_RECEIPT_CHANGED:' + name)
    require(digest(source_seal) == native['sourceSealSha256'] == receipt['sourceSeal'],
            'SOURCE_SEAL_MISMATCH')
    base = Path(boundary['baseImage'])
    require(base.is_absolute() and base.is_file() and digest(base) == native['baseSifSha256']
            == receipt['baseSif'], 'BASE_MISMATCH')
    require(re.fullmatch(r'sha256:[0-9a-f]{64}', sdk_sha256)
            and native['baseSdkSha256'] == receipt['baseSdk'] == sdk_sha256, 'SDK_MISMATCH')
    waf = json_file(stage / 'manifest/waf-install.json')
    require(waf and native['artifacts'] and native['repositoryPythonFiles'], 'EMPTY_NATIVE_RECEIPTS')
    for relative, expected in waf.items():
        path = owned_file(stage, relative)
        if 'symlink' in expected:
            require(path.is_symlink() and os.readlink(path) == expected['symlink']
                    and path.is_file(), 'WAF_LINK_CHANGED')
        else:
            require(path.is_file() and not path.is_symlink()
                    and digest(path) == 'sha256:' + expected['sha256'], 'WAF_FILE_CHANGED')
    for artifact in native['artifacts']:
        declared = Path(artifact['builderPath'])
        require(Path('/opt/ndnsf-stage') in declared.parents, 'NATIVE_PATH_ESCAPE')
        path = owned_file(stage, declared.relative_to('/opt/ndnsf-stage'))
        require(path.stat().st_size == artifact['bytes'] and digest(path) == artifact['sha256'],
                'NATIVE_ARTIFACT_CHANGED')
    for relative, expected in native['repositoryPythonFiles'].items():
        path = owned_file(stage / 'python', relative)
        require(path.is_file() and not path.is_symlink()
                and digest(path) == expected['sha256']
                and stat.S_IMODE(path.stat().st_mode) == expected['mode'], 'PYTHON_PAYLOAD_CHANGED')
    log_path = root / 'original-build.log'
    require(digest(log_path)[7:] == receipt['originalBuildLogSha256'], 'BUILD_LOG_CHANGED')
    log = log_path.read_text(errors='strict')
    completed = '+ rm -rf /src/ndnsf /src/ndn-svs /src/nac-abe /src/ndn-sd /build-input /opt/ndnsf-build-work'
    require(completed in log and 'verify-native.py builder' in log
            and 'INFO:    Extracting local image...' in log.split(completed, 1)[1],
            'BUILDER_COMPLETION_MISSING')
    # The normal validator has already checked exactly two localimage stages.
    text = definition.read_text()
    starts = list(re.finditer(r'(?im)^Bootstrap:', text))
    require(len(starts) == 2, 'DEFINITION_STAGE_SPLIT')
    final = text[starts[1].start():]
    derived = f'Bootstrap: localimage\nFrom: {image}\nStage: builder\n\n' + final
    proof = {'originalBoundary': boundary, 'retention': str(retention_path),
             'retentionSha256': digest(retention_path), 'priorBuildJob': receipt['priorBuildJob'],
             'tree': image_rows, 'nativeManifestSha256': digest(manifest_path),
             'sourceSealSha256': digest(source_seal), 'baseSdkSha256': sdk_sha256,
             'baseSifSha256': native['baseSifSha256'], 'checkpointRoot': str(root),
             'originalDefinition': str(definition), 'originalDefinitionSha256': digest(definition)}
    require(snapshot(image) == image_rows, 'TREE_CHANGED_DURING_ADMISSION')
    return proof, derived


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('prepare', 'verify'))
    parser.add_argument('--checkpoint-root', type=Path, required=True)
    parser.add_argument('--retention-record', type=Path, required=True)
    parser.add_argument('--definition', type=Path, required=True)
    parser.add_argument('--source-seal', type=Path, required=True)
    parser.add_argument('--base-sdk-sha256', required=True)
    parser.add_argument('--derived-definition', type=Path, required=True)
    parser.add_argument('--record', type=Path, required=True)
    parser.add_argument('--expected-record-sha256')
    args = parser.parse_args()
    proof, derived = inspect(args.checkpoint_root, args.retention_record, args.definition,
                             args.source_seal, args.base_sdk_sha256)
    record = {'schemaVersion': 'ndnsf-native-builder-checkpoint-v1', 'proof': proof,
              'nativeCompilation': 'reused-container-builder',
              'derivedDefinition': str(args.derived_definition),
              'derivedDefinitionSha256': 'sha256:' + hashlib.sha256(derived.encode()).hexdigest()}
    if args.operation == 'prepare':
        require(not args.expected_record_sha256, 'UNEXPECTED_VERIFY_DIGEST')
        require(not args.record.exists() and not args.derived_definition.exists(), 'OUTPUT_EXISTS')
        with args.derived_definition.open('x') as stream:
            stream.write(derived)
        with args.record.open('x') as stream:
            json.dump(record, stream, indent=2, sort_keys=True)
            stream.write('\n')
    else:
        require(args.expected_record_sha256 and digest(args.record) == args.expected_record_sha256,
                'CHECKPOINT_RECORD_CHANGED')
        require(json_file(args.record) == record and args.derived_definition.read_text() == derived,
                'CHECKPOINT_OR_DEFINITION_CHANGED')
    boundary = dict(proof['originalBoundary'], definition=str(args.derived_definition),
                    definitionSha256=record['derivedDefinitionSha256'],
                    nativeCompilation='reused-container-builder',
                    builderCheckpoint={'path': str(args.record), 'sha256': digest(args.record)},
                    originalDefinition={'path': str(args.definition),
                                        'sha256': digest(args.definition)})
    print(json.dumps(boundary, sort_keys=True))


if __name__ == '__main__':
    try:
        main()
    except (CheckpointError, OSError, KeyError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
