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
    role = RoleAssemblySpec('BackboneNeck', 0, 0, 2, digest, digest, 'cpu', (),
        model_manifest_digest=digest, artifact_profile_digest=digest,
        graph_digest=digest, canonical_initializer_digest=digest,
        adapter_descriptor_digest=digest, assembler_descriptor_digest=digest,
        backend_abi='onnxruntime-cpu-test', node_indices=(0, 1),
        expected_inputs=({'name': 'input'},), expected_outputs=({'name': 'output'},),
        precision='float32', resource_envelope={'memory_mb': 1})
    flow = RoleDataflowContract('/request/1/', 2, digest, 'BackboneNeck',
        may_publish=(endpoint, endpoint2))
    return ProviderSelectionProjectionV3('/provider/a', '/request/1/', 2,
        digest, digest, digest, digest, digest, (role,), (), 5000,
        ExecutionRole('BackboneNeck', 'BackboneNeck', 0, 0, 2, 'cpu'),
        role, flow, DeviceBinding(DeviceBindingMode.CPU, '/provider/a',
            'BackboneNeck', digest, digest, digest, 1),
        group_capability_v1=b'PRIVATE-CAPABILITY'.hex())


def test_public_projection_uses_actual_typed_names_scopes_and_attempt():
    from ndnsf_distributed_inference.sdk.public_evidence import public_assignment_projection
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
        'sessionId', 'provider', 'role', 'inputs', 'outputs', 'model'}
    assert public['model'] == dict(modelManifestDigest=source.assembly.model_manifest_digest,
                                   artifactDigest=source.assembly.artifact_digest)


@pytest.mark.parametrize('field,value', [('attempt', 1), ('attempt', True),
    ('request_id', 'other'), ('plan_digest', 'sha256:'+'b'*64), ('provider', '/other')])
def test_public_projection_rejects_external_binding_mismatch(field, value):
    from ndnsf_distributed_inference.sdk.public_evidence import public_assignment_projection
    source = assignment()
    binding = dict(request_id=source.request_id, attempt=2,
        plan_digest=source.plan_digest, provider=source.provider)
    binding[field] = value
    with pytest.raises(ValueError):
        public_assignment_projection(source.to_bytes(), **binding)


def test_public_projection_rejects_missing_certified_model_identity(monkeypatch):
    from types import SimpleNamespace
    from ndnsf_distributed_inference.sdk.public_evidence import public_assignment_projection
    from ndnsf_distributed_inference.sdk.placement import ProviderSelectionProjectionV3
    source = assignment()
    class Incomplete:
        request_id = source.request_id
        attempt = source.attempt
        plan_digest = source.plan_digest
        provider = source.provider
        assembly = SimpleNamespace(model_manifest_digest='',
                                   artifact_digest=source.assembly.artifact_digest)
        dataflow = source.dataflow
        roles = source.roles
    monkeypatch.setattr(ProviderSelectionProjectionV3, 'from_bytes',
                        staticmethod(lambda _wire: Incomplete()))
    with pytest.raises(ValueError, match='PUBLIC_ASSIGNMENT_MODEL_IDENTITY'):
        public_assignment_projection(source.to_bytes(), request_id=source.request_id,
            attempt=2, plan_digest=source.plan_digest, provider=source.provider)


@pytest.mark.parametrize('mode,expected', [
    ('single', 'backbone-to-head'),
    ('redistribution', 'backbone-to-head/from/BackboneNeck'),
])
def test_native_scope_alternatives(mode, expected):
    from ndnsf_distributed_inference.sdk.public_evidence import public_assignment_projection
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
    from ndnsf_distributed_inference.sdk.public_evidence import public_assignment_projection
    source = assignment()
    with pytest.raises(ValueError):
        public_assignment_projection(wire, request_id=source.request_id,
            attempt=2, plan_digest=source.plan_digest, provider=source.provider)


@pytest.mark.parametrize('fault', ['none', 'disabled', 'role', 'provider', 'plan',
    'attempt', 'coverage', 'existing', 'symlink'])
def test_user_retains_only_bound_public_evidence(tmp_path, fault):
    """Actual User function and typed wire; handle is a boundary fixture."""
    import ast
    import os
    import re
    from types import SimpleNamespace as NS
    path = Path(__file__).resolve().parents[3] / 'examples/python/NDNSF-DistributedInference/yolo_2x2/user.py'
    tree = ast.parse(path.read_text())
    method = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                  and node.name == '_retain_public_assignments')
    namespace = dict(Path=Path, os=os, re=re, json=json)
    exec(compile(ast.Module(body=[method], type_ignores=[]), str(path), 'exec'), namespace)
    source = assignment()
    role = 'wrong' if fault == 'role' else source.dataflow.role
    plan = NS(providers_by_role={role: '/other' if fault == 'provider' else source.provider},
        assignment_payloads_by_role={role: source.to_bytes()})
    if fault == 'coverage':
        plan.assignment_payloads_by_role = {}
    handle = NS(sealed_plan=plan, execution_plan_digest='bad' if fault == 'plan' else source.plan_digest)
    args = NS(retain_public_assignments=fault != 'disabled', request_id=source.request_id,
        lifecycle_output_dir=str(tmp_path))
    target = tmp_path / 'yolo-public-assignments.json'
    if fault == 'existing':
        target.write_text('preserve')
    if fault == 'symlink':
        target.symlink_to(tmp_path / 'missing')
    invoke = lambda: namespace['_retain_public_assignments'](args, handle,
        'attempt-1' if fault == 'attempt' else 'attempt-2')
    if fault == 'none':
        invoke()
        record = json.loads(target.read_text())
        assert len(record['assignments']) == 1
        assert record['assignments'][0]['planDigest'] == source.plan_digest
        assert source.group_capability_v1 not in target.read_text()
        assert target.stat().st_mode & 0o777 == 0o600
    elif fault == 'disabled':
        invoke()
        assert not target.exists()
    else:
        with pytest.raises((ValueError, FileExistsError)):
            invoke()
        if fault == 'existing':
            assert target.read_text() == 'preserve'
        elif fault != 'symlink':
            assert not target.exists()


