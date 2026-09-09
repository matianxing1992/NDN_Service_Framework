"""Role output binding tests; synthetic logs/profiles are not inference."""
import json
from pathlib import Path
import sys

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_result as result
from runtime.yolo_worker import NodeRuntime
from test_yolo_worker import prepared
from test_yolo_native_observation import observation


def test_profile_path_is_scoped_to_owner(tmp_path):
    root = tmp_path / 'BackboneNeck'
    (root / 'ort').mkdir(parents=True)
    path = root / 'ort/session.json'
    path.write_text('[]')
    assert result.resolve_role_output(root, '/output/ort/session.json') == path


@pytest.mark.parametrize('name', ['/tmp/profile.json', '/output/../other/profile.json',
    '/output//ort/profile.json', '/output/./ort/profile.json', '/output',
    '/output/ort\\profile.json', '/output/missing.json'])
def test_invalid_container_path_rejected(tmp_path, name):
    with pytest.raises(result.EvidenceError):
        result.resolve_role_output(tmp_path, name)


def test_profile_cannot_follow_peer_output_symlink(tmp_path):
    (tmp_path / 'peer').mkdir()
    (tmp_path / 'peer/profile.json').write_text('[]')
    (tmp_path / 'owner').mkdir()
    (tmp_path / 'owner/ort').symlink_to(tmp_path / 'peer', target_is_directory=True)
    with pytest.raises(result.EvidenceError):
        result.resolve_role_output(tmp_path / 'owner', '/output/ort/profile.json')


@pytest.mark.parametrize('mode,role', [('local-cpu', 'BackboneNeck'),
    ('two-node-gpu', 'BackboneNeck'), ('two-node-gpu', 'Merge')])
def test_real_launcher_pid_and_output_join(tmp_path, mode, role):
    worker = NodeRuntime(**prepared(tmp_path, mode=mode))
    row = observation()
    row.update(roles=[role], profileRequestId='/app/request/1',
               providerProfilePath='/output/ort/profile.json')
    row['artifactDigests'] = {role: 'sha256:'+'b'*64}
    if role == 'Merge':
        row.update(runnerKind='native-yolo-postprocess', realCompute='false',
                   loadCompleted='false', warmupCompleted='false',
                   nodeProviderAssignments='', gpuUuid='', cudaVisibleDevices='')
    else:
        backend = 'CPUExecutionProvider' if mode == 'local-cpu' else 'CUDAExecutionProvider'
        row['runnerKind'] = 'onnxruntime-cpu' if mode == 'local-cpu' else 'onnxruntime-cuda'
        row['nodeProviderAssignments'][0].update(provider=backend, nodeName='conv_kernel_time')
        path = worker.output / role / 'ort'
        path.mkdir(parents=True)
        (path / 'profile.json').write_text(json.dumps([dict(cat='Node', name='conv_kernel_time',
                                                         args={'provider': backend})]))
    prefix = 'NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED '
    code = ('import json,os,sys,time; row=json.loads(sys.argv[1]); '
            'row["processId"]=str(os.getpid()); '
            'print("NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED "+json.dumps(row), flush=True); time.sleep(60)')
    try:
        child = worker.start_service(role, [sys.executable, '-c', code, json.dumps(row)])
        worker.wait_marker(role, prefix, seconds=3)
    finally:
        rows = worker.close()
    result.validate_worker_cleanup(worker, rows)
    evidence = result.collect_role_execution(worker, role=role, provider='/app/worker-a',
        request_id='/app/request/1', attempt=1, plan_digest='sha256:'+'1'*64)
    assert evidence['native']['observation']['processId'] == child.pid
    assert (evidence['profile'] is None) == (role == 'Merge')
    assert evidence['qualification'] == 'ROLE_EXECUTION_COMPONENT_ONLY'
