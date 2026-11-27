#!/usr/bin/env python3
"""Preserve native WAN profiles without delaying the allocation's final audit.

This host observer copies closed files. The native backend validates profiles
when producing CUDA evidence. Reopen the preserved JSON with --verify-only after
execution; neither observer mode qualifies inference or changes cleanup checks.
Optional gzip storage retains the raw digest and byte count. Both raw and stored
byte budgets apply; verification bounds decompression before decoding the JSON.
"""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import time


def read_json(path):
    if path.stat().st_size > 16 << 20:
        raise ValueError(f'observer control file exceeds its bound: {path}')
    return json.loads(path.read_bytes())


def identity(info):
    # Reading a closed profile can update atime without changing its contents.
    return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def locations(root, output_root):
    root = Path(root).resolve(strict=True)
    config = read_json(root / 'd/deployment-real.json')
    host = socket.gethostname().split('.')[0]
    node, = [n for n in config['nodes'] if n['host'].split('.')[0] == host]
    provider, = [a for a in config['actors']
                 if a['kind'] == 'provider' and a['node'] == node['id']]
    clip = Path(config['requester']['wan_generation']['output_directory'])
    output = ((Path(output_root) if output_root is not None else clip.parents[1]) /
              'profiles' / provider['id'])
    return root, config, node, provider, output


