"""Source-shaped device fields; not a real GPU allocation probe."""
from pathlib import Path
import sys
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_result import validate_device_binding

UUID = 'GPU-12345678-1234-1234-1234-123456789abc'


@pytest.mark.parametrize('selector', ['0', '3', UUID, UUID.upper()])
@pytest.mark.parametrize('fault', ['none', 'uuid', 'list', 'selector', 'ordinal', 'source', 'missing-device'])
def test_runtime_gpu_identity_matches_allocation_and_launch(selector, fault):
    row = dict(runnerKind='onnxruntime-cuda', device={'kind': 'cuda', 'id': '0'},
        gpuUuid=UUID, gpuUuids=[UUID], cudaVisibleDevices=selector,
        gpuIdentitySource='cuda-runtime-pci+driver-uuid')
    if fault == 'uuid': row['gpuUuid'] = 'GPU-'+ '0'*36
    if fault == 'list': row['gpuUuids'] = [UUID, UUID]
    if fault == 'selector': row['cudaVisibleDevices'] = '0,1'
    if fault == 'ordinal': row['device']['id'] = '1'
    if fault == 'source': row['gpuIdentitySource'] = 'environment-variable'
    if fault == 'missing-device': row.pop('device')
    if fault == 'none':
        assert validate_device_binding(row, expected_uuid=UUID, expected_visible=selector)['gpuUuid'] == UUID
    else:
        with pytest.raises(ValueError): validate_device_binding(row, expected_uuid=UUID, expected_visible=selector)


@pytest.mark.parametrize('runner', ['onnxruntime-cpu', 'native-yolo-postprocess'])
@pytest.mark.parametrize('exposed', [False, True])
def test_cpu_roles_do_not_claim_gpu(runner, exposed):
    row = dict(runnerKind=runner, gpuUuid='', cudaVisibleDevices=UUID if exposed else '')
    if exposed:
        with pytest.raises(ValueError): validate_device_binding(row)
    else:
        assert validate_device_binding(row)['qualification'] == 'CPU_DEVICE_COMPONENT_ONLY'


@pytest.mark.parametrize('uuid,visible', [(None, '0'), (UUID, None), (UUID, '0,1'),
    (UUID, 'GPU-00000000-0000-0000-0000-000000000000')])
def test_missing_or_contradictory_expected_allocation_rejected(uuid, visible):
    with pytest.raises(ValueError):
        validate_device_binding({'runnerKind': 'onnxruntime-cuda'}, expected_uuid=uuid, expected_visible=visible)
