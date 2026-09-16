#!/usr/bin/env python3
"""Compile existing C++ DI unit tests against a hash-bound candidate SIF.

Only tests and fixtures are supplied by the host. Production headers, libraries
and compiler come from the candidate. This is not MiniNDN qualification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import time

TESTS = ('di-runtime.t.cpp', 'di-preparation.t.cpp',
         'distributed-inference-native-plan.t.cpp',
         'distributed-inference-native-yolo-merge.t.cpp')
INPUTS = ('tests/main.cpp', 'tests/boost-test.hpp', 'tests/fixtures/spec182',
          *('tests/unit-tests/' + name for name in TESTS))


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sif', required=True, type=Path)
    parser.add_argument('--expected-sif-sha256', required=True)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    sif = args.sif.resolve(strict=True)
    source = args.source.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    record = {'status': 'FAIL', 'scope': 'DI_CPP_UNIT_SMOKE_ONLY'}
    started = time.monotonic()
    try:
        if digest(sif) != args.expected_sif_sha256:
            raise ValueError('SIF_HASH_MISMATCH')
        record['sifSha256'] = args.expected_sif_sha256
        revision = subprocess.check_output(
            ['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        if not re.fullmatch('[0-9a-f]{40}', revision):
            raise ValueError('SOURCE_REVISION_INVALID')
        record['sourceRevision'] = revision
        archive = output / 'tests.tar'
        subprocess.run(['git', '-C', str(source), 'archive', '--format=tar',
                        '--output=' + str(archive), revision, '--', *INPUTS], check=True)
        record['testArchiveSha256'] = digest(archive)
        inputs = output / 'inputs'
        inputs.mkdir()
        home = output / 'home'
        home.mkdir()
        home_stat = home.lstat()
        if (not stat.S_ISDIR(home_stat.st_mode) or
                home_stat.st_uid != os.geteuid()):
            raise ValueError('PRIVATE_HOME_DIRECTORY_INVALID')
        os.chmod(home, 0o700)
        if stat.S_IMODE(home.stat().st_mode) != 0o700:
            raise ValueError('PRIVATE_HOME_MODE_INVALID')
        subprocess.run(['tar', '-xf', str(archive), '-C', str(inputs)], check=True)
        record['testFiles'] = {str(p.relative_to(inputs)): digest(p)
                               for p in sorted(inputs.rglob('*')) if p.is_file()}
        apptainer = Path(shutil.which('apptainer')).resolve(strict=True)
        version = subprocess.check_output([str(apptainer), 'version'], text=True).strip()
        if version != '1.5.3':
            raise ValueError('APPTAINER_VERSION_MISMATCH')
        record['apptainer'] = {'path': str(apptainer), 'version': version,
                               'sha256': digest(apptainer)}
        identity = ('import json,sys; from pathlib import Path; '
                    'p=Path("/opt/ndnsf-di/replay/source-seal.json"); '
                    'assert json.loads(p.read_text())["sourceRevision"] == sys.argv[1], '
                    '"CANDIDATE_TEST_SOURCE_MISMATCH"')
        # All interpolated shell tokens below are fixed strings or a validated Git ID.
        script = ('set -eu\nexport PATH=/usr/bin:/bin\n'
                  'export HOME=/home/tianxing\n'
                  'export LD_LIBRARY_PATH=/opt/ndnsf-di/current/lib:/opt/ndn-base/lib:/opt/onnxruntime/lib\n'
                  f"/usr/bin/python3 -c '{identity}' {revision}\n"
                  'cd /test-inputs\n'
                  '/usr/bin/g++ -B/usr/bin -std=c++17 -O0 -g0 -pthread '
                  '$(/usr/bin/pkg-config --cflags ndnsf-distributed-inference libnac-abe) '
                  '-DBOOST_TEST_DYN_LINK -I/test-inputs -I/opt/ndnsf-di/replay/repo '
                  '-I/opt/ndnsf-di/current/include -I/opt/ndn-base/include '
                  'tests/main.cpp ' + ' '.join('tests/unit-tests/' + name for name in TESTS) +
                  ' -L/opt/ndnsf-di/current/lib -L/opt/ndn-base/lib '
                  '-Wl,-rpath,/opt/ndnsf-di/current/lib -Wl,-rpath,/opt/ndn-base/lib '
                  '-lndnsf-distributed-inference -lndn-service-framework '
                  '$(/usr/bin/pkg-config --libs ndnsf-distributed-inference libnac-abe) '
                  '-lboost_unit_test_framework -lcrypto -lboost_system '
                  '-o /evidence/di-unit-smoke\n'
                  'ldd /evidence/di-unit-smoke\n'
                  '/usr/bin/timeout --kill-after=5s 180s /evidence/di-unit-smoke '
                  '--report_level=detailed --log_level=test_suite\n'
                  'echo DI_CPP_UNIT_SMOKE_PASS\n')
        (output / 'run.sh').write_text(script)
        command = [str(apptainer), 'exec', '--cleanenv', '--containall',
                   '--no-mount', 'home,cwd,hostfs,bind-paths',
                   '--bind', str(inputs) + ':/test-inputs:ro',
                   '--bind', str(home) + ':/home/tianxing',
                   '--bind', str(output) + ':/evidence', str(sif),
                   '/bin/sh', '/evidence/run.sh']
        record['command'] = command
        with (output / 'run.log').open('w') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                rc = process.wait(timeout=600)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise
        if rc:
            raise subprocess.CalledProcessError(rc, command)
        if 'DI_CPP_UNIT_SMOKE_PASS' not in (output / 'run.log').read_text():
            raise ValueError('CPP_PASS_MARKER_MISSING')
        if digest(sif) != args.expected_sif_sha256:
            raise ValueError('SIF_CHANGED')
        for name, expected in record['testFiles'].items():
            if digest(inputs / name) != expected:
                raise ValueError('TEST_INPUT_CHANGED:' + name)
        record['binarySha256'] = digest(output / 'di-unit-smoke')
        record['status'] = 'PASS'
    except Exception as error:
        record['error'] = str(error)
        raise
    finally:
        record['elapsedSeconds'] = time.monotonic() - started
        (output / 'record.json').write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    main()
