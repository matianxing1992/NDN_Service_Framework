"""Pure production planning kernels; no native import or GPU qualification.

The receiving host has not built _ndnsf yet. Execute verbatim AST-selected
methods with the real V3 value types, rather than fabricate a native module.
Full public import and network acceptance remain mandatory in Spec183 T008+.
"""
from __future__ import annotations

import ast
from dataclasses import replace
import hashlib
import json
import math
import os
from pathlib import Path
import re
from types import SimpleNamespace as NS

import pytest
from ndnsf_distributed_inference.sdk import placement as wire

ROOT = Path(__file__).resolve().parents[2] / 'NDNSF-DistributedInference/ndnsf_distributed_inference'
D = 'sha256:' + '1' * 64
ROLES = ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')


def kernels():
    if os.environ.get('SPEC183_REQUIRE_NATIVE_PLANNER_IMPORT') == '1':
        from ndnsf_distributed_inference.app_sdk.placement import AutomaticPlanningCoordinator
        from ndnsf_distributed_inference.planner.presplit_first import PreSplitFirstStrategy
        return AutomaticPlanningCoordinator, PreSplitFirstStrategy(at_ms=1)
    namespace = dict(vars(wire), replace=replace, hashlib=hashlib, json=json, math=math, re=re)
    nodes = [ast.ImportFrom(module='__future__', names=[ast.alias(name='annotations')], level=0)]
    tree = ast.parse((ROOT / 'app_sdk/placement.py').read_text())
    nodes += [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == '_v3_role_kind']
    for relative, name, methods in [
        ('app_sdk/placement.py', 'AutomaticPlanningCoordinator', {'_v3_role_specs', '_role_layer_range'}),
        ('planner/presplit_first.py', 'PreSplitFirstStrategy', {'propose_v3', '_v3_reuse_cost'}),
    ]:
        source = ast.parse((ROOT / relative).read_text())
        cls = next(node for node in source.body if isinstance(node, ast.ClassDef) and node.name == name)
        body = [node for node in cls.body if isinstance(node, ast.FunctionDef) and node.name in methods]
        assert len(body) == len(methods)
        nodes.append(ast.ClassDef(name=name, bases=[], keywords=[], body=body, decorator_list=[]))
    exec(compile(ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])),
                 '<verbatim-production-planning-kernels>', 'exec'), namespace)
    strategy = namespace['PreSplitFirstStrategy']()
    strategy.name, strategy.version, strategy.state_digest = 'kernel-test', '1', D
    return namespace['AutomaticPlanningCoordinator'], strategy


def candidate(backends):
    return NS(execution_plan=NS(roles=ROLES, dependencies=(), node_roles={str(i): r for i,r in enumerate(ROLES)}),
              requirements_by_role={r: NS(backends=backends, estimated_peak_gpu_memory_bytes=0) for r in ROLES},
              tensor_degrees_by_role={}, rank_artifact_digests_by_role={},
              artifacts_by_role={r:(D,) for r in ROLES}, role_state_inputs_by_role={},
              role_state_outputs_by_role={}, candidate_digest=D,
              model=NS(adapter=NS(name='yolo26n-onnx',version='1')),
              result_egress_role='Merge', merge_kind='', postprocessing={})


def views(gpu):
    result=[]
    for role in ROLES:
        cuda = gpu and role != 'Merge'
        devices = ('cuda:0',) if cuda else ()
        topology = wire.DeviceTopologyProfile('/provider/'+role, devices, 'cuda' if cuda else 'cpu')
        offer = wire.ProviderOfferV3(
            '/request/test', 1, '/YOLO', '/provider/'+role, D, D, True,
            wire.ExecutionDisposition.ACCEPT_WITH_PREPARATION, True, topology,
            resources=tuple(wire.DeviceResourceSnapshot(d,16000,12000,topology_digest=topology.digest()) for d in devices),
            accepted_roles=(role,), backends=('onnxruntime-cuda' if cuda else 'onnxruntime-cpu',),
            boot_epoch='boot-0001', captured_at_ms=1, expires_at_ms=100,
            signer_key_id='fixture', signature='fixture')
        result.append(wire.ProviderPlanningViewV3.from_offer(offer))
    return tuple(result)


@pytest.mark.parametrize('gpu', [False, True])
@pytest.mark.parametrize('backends', [('onnxruntime-cpu','onnxruntime-cuda'),
                                    ('onnxruntime-cuda','onnxruntime-cpu')])
def test_declared_cpu_cuda_alternatives_reach_real_v3_selection(backends, gpu):
    coordinator, strategy = kernels()
    roles = coordinator._v3_role_specs(candidate(backends))
    proposal = strategy.propose_v3(request_id='/request/test', attempt=1,
        model_digest=D, graph_digest=D, roles=roles, providers=views(gpu), ack_closed_digest=D)
    for role in proposal.roles:
        cuda = gpu and role.role != 'Merge'
        assert role.backend == ('onnxruntime-cuda' if cuda else 'onnxruntime-cpu')
        assert role.device_set == (('cuda:0',) if cuda else ())


def test_cpu_only_requirement_must_not_expand_to_cuda():
    coordinator, strategy = kernels()
    roles = coordinator._v3_role_specs(candidate(('onnxruntime-cpu',)))
    with pytest.raises(ValueError, match='no distinct feasible Provider'):
        strategy.propose_v3(request_id='/request/test', attempt=1, model_digest=D,
            graph_digest=D, roles=roles, providers=views(True), ack_closed_digest=D)


def test_cuda_only_requirement_does_not_admit_cpu_merge():
    coordinator, strategy = kernels()
    roles = coordinator._v3_role_specs(candidate(('onnxruntime-cuda',)))
    with pytest.raises(ValueError, match='Merge#0'):
        strategy.propose_v3(request_id='/request/test', attempt=1, model_digest=D,
            graph_digest=D, roles=roles, providers=views(True), ack_closed_digest=D)


@pytest.mark.parametrize('fault', ['missing-role', 'insufficient-memory', 'wrong-engine'])
def test_backend_alternatives_do_not_bypass_other_placement_constraints(fault):
    coordinator, strategy = kernels()
    roles = coordinator._v3_role_specs(candidate(('onnxruntime-cpu','onnxruntime-cuda')))
    providers = views(True)
    if fault == 'missing-role':
        providers = providers[1:]
    elif fault == 'insufficient-memory':
        roles = tuple(replace(role, required_device_memory_mb=16001) for role in roles)
    else:
        roles = tuple(replace(role, backend='tensorrt') for role in roles)
    with pytest.raises(ValueError, match='no distinct feasible Provider'):
        strategy.propose_v3(request_id='/request/test', attempt=1, model_digest=D,
            graph_digest=D, roles=roles, providers=providers, ack_closed_digest=D)
