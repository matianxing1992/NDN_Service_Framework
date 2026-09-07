"""Native argv wiring at the process boundary; not model/SIF qualification."""
import subprocess
import sys

import pytest

from test_yolo_worker import prepared
from runtime.yolo_worker import NodeRuntime


def application_inputs(tmp_path, rank):
    inputs = prepared(tmp_path, rank=rank)
    for name in ('native-execution-plan.json', 'service-manifest.json', 'trust-schema.conf'):
        (inputs['public'] / name).write_text('test fixture, not a qualified document')
    for home in inputs['homes'].values():
        (home / 'offer.pem').write_text('test fixture, not a key')
    return inputs


@pytest.mark.parametrize('cpu', [False, True])
@pytest.mark.parametrize('role,rank', [('BackboneNeck', 0), ('Merge', 0),
                                    ('DetectShard0', 1), ('DetectShard1', 1)])
def test_native_provider_argv_reaches_process_boundary(tmp_path, monkeypatch, role, rank, cpu):
    inputs = application_inputs(tmp_path, rank)
    if cpu:
        from runtime.yolo_worker import assigned_roles
        inputs.update(mode='local-cpu', rank=0, gpu_device=None)
        for name in assigned_roles('local-cpu', 0):
            if name not in inputs['homes']:
                home = tmp_path / 'private' / name
                (home / '.ndn/ndnsec-key-file').mkdir(parents=True)
                (home / '.ndn/pib.db').write_bytes(b'not-a-pib-layout-fixture')
                (home / '.ndn/ndnsec-key-file/key.privkey').write_bytes(b'not-a-private-key')
                inputs['homes'][name] = home
                (home / 'offer.pem').write_text('fixture')
        inputs['homes'].pop('nfd1', None)
    worker = NodeRuntime(**inputs)
    original = subprocess.Popen
    observed = []

    def boundary(argv, **kwargs):
        observed.append(argv)
        return original([sys.executable, '-c', 'import time; time.sleep(60)'], **kwargs)

    monkeypatch.setattr(subprocess, 'Popen', boundary)
    try:
        worker.start_provider(role, identity='/run/actual/' + role,
                              service='/Detection/YOLO', group='/run/sync',
                              controller='/run/controller', permission_wait_ms=12000)
        argv, = observed
        assert argv[argv.index('--provider') + 1] == '/run/actual/' + role
        assert argv[argv.index('--roles') + 1] == role
        assert argv[argv.index('--artifact-cache-dir') + 1] == '/output/artifact-cache'
        assert argv[argv.index('--selection-offer-key-file') + 1] == '/identities/' + role + '/offer.pem'
        assert argv[argv.index('--permission-wait-ms') + 1] == '12000'
        assert argv[argv.index('--offer-device') + 1] == ('cpu' if cpu or role == 'Merge' else 'cuda:0')
        assert ('--nv' in argv) == (not cpu and role != 'Merge')
        assert '--offer-has-model' not in argv
        assert not any(':/artifacts:' in arg for arg in argv)
    finally:
        assert all(row['reaped'] for row in worker.close())


@pytest.mark.parametrize('fault', ['missing-plan', 'missing-key', 'foreign-key',
                                  'wrong-role', 'bad-name', 'bool-budget', 'long-budget'])
def test_provider_rejects_bad_inputs_before_process_or_lease(tmp_path, fault):
    inputs = application_inputs(tmp_path, 0)
    worker = NodeRuntime(**inputs)
    options = dict(role='BackboneNeck', identity='/run/provider', service='/YOLO',
                   group='/run/sync', controller='/run/controller', permission_wait_ms=12000)
    if fault == 'missing-plan':
        (inputs['public'] / 'native-execution-plan.json').unlink()
    elif fault in ('missing-key', 'foreign-key'):
        key = inputs['homes']['BackboneNeck'] / 'offer.pem'
        key.unlink()
        if fault == 'foreign-key':
            key.symlink_to(inputs['homes']['Merge'] / 'offer.pem')
    elif fault == 'wrong-role':
        options['role'] = 'DetectShard0'
    elif fault == 'bad-name':
        options['identity'] = '/bad\nname'
    elif fault == 'bool-budget':
        options['permission_wait_ms'] = True
    else:
        options['permission_wait_ms'] = 120001
    try:
        with pytest.raises(ValueError, match='WORKER_'):
            worker.start_provider(**options)
        assert worker.launches == [] and worker.leases == {}
    finally:
        worker.close()
