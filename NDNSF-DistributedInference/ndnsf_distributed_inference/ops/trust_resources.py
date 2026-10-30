"""Translate bounded native resource records into filesystem input admission."""
from __future__ import annotations

import base64
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import resource
import tempfile
import time

from .deployment_config import _translate_native
from .processes import run_finite_application


MAX_BYTES = 1024 * 1024
MAX_RECORDS = 1024


class TrustResourceError(RuntimeError):
    def __init__(self, message, proof=None):
        super().__init__(message)
        self.proof = proof


def describe_failure(error):
    """Keep bounded failed-probe evidence in the existing caller error stream."""
    proof = getattr(error, 'proof', None) or getattr(error, 'trust_resource_proof', None)
    if proof is None:
        return str(error)
    return str(error) + '\nNDNSF_TRUST_RESOURCE_FAILURE=' + json.dumps(
        proof, sort_keys=True, separators=(',', ':'))


def decode_resources(raw, schema):
    """Decode the native helper contract; never interpret a trust schema in Python."""
    if not isinstance(raw, bytes) or len(raw) > MAX_BYTES:
        raise TrustResourceError('TRUST_RESOURCE_OUTPUT_LIMIT')
    fields = raw.split(b'\0')
    if (fields[0] != b'NDNSF_TRUST_RESOURCES_V1' or fields[-1] != b'' or
            len(fields) < 6 or len(fields) % 2 or (len(fields) - 2) // 2 > MAX_RECORDS):
        raise TrustResourceError('TRUST_RESOURCE_FRAMING')
    records = {}
    for index in range(1, len(fields) - 1, 2):
        flag, path = fields[index:index + 2]
        if flag not in (b'R', b'O'):
            raise TrustResourceError('TRUST_RESOURCE_REQUIRED_FLAG')
        try:
            text = path.decode('utf-8')
        except UnicodeDecodeError as error:
            raise TrustResourceError('TRUST_RESOURCE_PATH_ENCODING') from error
        if not text or not Path(text).is_absolute() or any(ord(c) < 32 for c in text):
            raise TrustResourceError('TRUST_RESOURCE_PATH_INVALID')
        if text in records:
            raise TrustResourceError('TRUST_RESOURCE_DUPLICATE')
        records[text] = flag == b'R'
    schema = str(schema)
    if records.get(schema) is not True or schema + '.producer-signers' not in records:
        raise TrustResourceError('TRUST_RESOURCE_SCHEMA_BINDING')
    if any(not required and path != schema + '.producer-signers'
           for path, required in records.items()):
        raise TrustResourceError('TRUST_RESOURCE_OPTIONAL_BINDING')
    return records


def inspect_resources(command, schema, env, timeout):
    """Use the maintained finite owner and separate bounded anonymous streams."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise TrustResourceError('TRUST_RESOURCE_TIMEOUT_INVALID')
    cleanup = []
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic()
    failure = None
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as diagnostic:
        try:
            run_finite_application('trust-resource-inspection', command, diagnostic, cleanup,
                                   seconds=min(15, timeout), env=env, stdout=output)
        except BaseException as error:
            failure = error
        output.seek(0)
        raw = output.read(MAX_BYTES + 1)
        diagnostic.seek(0)
        errors = diagnostic.read(65537)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    proof = dict(schema='ndnsf-trust-resource-inspection-v1', command=list(command),
                 wall_seconds=time.monotonic() - started,
                 child_cpu_seconds=after.ru_utime + after.ru_stime -
                                   before.ru_utime - before.ru_stime,
                 stdout_base64=base64.b64encode(raw).decode('ascii'),
                 stderr_base64=base64.b64encode(errors).decode('ascii'),
                 stdout_sha256=hashlib.sha256(raw).hexdigest(),
                 stdout_limit_exceeded=len(raw) > MAX_BYTES,
                 stderr_limit_exceeded=len(errors) > 65536,
                 cleanup=cleanup, failure=None if failure is None else repr(failure))
    if failure is not None:
        if isinstance(failure, (KeyboardInterrupt, SystemExit)):
            failure.trust_resource_proof = proof
            raise failure
        raise TrustResourceError('TRUST_RESOURCE_PROBE_FAILED:' + str(failure), proof) from failure
    if errors or len(raw) > MAX_BYTES or len(cleanup) != 1 or any(
            row['forced'] or not row['reaped'] or row['error'] for row in cleanup):
        raise TrustResourceError('TRUST_RESOURCE_PROBE_OUTPUT_OR_CLEANUP', proof)
    try:
        records = decode_resources(raw, schema)
    except TrustResourceError as error:
        error.proof = proof
        raise
    proof['resources'] = records
    return records, proof


def admit_resources(config, records):
    """Protect all candidates, including absent optional files, from configured writes."""
    from .container import _canonical, _contains

    native = copy.deepcopy(config)
    by_kind = {kind: [actor for actor in native['actors'] if actor['kind'] == kind]
               for kind in ('controller', 'artifact-authority', 'requester', 'provider', 'repo')}

    def resolved(value, field, **kwargs):
        return value

    writes = _translate_native(native, by_kind, resolved)
    writes.extend((value, True, 'storage.' + key) for key, value in
                  config['storage'].items() if key != 'max_bytes')
    writes.extend((actor['credential_home'], True, 'actor.credential_home')
                  for actor in config['actors'] if not actor['external'])
    for node in config['nodes']:
        if node['nfd']['mode'] == 'managed':
            socket = Path(node['nfd']['socket'])
            directory = config['platform']['kind'] == 'slurm-apptainer'
            writes.append((str(socket.parent if directory else socket), directory, 'node.nfd.socket'))
    present = []
    for path, required in records.items():
        candidate = _canonical(path)
        for value, directory, field in writes:
            target = _canonical(value)
            if candidate == target or (directory and _contains(target, candidate)):
                raise TrustResourceError('TRUST_RESOURCE_OUTPUT_OVERLAP:' + field + ':' + path)
        if not os.path.lexists(candidate) and not required:
            continue
        if not candidate.is_file() or not os.access(candidate, os.R_OK):
            raise TrustResourceError('TRUST_RESOURCE_INPUT_UNAVAILABLE:' + path)
        present.append(candidate)
    return present
