"""CUDA ABI doubles and real launcher/CLI boundaries, not GPU qualification."""
import ctypes as ct
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_gpu_probe as probe

UUID = 'GPU-12345678-1234-1234-1234-123456789abc'
NONCE = 'a'*64


@pytest.mark.parametrize('fault', ['none', 'count', 'count-error', 'pci', 'init', 'device', 'uuid', 'zero'])
def test_cuda_abi_resolves_runtime_ordinal_via_pci(monkeypatch, fault):
    def count(ptr):
        ptr._obj.value = 2 if fault == 'count' else 1
        return 1 if fault == 'count-error' else 0
    def pci(buf, size, ordinal):
        assert size == 32 and ordinal == 0
        buf.value = b'0000:65:00.0'
        return 1 if fault == 'pci' else 0
    def init(flags):
        assert flags == 0
        return 1 if fault == 'init' else 0
    def device(ptr, bus):
        assert bus.value == b'0000:65:00.0'
        ptr._obj.value = 7  # Deliberately NOT runtime ordinal zero.
        return 1 if fault == 'device' else 0
    def uuid(ptr, dev):
        assert dev == 7
        raw = bytes.fromhex(UUID[4:].replace('-', '')) if fault != 'zero' else bytes(16)
        ct.memmove(ptr, raw, 16)
        return 1 if fault == 'uuid' else 0
    runtime = SimpleNamespace(cudaGetDeviceCount=count, cudaDeviceGetPCIBusId=pci)
    driver = SimpleNamespace(cuInit=init, cuDeviceGetByPCIBusId=device, cuDeviceGetUuid=uuid)
    monkeypatch.setattr(probe.ct, 'CDLL', lambda name: runtime if name.startswith('libcudart') else driver)
    if fault == 'none': assert probe.query_cuda_uuid() == UUID
    else:
        with pytest.raises(ValueError): probe.query_cuda_uuid()


@pytest.mark.parametrize('fault', ['none', 'nonce', 'selector', 'uuid', 'source', 'extra', 'duplicate', 'zero'])
def test_probe_record_is_bound_and_not_allocation_attestation(fault):
    row = dict(schema='tiger-cuda-device-v1', nonce=NONCE, visible='3', uuid=UUID,
        source='cuda-runtime-pci+driver-uuid')
    if fault == 'nonce': row['nonce'] = 'b'*64
    if fault == 'selector': row['visible'] = '0'
    if fault == 'uuid': row['uuid'] = 'from-env'
    if fault == 'source': row['source'] = 'nvidia-smi-index'
    if fault == 'extra': row['pass'] = True
    if fault == 'zero': row['uuid'] = 'GPU-00000000-0000-0000-0000-000000000000'
    payload = (probe.MARKER+json.dumps(row)+'\n').encode()
    if fault == 'duplicate': payload *= 2
    if fault == 'none': assert probe.read_probe(payload, nonce=NONCE, visible='3') == dict(uuid=UUID, visible='3')
    else:
        with pytest.raises(ValueError): probe.read_probe(payload, nonce=NONCE, visible='3')


def test_actual_cli_rejects_missing_selector_before_cuda():
    env = dict(os.environ, CUDA_VISIBLE_DEVICES='')
    completed = subprocess.run([sys.executable, str(Path(probe.__file__)), '--nonce', NONCE],
        env=env, capture_output=True, text=True, timeout=5)
    assert completed.returncode == 2 and probe.MARKER not in completed.stdout


def test_worker_probe_uses_candidate_container_and_cleans_finite_process(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    from test_yolo_worker import prepared
    inputs = prepared(tmp_path)
    launcher = Path(inputs['profile']['apptainer'])
    # The OS boundary emits a synthetic device. It does not load CUDA.
    launcher.write_text('#!/usr/bin/python3\nimport json,sys\n'
        'args=sys.argv[1:]\nnonce=args[args.index("--nonce")+1]\n'
        'assert "--nv" in args and "CUDA_VISIBLE_DEVICES=0" in args\n'
        'assert "runtime.yolo_gpu_probe" in args\n'
        'print("TIGER_CUDA_DEVICE_OBSERVED "+json.dumps(dict(schema="tiger-cuda-device-v1",'
        'nonce=nonce,visible="0",uuid="'+UUID+'",source="cuda-runtime-pci+driver-uuid")))\n')
    state = NodeRuntime(**inputs)
    try:
        binding = state.probe_gpu_device(seconds=3)
        assert binding == dict(uuid=UUID, visible='0')
        receipt = json.loads((state.output/'gpu-probe.json').read_text())
        assert receipt['binding'] == binding and receipt['nonce'] == state.gpu_probe['nonce']
        assert receipt['qualification'] == 'CUDA_VISIBILITY_COMPONENT_ONLY'
        launch, = state.launches
        assert launch['invocation'] == 'gpu-device' and launch['pid'] > 0
        with pytest.raises(ValueError): state.probe_gpu_device(seconds=3)
    finally:
        rows = state.close()
    assert len(rows) == 1 and rows[0]['reaped'] and not rows[0]['forced']


def test_worker_does_not_accept_failed_probe_even_if_it_prints_valid_json(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    from test_yolo_worker import prepared
    inputs = prepared(tmp_path)
    launcher = Path(inputs['profile']['apptainer'])
    launcher.write_text('#!/usr/bin/python3\nimport sys,json\n'
        'nonce=sys.argv[sys.argv.index("--nonce")+1]\n'
        'print("TIGER_CUDA_DEVICE_OBSERVED "+json.dumps(dict(schema="tiger-cuda-device-v1",'
        'nonce=nonce,visible="0",uuid="'+UUID+'",source="cuda-runtime-pci+driver-uuid")))\n'
        'sys.exit(7)\n')
    state = NodeRuntime(**inputs)
    try:
        with pytest.raises(RuntimeError): state.probe_gpu_device(seconds=3)
        assert state.gpu_probe is None and not (state.output/'gpu-probe.json').exists()
    finally:
        rows = state.close()
    assert rows[0]['exitCode'] == 7 and rows[0]['reaped']
