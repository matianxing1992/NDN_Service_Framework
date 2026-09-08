"""Real receiver filesystem operations; transport is never runtime evidence."""
import copy
import json
import os
from pathlib import Path
import shutil
import select
import subprocess
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_transport as transport


@pytest.fixture
def transfer(tmp_path):
    roots = (tmp_path / 'artifacts', tmp_path / 'runs')
    paths = [roots[0] / 'candidate/profile.json', roots[1] / 'run/prepare.json']
    for index, path in enumerate(paths):
        path.parent.mkdir(parents=True)
        path.write_bytes(('file-' + str(index)).encode())
        path.chmod(0o600 if index else 0o444)
    manifest = transport.inventory(paths, roots=roots, candidate_digest='sha256:'+'a'*64,
                                   private_paths=[paths[1]])
    staging = tmp_path / 'incoming'
    (tmp_path / 'locks').mkdir()
    (staging / 'blobs').mkdir(parents=True)
    for row in manifest['files']:
        shutil.copyfile(row['path'], staging/'blobs'/row['sha256'].split(':')[1])
        Path(row['path']).unlink()
    return roots, paths, manifest, staging


def receive(manifest, *, roots, staging):
    return transport.receive(manifest, roots=roots, staging=staging,
                             lock_root=staging.parent/'locks')


def test_publish_and_retry_preserve_original_paths_bytes_modes_and_identity(transfer):
    roots, paths, manifest, staging = transfer
    result = receive(manifest, roots=roots, staging=staging)
    assert result['status'] == 'CONTENT_VERIFIED' and result['qualification'] == 'NOT_EVALUATED'
    assert result['candidateDigest'] == manifest['candidateDigest'] and result['reused'] == 0
    before = [(path.read_bytes(), path.stat().st_ino, path.stat().st_mode) for path in paths]
    # Complete retry needs no retransmission of already matching blobs.
    shutil.rmtree(staging/'blobs')
    again = receive(manifest, roots=roots, staging=staging)
    assert again['reused'] == 2 and again['manifestDigest'] == result['manifestDigest']
    assert before == [(path.read_bytes(), path.stat().st_ino, path.stat().st_mode) for path in paths]


@pytest.mark.parametrize('fault', ['bad-blob', 'conflict', 'mode', 'parent', 'symlink', 'capacity'])
def test_rejects_every_input_before_first_publication(transfer, monkeypatch, fault):
    roots, paths, manifest, staging = transfer
    if fault == 'bad-blob':
        (staging/'blobs'/manifest['files'][1]['sha256'].split(':')[1]).write_bytes(b'corrupt')
    elif fault in ('conflict','mode'):
        paths[1].write_bytes(b'wrong' if fault == 'conflict' else b'file-1')
        paths[1].chmod(0o644)
    elif fault == 'parent':
        paths[1].parent.rmdir()
        paths[1].parent.write_bytes(b'not a directory')
    elif fault == 'symlink':
        paths[1].symlink_to(staging/'blobs'/manifest['files'][1]['sha256'].split(':')[1])
    else:
        monkeypatch.setattr(transport.os,'statvfs',lambda _: SimpleNamespace(f_bavail=0,f_frsize=4096))
    with pytest.raises((ValueError, OSError)):
        receive(manifest, roots=roots, staging=staging)
    assert not paths[0].exists()
    assert not list(roots[0].rglob('.spec183-transfer-*'))


def test_interrupted_publication_resumes_without_overwriting_first_file(transfer, monkeypatch):
    roots, paths, manifest, staging = transfer
    link = os.link
    def interrupt(source, target, **kwargs):
        if Path(target) == paths[1]:
            raise OSError('simulated interruption')
        return link(source, target, **kwargs)
    monkeypatch.setattr(transport.os,'link',interrupt)
    with pytest.raises(OSError, match='simulated interruption'):
        receive(manifest, roots=roots, staging=staging)
    inode = paths[0].stat().st_ino
    assert not paths[1].exists()
    assert not list(roots[0].rglob('.spec183-transfer-*'))
    assert not list(roots[1].rglob('.spec183-transfer-*'))
    monkeypatch.setattr(transport.os,'link',link)
    assert receive(manifest, roots=roots, staging=staging)['reused'] == 1
    assert paths[0].stat().st_ino == inode


