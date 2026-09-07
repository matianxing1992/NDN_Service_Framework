"""Independent CUDA identity probe, executed inside the candidate SIF.

No inference and no Slurm allocation attestation. The task launcher must bind
this owned subprocess to its actual allocation and preserve its output. CUDA
runtime ordinal zero is resolved via PCI, never via an NVML list index.
"""
import argparse
import ctypes as ct
import json
import os
import re

UUID = r'GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}'
SELECTOR = r'(?:0|[1-9][0-9]*|' + UUID + r')'
MARKER = 'TIGER_CUDA_DEVICE_OBSERVED '


def query_cuda_uuid():
    """Measure one runtime-visible CUDA device through the CUDA 12 ABI."""
    try:
        runtime = ct.CDLL('libcudart.so.12')
    except OSError:
        runtime = ct.CDLL('libcudart.so')
    driver = ct.CDLL('libcuda.so.1')
    def function(library, name, arguments):
        fn = getattr(library, name)
        fn.argtypes, fn.restype = arguments, ct.c_int
        return fn
    count_fn = function(runtime, 'cudaGetDeviceCount', [ct.POINTER(ct.c_int)])
    pci_fn = function(runtime, 'cudaDeviceGetPCIBusId', [ct.c_char_p, ct.c_int, ct.c_int])
    init_fn = function(driver, 'cuInit', [ct.c_uint])
    by_pci_fn = function(driver, 'cuDeviceGetByPCIBusId', [ct.POINTER(ct.c_int), ct.c_char_p])
    uuid_fn = function(driver, 'cuDeviceGetUuid', [ct.c_void_p, ct.c_int])
    count = ct.c_int()
    if count_fn(ct.byref(count)) != 0 or count.value != 1:
        raise ValueError('CUDA_PROBE_REQUIRES_ONE_VISIBLE_DEVICE')
    pci = ct.create_string_buffer(32)
    device = ct.c_int(-1)
    value = (ct.c_ubyte * 16)()
    if (pci_fn(pci, len(pci), 0) != 0 or not pci.value or init_fn(0) != 0
            or by_pci_fn(ct.byref(device), pci) != 0 or device.value < 0
            or uuid_fn(ct.byref(value), device.value) != 0 or not any(value)):
        raise ValueError('CUDA_PROBE_IDENTITY_FAILED')
    raw = bytes(value).hex()
    return 'GPU-' + '-'.join((raw[:8], raw[8:12], raw[12:16], raw[16:20], raw[20:]))


def read_probe(payload, *, nonce, visible):
    """Validate the bounded output from an owned, successfully reaped probe."""
    if (not isinstance(nonce, str) or re.fullmatch(r'[0-9a-f]{64}', nonce) is None
            or not isinstance(visible, str) or re.fullmatch(SELECTOR, visible) is None
            or not isinstance(payload, bytes) or len(payload) > 8192):
        raise ValueError('CUDA_PROBE_EXPECTED_BINDING')
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('CUDA_PROBE_DUPLICATE_FIELD')
            result[key] = value
        return result
    lines = [line for line in payload.decode('utf-8').splitlines() if line.startswith(MARKER)]
    if len(lines) != 1:
        raise ValueError('CUDA_PROBE_RECORD_COUNT')
    row = json.loads(lines[0][len(MARKER):], object_pairs_hook=pairs)
    if (not isinstance(row, dict) or set(row) != {'schema', 'nonce', 'visible', 'uuid', 'source'}
            or row['schema'] != 'tiger-cuda-device-v1' or row['nonce'] != nonce
            or row['visible'] != visible or row['source'] != 'cuda-runtime-pci+driver-uuid'
            or not isinstance(row['uuid'], str) or re.fullmatch(UUID, row['uuid']) is None
            or row['uuid'].lower() == 'gpu-00000000-0000-0000-0000-000000000000'
            or (visible.startswith('GPU-') and visible.lower() != row['uuid'].lower())):
        raise ValueError('CUDA_PROBE_RECORD_BINDING')
    return dict(uuid='GPU-'+row['uuid'][4:].lower(), visible=visible)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nonce', required=True)
    args = parser.parse_args(argv)
    visible = os.environ.get('CUDA_VISIBLE_DEVICES', '')
    if re.fullmatch(r'[0-9a-f]{64}', args.nonce) is None or re.fullmatch(SELECTOR, visible) is None:
        parser.error('one explicit CUDA selector and a 64-hex nonce are required')
    row = dict(schema='tiger-cuda-device-v1', nonce=args.nonce, visible=visible,
        uuid=query_cuda_uuid(), source='cuda-runtime-pci+driver-uuid')
    payload = MARKER + json.dumps(row, sort_keys=True)
    read_probe(payload.encode(), nonce=args.nonce, visible=visible)
    print(payload, flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