def capture(root, timeout=9000, max_bytes=2 << 30, max_profiles=8192, output_root=None,
            compress=False, max_stored_bytes=None):
    root, config, node, provider, output = locations(root, output_root)
    deployment = config['deployment_id']
    host = socket.gethostname().split('.')[0]
    actor = provider['id']
    source_root = Path(provider['parameters']['artifact-cache-dir'])
    clip = Path(config['requester']['wan_generation']['output_directory'])
    output.mkdir(parents=True, mode=0o700)
    operation = read_json(root / 'd/state' / deployment / 'state.json')
    job = os.environ['SLURM_JOB_ID']
    if str(operation['job_reference']['job_id']) != job:
        raise ValueError('profile observer job does not match the operation')
    evidence = root / 'd/evidence' / deployment / operation['boot_id']
    requester, = [a for a in config['actors'] if a['kind'] == 'requester']
    requester_state = (evidence / 'actors' / requester['id'] / 'runtime' /
                       deployment / 'state.json')
    gpu_receipt = evidence / 'nodes' / node['id'] / 'gpu-allocation.json'
    started = time.monotonic()
    seen = set()
    total = 0
    stored = 0
    storage_limit = max_bytes if max_stored_bytes is None else max_stored_bytes
    gpu = None
    last_sample = -float('inf')
    with (root / (actor + '-profiles.jsonl')).open('x') as index, \
            (root / (actor + '-gpu-memory.jsonl')).open('x') as memory_log:
        while time.monotonic() - started < timeout:
            cancelled = (root / 'capture-drain').is_file()
            done = cancelled or (clip / 'manifest.json').is_file()
            # Every observer reads the Requester's terminal state directly.
            # A peer busy copying its last file must not delay our retirement.
            if not done and requester_state.is_file():
                done = read_json(requester_state).get('status') == 'STOPPED'
            if gpu is None and gpu_receipt.is_file():
                observed = read_json(gpu_receipt)
                if (str(observed['job_id']) != job or
                        observed['host'].split('.')[0] != host):
                    raise ValueError('profile observer GPU receipt binding differs')
                device, = observed['devices']
                gpu = device['uuid']
                if not gpu.startswith('GPU-'):
                    raise ValueError('profile observer requires a GPU UUID')
            now = time.monotonic()
            if not done and gpu is not None and now - last_sample >= 2:
                sample = dict(job_id=job, host=host, gpu_uuid=gpu, time_ns=time.time_ns())
                try:
                    text = subprocess.check_output(
                        ['nvidia-smi', '--id=' + gpu,
                         '--query-gpu=memory.total,memory.free',
                         '--format=csv,noheader,nounits'], text=True, timeout=1).strip()
                    sample['total_mib'], sample['free_mib'] = map(int, text.split(','))
                except (OSError, ValueError, subprocess.SubprocessError) as error:
                    sample['error'] = str(error)
                print(json.dumps(sample), file=memory_log, flush=True)
                last_sample = now
            pending = False
            for source in source_root.glob('ort-profile-*.json'):
                if source in seen:
                    continue
                try:
                    before = source.lstat()
                    if not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid():
                        raise ValueError(f'profile is not an owned regular file: {source}')
                    if before.st_size > (256 << 20 if compress else 128 << 20):
                        raise ValueError(f'profile exceeds its file bound: {source}')
                    if before.st_size == 0:
                        pending = True
                        continue
                    with source.open('rb') as stream:
                        stream.seek(max(0, before.st_size - 128))
                        closed = stream.read().rstrip().endswith(b']')
                    if not closed:
                        pending = True
                        continue
                    if not compress:
                        payload = source.read_bytes()
                        if (identity(source.lstat()) != identity(before) or
                                len(payload) != before.st_size):
                            pending = True
                            continue
                except FileNotFoundError:
                    continue
                # Do not decode large ORT JSON arrays on the live capture path.
                # Preserve exact bytes and decode them in the post-run verifier.
                if total + before.st_size > max_bytes or len(seen) >= max_profiles:
                    raise ValueError('profile observer capacity exceeded')
                destination = output / (source.name + ('.gz' if compress else ''))
                created = False
                try:
                    with destination.open('xb') as target:
                        created = True
                        if compress:
                            checksum = hashlib.sha256()
                            copied = 0
                            with source.open('rb') as stream, gzip.GzipFile(
                                    filename='', fileobj=target, mode='wb', compresslevel=1,
                                    mtime=0) as archive:
                                while True:
                                    chunk = stream.read(min(1 << 20, before.st_size - copied + 1))
                                    if not chunk:
                                        break
                                    copied += len(chunk)
                                    if copied > before.st_size:
                                        raise ValueError('profile grew while being compressed')
                                    checksum.update(chunk)
                                    archive.write(chunk)
                                    if stored + target.tell() > storage_limit:
                                        raise ValueError('profile archive storage capacity exceeded')
                            digest = checksum.hexdigest()
                        else:
                            copied = target.write(payload)
                            digest = hashlib.sha256(payload).hexdigest()
                        stored_bytes = target.tell()
                        if stored + stored_bytes > storage_limit:
                            raise ValueError('profile archive storage capacity exceeded')
                    if (copied != before.st_size or
                            identity(source.lstat()) != identity(before)):
                        destination.unlink()
                        pending = True
                        continue
                except FileNotFoundError:
                    if created:
                        destination.unlink(missing_ok=True)
                    continue
                except BaseException:
                    if created:
                        destination.unlink(missing_ok=True)
                    raise
                total += copied
                stored += stored_bytes
                seen.add(source)
                row = dict(original=str(source), file=str(destination), bytes=copied,
                           sha256=digest)
                if compress:
                    row.update(compression='gzip', stored_bytes=stored_bytes)
                print(json.dumps(row), file=index, flush=True)
            if done and (not pending or cancelled):
                result = dict(host=host, actor=actor, profiles=len(seen), bytes=total,
                              stored_bytes=stored,
                              cancelled=cancelled,
                              wall_seconds=time.monotonic() - started)
                print(json.dumps(result), flush=True)
                return result
            time.sleep(0.2)
    raise TimeoutError('profile observer did not drain before its deadline')