@pytest.mark.parametrize('fault', ['none', 'application-input', 'missing', 'scope', 'name', 'provider',
    'attempt', 'plan', 'role', 'duplicate', 'secret-field', 'symlink'])
def test_public_cross_role_join(tmp_path, fault):
    from ndnsf_distributed_inference.sdk.public_evidence import public_assignment_projection
    from runtime.yolo_result import read_public_dependency_contract
    source = assignment()
    if fault == 'application-input':
        endpoint = replace(source.dataflow.may_publish[0], source_kind='APPLICATION_INPUT',
            producer_namespace='/user', producer_role='', consumer_role='BackboneNeck',
            consumer_roles=('BackboneNeck',), group_id='application-input',
            operation='APPLICATION_INPUT', tensor_id='application-input', endpoint_digest='')
        source = replace(source, dataflow=replace(source.dataflow,
            must_fetch=(endpoint,), dataflow_digest=''))
    role = replace(source.assembly, role='DetectShard0')
    consumer = replace(source, provider='/provider/b', roles=(role,), assembly=role,
        execution_role=replace(source.execution_role, role_id='DetectShard0', stage_id='DetectShard0'),
        dataflow=RoleDataflowContract(source.request_id, 2, source.plan_digest,
            'DetectShard0', must_fetch=source.dataflow.may_publish),
        device_binding=replace(source.device_binding, provider='/provider/b', role='DetectShard0'))
    rows = [public_assignment_projection(p.to_bytes(), request_id=p.request_id,
        attempt=2, plan_digest=p.plan_digest, provider=p.provider) for p in (source, consumer)]
    if fault == 'missing': rows[1]['inputs'].pop()
    if fault == 'scope': rows[1]['inputs'][0]['scope'] = 'wrong'
    if fault == 'name': rows[1]['inputs'][0]['planned_name'] = '/wrong'
    if fault == 'provider': rows[1]['provider'] = '/wrong'
    if fault == 'attempt': rows[1]['attempt'] = 1
    if fault == 'plan': rows[1]['planDigest'] = 'sha256:'+'b'*64
    if fault == 'role': rows[1]['role'] = 'wrong'
    if fault == 'duplicate': rows.append(rows[0])
    if fault == 'secret-field': rows[0]['group_capability_v1'] = 'secret'
    path = tmp_path/'public.json'
    path.write_text(json.dumps(dict(schema='yolo-public-assignments-v2', assignments=rows)))
    if fault == 'symlink':
        link = tmp_path/'link.json'
        link.symlink_to(path)
        path = link
    def run():
        return read_public_dependency_contract(path, request_id=source.request_id,
            attempt=2, plan_digest=source.plan_digest,
            providers_by_role={'BackboneNeck': '/provider/a', 'DetectShard0': '/provider/b'})
    if fault in ('none', 'application-input'):
        contract = run()
        assert len(contract['edges']) == 2
        assert contract['sessionId'] == 'request/1/attempt/2'
        assert contract['modelBindings']['BackboneNeck'] == rows[0]['model']
        assert len(contract['applicationInputs']) == int(fault == 'application-input')
        # Pair against source-format logs; these are not a transport run.
        from runtime.yolo_result import collect_dependency_result
        logs = {}
        for role_name, direction in [('BackboneNeck', 'publish'), ('DetectShard0', 'fetch')]:
            lines = []
            for edge in contract['edges']:
                event = dict(session=contract['sessionId'], **edge,
                    direction=direction+'-ndnsf-data-v1', payload_bytes='123', status='ok')
                lines.append('NDNSF_DI_DEPENDENCY_OBJECT ' + ' '.join(k+'='+v for k,v in event.items()))
            logs[role_name] = tmp_path/(role_name+'.log')
            logs[role_name].write_text('\n'.join(lines)+'\n')
        def collect():
            return collect_dependency_result(path, logs, request_id=source.request_id,
                attempt=2, plan_digest=source.plan_digest,
                providers_by_role={'BackboneNeck': '/provider/a', 'DetectShard0': '/provider/b'})
        result = collect()
        assert result['edgeCount'] == 2
        assert result['applicationInputCount'] == int(fault == 'application-input')
        logs['DetectShard0'].write_text(logs['DetectShard0'].read_text().replace('payload_bytes=123', 'payload_bytes=124'))
        with pytest.raises(ValueError):
            collect()
    else:
        with pytest.raises(ValueError): run()