@pytest.mark.parametrize('fault', ['outside', 'duplicate', 'root', 'private-mode', 'parent-collision'])
def test_manifest_rejects_ambiguous_or_unsafe_destinations_before_copy(transfer,fault):
    roots, paths, manifest, staging = transfer
    value = copy.deepcopy(manifest)
    if fault == 'outside': value['files'][0]['path'] = str(staging.parent/'outside')
    elif fault == 'duplicate': value['files'].append(value['files'][0])
    elif fault == 'root': value['roots'][0] = '/'
    elif fault == 'private-mode': value['files'][1]['mode'] = 0o644
    else:
        value['files'][1]['path'] = value['files'][0]['path']+'/child'
    with pytest.raises(ValueError): receive(value, roots=roots, staging=staging)
    assert not any(path.exists() for path in paths)


def test_inventory_refuses_fifo_without_blocking(tmp_path):
    roots = (tmp_path/'a',tmp_path/'r')
    roots[0].mkdir()
    fifo = roots[0]/'fifo'
    os.mkfifo(fifo)
    with pytest.raises(ValueError,match='REGULAR_FILE'):
        transport.inventory([fifo],roots=roots,candidate_digest='sha256:'+'a'*64)


def test_real_receiver_cli_reports_only_transport_verification(transfer,tmp_path):
    roots, paths, manifest, staging = transfer
    document = staging/'manifest.json'
    document.write_text(json.dumps(manifest))
    script = Path(__file__).resolve().parents[1]/'tools/spec183_transport.py'
    result = subprocess.run([sys.executable,str(script),'receive','--artifact-root',str(roots[0]),
        '--run-root',str(roots[1]),'--input',str(document),'--staging',str(staging),
        '--lock-root',str(staging.parent/'locks')],
        check=True,capture_output=True,text=True,timeout=10)
    assert json.loads(result.stdout)['qualification'] == 'NOT_EVALUATED'
    assert all(path.is_file() for path in paths)


@pytest.mark.parametrize('state', ['PREPARED','SUBMITTING','SUBMISSION_UNKNOWN','SUBMITTED','RUNNING'])
def test_receiver_rejects_every_unclosed_submission_before_publishing(transfer,state):
    from runtime.yolo_submission import SubmissionJournal
    roots, paths, manifest, staging = transfer
    journal = SubmissionJournal(staging.parent/'locks',candidate_id='sha256:'+'b'*64,gate='two-node-gpu')
    journal.reserve('live-run')
    if state != 'PREPARED': journal.mark_submitting('live-run')
    if state == 'SUBMISSION_UNKNOWN': journal.mark_unknown('live-run')
    if state in ('SUBMITTED','RUNNING'): journal.record_submission('live-run','123')
    if state == 'RUNNING': journal.mark_running('live-run','123')
    with pytest.raises(ValueError,match='TRANSPORT_ACTIVE_SUBMISSION'):
        receive(manifest,roots=roots,staging=staging)
    assert not any(path.exists() for path in paths)
    assert journal.get('live-run')['state'] == state


def test_closed_journal_allows_transport_without_changing_history(transfer):
    from runtime.yolo_submission import SubmissionJournal
    roots, paths, manifest, staging = transfer
    journal = SubmissionJournal(staging.parent/'locks',candidate_id='sha256:'+'b'*64,gate='two-node-gpu')
    journal.reserve('cancelled-run')
    journal.cancel_prepared('cancelled-run')
    before = journal.path.read_bytes()
    assert receive(manifest,roots=roots,staging=staging)['files'] == 2
    assert journal.path.read_bytes() == before


def test_transport_lock_excludes_new_reservation_in_another_process(transfer):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    roots, paths, manifest, staging = transfer
    root = Path(__file__).resolve().parents[1]
    script = '''import sys
sys.path.insert(0,sys.argv[1])
from runtime.yolo_submission import transport_guard
with transport_guard(sys.argv[2]):
 print('LOCKED',flush=True)
 sys.stdin.readline()
'''
    child = subprocess.Popen([sys.executable,'-B','-c',script,str(root),str(staging.parent/'locks')],
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    journal = SubmissionJournal(staging.parent/'locks',candidate_id='sha256:'+'c'*64,gate='single-node-gpu')
    try:
        assert select.select([child.stdout],[],[],5)[0]
        assert child.stdout.readline().strip() == 'LOCKED'
        with pytest.raises(JournalError,match='JOURNAL_LOCK_TIMEOUT'):
            journal.reserve('new-run')
        assert not journal.path.exists()
    finally:
        try: child.communicate('release\n',timeout=5)
        except subprocess.TimeoutExpired:
            child.kill(); child.communicate(timeout=5)
    assert child.returncode == 0
    assert journal.reserve('new-run')['state'] == 'PREPARED'
