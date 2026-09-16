#!/usr/bin/env python3
"""Run the frozen YOLO26n C++ model smoke inside a hash-bound local SIF.

Packaging orchestration only: C++ owns preprocessing and numerical assertions.
This is not NDNSF request-chain or MiniNDN qualification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[5]
SOURCE = ROOT / 'Experiments/TigerCluster/tests/yolo-cpu-smoke.cpp'
FIXTURE = ROOT / 'tests/fixtures/spec180/yolo26n/fixed-fixture.ppm'
EXPECTED = {
    'canonical/yolo26n.onnx': '956ee2aa62f34c1ac035b85a837b70786bfa8da3ae8650e7539abbe572d0dd2a',
    'canonical/yolo26n.weights': '1a998d3d56c0103e57ea6df557370a219a3df53380572b4e9337ff26b4a94a7f',
    'oracle/full-model-output.npy': 'ed5a23d73e3cda04677bfc9d895732b44e1455f20d9dc433ae5462eb7b9b7175',
}


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-sif', required=True, type=Path)
    parser.add_argument('--expected-sif-sha256', required=True)
    parser.add_argument('--model-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    sif = args.base_sif.resolve(strict=True)
    models = args.model_root.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    record = {'status': 'FAIL', 'scope': 'YOLO_CPU_MODEL_SMOKE_ONLY', 'inputs': {}}
    started = time.monotonic()
    try:
        checks = [(sif, args.expected_sif_sha256),
                  (FIXTURE, '7edf1f524ef450be6ee2304b3c0b47b70c3d72610c18f2f2fa28157d7b8a113c')]
        checks.extend((models / relative, expected) for relative, expected in EXPECTED.items())
        for path, expected in checks:
            actual = digest(path)
            if actual != expected:
                raise ValueError('INPUT_HASH_MISMATCH:' + str(path))
            record['inputs'][str(path)] = actual
        shutil.copyfile(SOURCE, output / 'probe.cpp')
        record['sourceSha256'] = digest(output / 'probe.cpp')
        apptainer = str(Path(shutil.which('apptainer')).resolve(strict=True))
        record['apptainer'] = {'path': apptainer, 'sha256': digest(Path(apptainer)),
            'version': subprocess.check_output([apptainer, 'version'], text=True).strip()}
        if record['apptainer']['version'] != '1.5.3':
            raise ValueError('APPTAINER_VERSION_MISMATCH')
        command = [apptainer, 'exec', '--cleanenv', '--containall',
                   '--bind', str(output) + ':/evidence',
                   '--bind', str(models) + ':/model:ro',
                   '--bind', str(FIXTURE) + ':/fixture.ppm:ro', str(sif),
                   '/bin/sh', '-ec',
                   'export PATH=/usr/bin:/bin; '
                   '/usr/bin/g++ -B/usr/bin -std=c++17 -O2 /evidence/probe.cpp '
                   '-I/opt/onnxruntime/include -L/opt/onnxruntime/lib '
                   '-Wl,-rpath,/opt/onnxruntime/lib -lonnxruntime -o /evidence/yolo-cpu-smoke; '
                   'ldd /evidence/yolo-cpu-smoke; '
                   'exec /usr/bin/timeout --kill-after=5s 120s /evidence/yolo-cpu-smoke '
                   '/model/canonical/yolo26n.onnx /fixture.ppm /model/oracle/full-model-output.npy']
        record['command'] = command
        with (output / 'run.log').open('w') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                returncode = process.wait(timeout=180)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise
            if returncode:
                raise subprocess.CalledProcessError(returncode, command)
        if 'YOLO_CPU_MODEL_SMOKE_PASS' not in (output / 'run.log').read_text():
            raise ValueError('CPP_PASS_MARKER_MISSING')
        for path, expected in checks:
            if digest(path) != expected:
                raise ValueError('INPUT_CHANGED:' + str(path))
        record['binarySha256'] = digest(output / 'yolo-cpu-smoke')
        record['status'] = 'PASS'
    except Exception as error:
        record['error'] = str(error)
        raise
    finally:
        record['elapsedSeconds'] = time.monotonic() - started
        (output / 'record.json').write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    main()
