"""Real typed Selection wire, not a native execution qualification."""
from dataclasses import replace
from pathlib import Path
import json
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'NDNSF-DistributedInference'))
from ndnsf_distributed_inference.sdk.placement import (
    DeviceBinding, DeviceBindingMode, ExecutionRole, ProviderSelectionProjectionV3,
    RoleAssemblySpec, RoleDataflowContract, TensorEndpoint,
)


def assignment():
    digest = 'sha256:' + 'a' * 64
    endpoint = TensorEndpoint('/provider/a', '/user', '/request/1/', 2, digest,
        'backbone-to-head', 'attempt-2', 'PIPELINE', 0, 'ROLE',
        'BackboneNeck', 0, 'DetectShard0', 'p3', digest, digest, 0, 1,
        digest, 'NDNSF_DATA_V1', 1000, 5000)
    endpoint2 = replace(endpoint, tensor_id='p4', endpoint_digest='')
    role = RoleAssemblySpec('BackboneNeck', 0, 0, 2, digest, digest, 'cpu', ())
    flow = RoleDataflowContract('/request/1/', 2, digest, 'BackboneNeck',
        may_publish=(endpoint, endpoint2))
    return ProviderSelectionProjectionV3('/provider/a', '/request/1/', 2,
        digest, digest, digest, digest, digest, (role,), (), 5000,
        ExecutionRole('BackboneNeck', 'BackboneNeck', 0, 0, 2, 'cpu'),
        role, flow, DeviceBinding(DeviceBindingMode.CPU, '/provider/a',
            'BackboneNeck', digest, digest, digest, 1),
        group_capability_v1=b'PRIVATE-CAPABILITY'.hex())


def test_public_projection_uses_actual_typed_names_scopes_and_attempt():
    from runtime.yolo_projection import public_assignment_projection
    source = assignment()
    public = public_assignment_projection(source.to_bytes(), request_id=source.request_id,
        attempt=2, plan_digest=source.plan_digest, provider=source.provider)
    assert public['sessionId'] == 'request/1/attempt/2'
    assert [edge['scope'] for edge in public['outputs']] == [
        'backbone-to-head/from/BackboneNeck/tensor/p3',
        'backbone-to-head/from/BackboneNeck/tensor/p4']
    assert public['outputs'][0]['planned_name'] == source.dataflow.may_publish[0].name_prefix
    assert 'PRIVATE-CAPABILITY' not in json.dumps(public)
    assert source.group_capability_v1 not in json.dumps(public)
    assert set(public) == {'schema', 'requestId', 'attempt', 'planDigest',
        'sessionId', 'provider', 'role', 'inputs', 'outputs'}


@pytest.mark.parametrize('field,value', [('attempt', 1), ('attempt', True),
    ('request_id', 'other'), ('plan_digest', 'sha256:'+'b'*64), ('provider', '/other')])
def test_public_projection_rejects_external_binding_mismatch(field, value):
    from runtime.yolo_projection import public_assignment_projection
    source = assignment()
    binding = dict(request_id=source.request_id, attempt=2,
        plan_digest=source.plan_digest, provider=source.provider)
    binding[field] = value
    with pytest.raises(ValueError):
        public_assignment_projection(source.to_bytes(), **binding)


@pytest.mark.parametrize('mode,expected', [
    ('single', 'backbone-to-head'),
    ('redistribution', 'backbone-to-head/from/BackboneNeck'),
])
def test_native_scope_alternatives(mode, expected):
    from runtime.yolo_projection import public_assignment_projection
    source = assignment()
    if mode == 'single':
        flow = replace(source.dataflow, may_publish=source.dataflow.may_publish[:1], dataflow_digest='')
        source = replace(source, dataflow=flow)
    else:
        source = replace(source, dependencies=({'key_scope': 'backbone-to-head',
            'redistributions': ({'operation': 'GATHER'},)},))
    public = public_assignment_projection(source.to_bytes(), request_id=source.request_id,
        attempt=2, plan_digest=source.plan_digest, provider=source.provider)
    assert public['outputs'][0]['scope'] == expected


@pytest.mark.parametrize('wire', [b'{}', b'not-json', b'x' * (1024 * 1024 + 1)])
def test_reject_invalid_selection_wire(wire):
    from runtime.yolo_projection import public_assignment_projection
    source = assignment()
    with pytest.raises(ValueError):
        public_assignment_projection(wire, request_id=source.request_id,
            attempt=2, plan_digest=source.plan_digest, provider=source.provider)
