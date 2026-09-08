"""Owned compute scratch and measured capacity; never a model qualification."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import subprocess
import tempfile
import time

from .identities import _credential_document
from .yolo_profile import HASH, _read_plane

LOCAL_FILESYSTEMS = {'ext2/ext3','ext4','xfs','btrfs','tmpfs','overlay','overlayfs','zfs'}


def _directory(path):
    path = Path(path)
    if (not path.is_absolute() or '..' in path.parts or not path.is_dir()
            or any(p.is_symlink() for p in (path, *path.parents))):
        raise ValueError('STORAGE_DIRECTORY')
    return path


def measured_capacity(path, required):
    """Available filesystem blocks, not a quota or space-reservation claim."""
    path = _directory(path)
    if type(required) is not int or required < 0:
        raise ValueError('STORAGE_REQUIRED_BYTES')
    info = os.statvfs(path)
    free = info.f_bavail * info.f_frsize
    if free < required or (info.f_files and info.f_favail < 16):
        raise ValueError('STORAGE_INSUFFICIENT')
    return dict(path=str(path), freeBytes=free, availableInodes=info.f_favail,
                quotaVerified=False, requiredBytes=required)


class NodeScratch:
    """Create a unique local directory and delete it only after clean ownership exit.

    Caller must establish Slurm identity before entering. On workload failure
    keep the SIF because a failed cleanup can leave processes mapping it.
    Records stay under the durable run root, never solely in scratch.
    """
    def __init__(self, *, prepared, profile, runtime_profile, allocation, rank):
        self.root = _directory(prepared['plan']['output'])
        self.profile, self.runtime_profile = profile, dict(runtime_profile)
        receipt = allocation['receipt']
        if (type(rank) is not int or rank not in (0,1) or receipt['rank'] != rank
                or not HASH.fullmatch(prepared['candidateDigest'])
                or not isinstance(runtime_profile.get('sifSha256'), str)
                or re.fullmatch(r'[a-f0-9]{64}', runtime_profile['sifSha256']) is None
                or type(runtime_profile.get('sifBytes')) is not int or runtime_profile['sifBytes'] <= 0):
            raise ValueError('STORAGE_BINDING')
        self.rank = rank
        self.record = dict(schema='tiger-yolo-storage-v1', runId=prepared['runId'],
            candidateDigest=prepared['candidateDigest'], jobId=receipt['jobId'],
            hostname=receipt['hostname'], rank=rank, sifSha256='sha256:'+runtime_profile['sifSha256'],
            sifBytes=runtime_profile['sifBytes'], scratch=None, status='STARTED')
        self.path = None
        self.final = self.root / ('storage-rank'+str(rank)+'.json')
        if self.final.exists() or self.final.is_symlink():
            raise ValueError('STORAGE_ALREADY_USED')

    def __enter__(self):
        deadline = time.monotonic() + self.profile['timing']['stagingSeconds']
        self.deadline = deadline
        try:
            base = _directory(os.environ.get('SLURM_TMPDIR') or self.profile['storage']['scratchRoot'])
            query = subprocess.run(['/usr/bin/stat','-f','--format=%T',str(base)],
                capture_output=True, check=True, timeout=max(0.01,deadline-time.monotonic()),
                env={'PATH':'/usr/bin:/bin','LC_ALL':'C'})
            fs = query.stdout.decode('ascii').strip()
            if fs not in LOCAL_FILESYSTEMS:
                raise ValueError('STORAGE_NOT_LOCAL_FILESYSTEM')
            required = self.profile['storage']['peakBytes'] + self.profile['storage']['marginBytes']
            if self.runtime_profile['sifBytes'] > self.profile['storage']['peakBytes']:
                raise ValueError('STORAGE_SIF_BUDGET')
            self.record.update(filesystem=fs, scratchCapacity=measured_capacity(base, required),
                               outputCapacity=measured_capacity(self.root, required))
            self.path = Path(tempfile.mkdtemp(prefix='ndnsf-di-'+self.record['runId']+'-r'+str(self.rank)+'-', dir=base))
            self.record['scratch'] = str(self.path)
            self.node = self.path/'node'
            self.node.mkdir(mode=0o700)
            probe = self.path/'fsync-probe'
            with probe.open('xb') as stream:
                stream.write(b'\0'*4096)
                stream.flush()
                os.fsync(stream.fileno())
            probe.unlink()
            # FD-relative spelling keeps the AF_UNIX probe below sun_path's
            # limit even when the allocation supplies a long scratch prefix.
            fd = os.open(self.node, os.O_RDONLY | os.O_DIRECTORY)
            try:
                with socket.socket(socket.AF_UNIX) as sock:
                    sock.bind('/proc/self/fd/'+str(fd)+'/socket-probe')
                (self.node/'socket-probe').unlink()
            finally:
                os.close(fd)
            source = Path(self.runtime_profile['sif'])
            _directory(source.parent)
            source_fd = os.open(source, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
            destination = self.path/'runtime.sif'
            digest, total = hashlib.sha256(), 0
            with os.fdopen(source_fd, 'rb') as incoming, destination.open('xb') as outgoing:
                before = os.fstat(incoming.fileno())
                if not stat.S_ISREG(before.st_mode) or before.st_size != self.record['sifBytes']:
                    raise ValueError('STORAGE_SIF_SIZE')
                while True:
                    if time.monotonic() >= deadline:
                        raise ValueError('STORAGE_STAGING_TIMEOUT')
                    chunk = incoming.read(1024*1024)
                    if not chunk: break
                    total += len(chunk)
                    if total > self.record['sifBytes']:
                        raise ValueError('STORAGE_SIF_CHANGED')
                    outgoing.write(chunk)
                    digest.update(chunk)
                outgoing.flush()
                os.fsync(outgoing.fileno())
                after = os.fstat(incoming.fileno())
                if (total != self.record['sifBytes'] or digest.hexdigest() != self.runtime_profile['sifSha256']
                        or (before.st_size,before.st_mtime_ns,before.st_ctime_ns) !=
                           (after.st_size,after.st_mtime_ns,after.st_ctime_ns)):
                    raise ValueError('STORAGE_SIF_CHANGED')
            destination.chmod(0o444)
            self.runtime_profile['sif'] = str(destination)
            self.record.update(probeBytes=4096, socketVerified=True, copiedBytes=total,
                               stagingSeconds=self.profile['timing']['stagingSeconds']-(deadline-time.monotonic()))
            _credential_document(self.root/('storage-rank'+str(self.rank)+'-start.json'), self.record)
            return self
        except BaseException as exc:
            if self.path is not None:
                shutil.rmtree(self.path)
            _credential_document(self.final, dict(self.record, status='STAGING_FAILED', errorType=type(exc).__name__))
            raise

    def __exit__(self, kind, value, traceback):
        if kind is not None:
            _credential_document(self.final, dict(self.record, status='RETAINED_ON_FAILURE', errorType=kind.__name__))
            return False
        try:
            shutil.rmtree(self.path)
            if self.path.exists(): raise ValueError('STORAGE_CLEANUP_INCOMPLETE')
        except BaseException as exc:
            _credential_document(self.final, dict(self.record, status='CLEANUP_FAILED', errorType=type(exc).__name__))
            raise
        _credential_document(self.final, dict(self.record, status='CLEANED'))
        return False


def verify_storage_cleanup(root, prepared, job_id):
    ranks = (0,1) if prepared['case'] in ('two-node-gpu', 'negative-dependency') else (0,)
    rows=[]
    for rank in ranks:
        row = _read_plane(Path(root)/('storage-rank'+str(rank)+'.json'))
        if (not isinstance(row, dict) or row.get('schema')!='tiger-yolo-storage-v1'
                or row.get('runId')!=prepared['runId'] or row.get('candidateDigest')!=prepared['candidateDigest']
                or row.get('jobId')!=job_id or type(row.get('rank')) is not int or row['rank']!=rank
                or row.get('status')!='CLEANED' or row.get('socketVerified') is not True
                or row.get('filesystem') not in LOCAL_FILESYSTEMS or row.get('probeBytes')!=4096
                or not isinstance(row.get('hostname'), str)
                or re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.-]{0,252}', row['hostname']) is None
                or not isinstance(row.get('sifSha256'), str) or not HASH.fullmatch(row['sifSha256'])
                or type(row.get('sifBytes')) is not int or row['sifBytes']<=0
                or type(row.get('copiedBytes')) is not int or row['copiedBytes']!=row['sifBytes']):
            raise ValueError('STORAGE_CLEANUP_BINDING')
        for key in ('scratchCapacity', 'outputCapacity'):
            capacity = row.get(key)
            if (not isinstance(capacity, dict) or type(capacity.get('freeBytes')) is not int
                    or type(capacity.get('requiredBytes')) is not int or capacity['requiredBytes']<0
                    or capacity['freeBytes']<capacity['requiredBytes']):
                raise ValueError('STORAGE_CAPACITY_BINDING')
        rows.append(row)
    if len({r['sifSha256'] for r in rows})!=1 or len({r['hostname'] for r in rows})!=len(ranks):
        raise ValueError('STORAGE_NODE_OR_SIF_BINDING')
    return rows