def verify(root, max_bytes=2 << 30, max_profiles=8192, output_root=None,
           compress=False, max_stored_bytes=None):
    root, config, node, provider, output = locations(root, output_root)
    actor = provider['id']
    operation = read_json(root / 'd/state' / config['deployment_id'] / 'state.json')
    evidence = root / 'd/evidence' / config['deployment_id'] / operation['boot_id']
    observed = read_json(evidence / 'nodes' / node['id'] / 'gpu-allocation.json')
    if (str(observed['job_id']) != str(operation['job_reference']['job_id']) or
            observed['host'].split('.')[0] != socket.gethostname().split('.')[0]):
        raise ValueError('archive verification GPU receipt binding differs')
    device, = observed['devices']
    total = 0
    stored = 0
    storage_limit = max_bytes if max_stored_bytes is None else max_stored_bytes
    seen = set()
    with (root / (actor + '-profiles.jsonl')).open() as index:
        while True:
            line = index.readline(64 << 10)
            if not line:
                break
            if not line.endswith('\n') or len(seen) >= max_profiles:
                raise ValueError('profile archive index exceeds its bound')
            row = json.loads(line)
            path = Path(row['file'])
            before = path.lstat()
            raw_bytes = row['bytes']
            stored_bytes = row.get('stored_bytes', raw_bytes)
            if row.get('compression', 'none') != ('gzip' if compress else 'none'):
                raise ValueError('profile archive compression differs')
            if (path.parent != output or not stat.S_ISREG(before.st_mode) or
                    before.st_uid != os.getuid() or path in seen or
                    not 0 < raw_bytes <= (256 << 20 if compress else 128 << 20) or
                    not 0 < before.st_size == stored_bytes or
                    total + raw_bytes > max_bytes or stored + stored_bytes > storage_limit):
                raise ValueError('profile archive identity or capacity differs')
            if compress:
                payload = bytearray()
                with gzip.open(path, 'rb') as stream:
                    while True:
                        chunk = stream.read(min(1 << 20, raw_bytes - len(payload) + 1))
                        if not chunk:
                            break
                        payload.extend(chunk)
                        if len(payload) > raw_bytes:
                            raise ValueError('profile archive uncompressed size differs')
                if len(payload) != raw_bytes:
                    raise ValueError('profile archive uncompressed size differs')
            else:
                payload = path.read_bytes()
            if (identity(path.lstat()) != identity(before) or
                    hashlib.sha256(payload).hexdigest() != row['sha256']):
                raise ValueError('profile archive contents differ')
            if not isinstance(json.loads(payload), list):
                raise ValueError('profile archive JSON is not an array')
            total += len(payload)
            stored += stored_bytes
            seen.add(path)
    result = dict(host=socket.gethostname().split('.')[0], gpu_uuid=device['uuid'],
                  profiles=len(seen), bytes=total, profile_json_validated=True,
                  stored_bytes=stored,
                  scope='preserved profile bytes and JSON only; inference qualification separate')
    with (root / (actor + '-profile-verification.json')).open('x') as receipt:
        json.dump(result, receipt, sort_keys=True)
    print(json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation_root', type=Path)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--timeout', type=int, default=9000)
    parser.add_argument('--max-bytes', type=int, default=2 << 30)
    parser.add_argument('--max-profiles', type=int, default=8192)
    parser.add_argument('--compress', action='store_true',
                        help='preserve raw SHA-256 using gzip; at most 256 MiB per source profile')
    parser.add_argument('--max-stored-bytes', type=int,
                        help='archive disk budget; defaults to the uncompressed --max-bytes budget')
    args = parser.parse_args()
    if not (0 < args.timeout <= 86400 and 0 < args.max_bytes <= 64 << 30 and
            0 < args.max_profiles <= 200000 and
            (args.max_stored_bytes is None or 0 < args.max_stored_bytes <= 64 << 30)):
        parser.error('observer deadline or capacity is outside its supported bounds')
    if args.verify_only:
        verify(args.operation_root, args.max_bytes, args.max_profiles, args.output_root,
               args.compress, args.max_stored_bytes)
    else:
        capture(args.operation_root, args.timeout, args.max_bytes, args.max_profiles,
                args.output_root, args.compress, args.max_stored_bytes)


if __name__ == '__main__':
    main()
