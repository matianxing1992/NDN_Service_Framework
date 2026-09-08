#!/usr/bin/env python3
"""Bounded shared-filesystem lock probe in a NEW isolated root; no Slurm calls."""
import argparse
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_submission import JournalError, SubmissionJournal, transport_guard
from runtime.yolo_transport import _path, inventory, receive


def probe(root):
    root = _path(str(root))
    root.mkdir(mode=0o700)  # Never clear, reuse or adopt an existing directory.
    locks = root/'locks'
    locks.mkdir()
    roots = (root/'artifacts', root/'runs')
    sources = [roots[0]/'fixture.txt', roots[1]/'fixture-run/prepare.json']
    for index, path in enumerate(sources):
        path.parent.mkdir(parents=True)
        path.write_bytes(('transport lock fixture '+str(index)+'\n').encode())
        path.chmod(0o444)
    manifest = inventory(sources, roots=roots, candidate_digest='sha256:'+'a'*64)
    staging = root/'incoming'
    (staging/'blobs').mkdir(parents=True)
    for row in manifest['files']:
        shutil.copyfile(row['path'], staging/'blobs'/row['sha256'].split(':')[1])
        Path(row['path']).unlink()
    journal = SubmissionJournal(locks,candidate_id='sha256:'+'b'*64,gate='single-node-gpu')
    journal.reserve('synthetic-reserved')
    try:
        receive(manifest,roots=roots,staging=staging,lock_root=locks)
    except JournalError as exc:
        if str(exc) != 'TRANSPORT_ACTIVE_SUBMISSION': raise
    else:
        raise AssertionError('active reservation did not exclude publication')
    assert not any(path.exists() for path in sources)
    journal.cancel_prepared('synthetic-reserved')
    child = subprocess.Popen([sys.executable,'-B',str(Path(__file__).resolve()),
        '--hold-lock',str(locks)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,text=True)
    try:
        assert select.select([child.stdout],[],[],5)[0], 'lock child readiness timeout'
        assert child.stdout.readline().strip() == 'LOCKED'
        try:
            journal.reserve('synthetic-after-lock')
        except JournalError as exc:
            if str(exc) != 'JOURNAL_LOCK_TIMEOUT': raise
        else:
            raise AssertionError('exclusive transport lock did not exclude reserve')
    finally:
        try: _, error = child.communicate('release\n',timeout=5)
        except subprocess.TimeoutExpired:
            child.kill(); _, error = child.communicate(timeout=5)
    assert child.returncode == 0, error
    journal.reserve('synthetic-after-lock')
    journal.cancel_prepared('synthetic-after-lock')
    result = receive(manifest,roots=roots,staging=staging,lock_root=locks)
    receipt = dict(scope='TRANSPORT_LOCK_PROBE_ONLY',qualification='NOT_EVALUATED',
        blockedActiveSubmission=True,blockedConcurrentReservation=True,
        childReaped=True,transport=result)
    with (root/'probe.json').open('x') as stream:
        json.dump(receipt,stream,sort_keys=True)
        stream.flush(); os.fsync(stream.fileno())
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument('--root',type=Path)
    actions.add_argument('--hold-lock',type=Path)
    args = parser.parse_args()
    if args.hold_lock:
        with transport_guard(args.hold_lock):
            print('LOCKED',flush=True)
            if not select.select([sys.stdin],[],[],15)[0]:
                raise TimeoutError('lock holder release timeout')
            if sys.stdin.readline() != 'release\n':
                raise ValueError('lock holder release protocol')
    else:
        print(json.dumps(probe(args.root),sort_keys=True))
