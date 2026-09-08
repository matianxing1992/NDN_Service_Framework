#!/usr/bin/env python3
"""Verify stable native artifacts; this receipt never qualifies an application."""
import hashlib
import importlib
import importlib.util
import json
import platform
from pathlib import Path
import subprocess
import sys

PREFIX = Path('/opt/ndnsf-di/current')
MANIFEST = PREFIX / 'manifest/base-runtime.json'


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return 'sha256:' + h.hexdigest()


def inspect():
    assert sys.version_info[:2] == (3, 10), 'BASE_PYTHON_ABI'
    assert platform.machine() == 'x86_64', 'BASE_ARCHITECTURE'
    artifacts = [PREFIX / 'lib' / name for name in (
        'libndn-service-framework.so', 'libnac-abe.so', 'libndn-svs.so', 'libndnsd.so')]
    for name in ('ndnsf._ndnsf', 'py_repoclient._py_repoclient'):
        path = Path(importlib.import_module(name).__file__).resolve()
        assert path.is_relative_to('/opt/venv/lib/python3.10/site-packages'), 'BASE_EXTENSION_SHADOWED'
        artifacts.append(path)
    assert not (PREFIX / 'bin/di-native-provider').exists(), 'APPLICATION_IN_BASE'
    assert not Path('/opt/ndnsf-di/replay').exists(), 'REPLAY_IN_BASE'
    assert importlib.util.find_spec('ndnsf_distributed_inference') is None, 'DI_PACKAGE_IN_BASE'
    rows = []
    for artifact in artifacts:
        result = subprocess.run(['ldd', '-r', str(artifact)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
        # Python's own C API is supplied by the interpreter for extension DSOs.
        errors = [line for line in result.stdout.splitlines()
                  if 'not found' in line or ('undefined symbol:' in line
                      and not line.split('undefined symbol:', 1)[1].strip().startswith(('Py', '_Py')))]
        assert not errors, '\n'.join(errors)
        rows.append({'path': str(artifact), 'sha256': digest(artifact),
                     'bytes': artifact.stat().st_size})
    return {'schemaVersion': 'spec183-base-runtime-v1', 'scope': 'BASE_LIBRARIES_ONLY',
            'sourceSealSha256': digest(PREFIX / 'manifest/base-source-seal.json'),
            'python': platform.python_version(), 'artifacts': rows}


if __name__ == '__main__':
    assert len(sys.argv) == 2 and sys.argv[1] in ('record', 'verify')
    actual = inspect()
    if sys.argv[1] == 'record':
        MANIFEST.write_text(json.dumps(actual, sort_keys=True, indent=2) + '\n')
    else:
        assert actual == json.loads(MANIFEST.read_text()), 'BASE_RUNTIME_CHANGED'
    print(json.dumps({'status': 'PASS', 'scope': actual['scope']}))
