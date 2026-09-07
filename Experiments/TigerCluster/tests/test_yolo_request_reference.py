"""Real DI assembler and CPU ORT; publication is a declared boundary fixture."""
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import sys
import time
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'NDNSF-DistributedInference'))
sys.path.insert(0, str(ROOT / 'Experiments/TigerCluster'))
from ndnsf_distributed_inference.adapters.onnx.executor import CertifiedOnnxAssemblyRecipe
from ndnsf_distributed_inference.sdk.placement import RoleAssemblySpec
from runtime.yolo_graph_reference import (RequestReferenceBinding, read_request_reference,
                                         validate_certified_graph_provenance)


def fixture(tmp_path):
    cases = json.loads((ROOT / 'tests/fixtures/spec181/assembly-vectors-v1.json').read_text())['cases']
    case = next(c for c in cases if c['id'] == 'external-component')
    base = RoleAssemblySpec(**case['role'])
    source, weights = bytes.fromhex(case['canonicalModelHex']), bytes.fromhex(case['initializerHex'])
    package, output = tmp_path / 'package', tmp_path / 'output'
    (package / 'canonical').mkdir(parents=True)
    output.mkdir(mode=0o700)
    (package / 'canonical/yolo26n.onnx').write_bytes(source)
    (package / 'weights.bin').write_bytes(weights)
    sha = lambda b: 'sha256:' + hashlib.sha256(b).hexdigest()
    (package / 'manifest.json').write_text(json.dumps(dict(
        graph={'graphDigest': sha(source)}, weights={'path': 'weights.bin', 'digest': sha(weights)})))
    names = ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')
    specs = tuple(replace(base, role=role, artifact_digest=sha(role.encode())) for role in names)
    candidate = SimpleNamespace(candidate_digest='sha256:'+'c'*64,
        graph_digest='sha256:'+'d'*64,
        fragments_by_role={s.role: s.artifact_digest for s in specs})

    class Publisher:
        model_manifest_digest = base.model_manifest_digest
        called = False
        def describe(self, selected):
            assert selected is candidate
            return SimpleNamespace(graph_digest=candidate.graph_digest,
                                   model_manifest_digest=self.model_manifest_digest)
        def ensure(self, selected, roles, *, deadline_ms):
            assert selected is candidate and roles == specs
            assert not (output / 'graph-reference.json').exists()
            self.called = True
            self.model_manifest_digest = 'sha256:'+'e'*64
            return SimpleNamespace(candidate_digest=candidate.candidate_digest)

    publisher = Publisher()
    wrapper = RequestReferenceBinding(publisher, package=package, output=output,
        backend='CPUExecutionProvider', run_id='reference-test', request_id='/request/1',
        runtime_candidate_digest='sha256:'+'f'*64)
    return wrapper, publisher, specs, candidate, case


def test_real_assembler_reference_binds_post_publication_manifest(tmp_path):
    wrapper, publisher, specs, candidate, case = fixture(tmp_path)
    for spec in specs:
        assert CertifiedOnnxAssemblyRecipe.from_role_spec(spec).digest == CertifiedOnnxAssemblyRecipe(**case['recipe']).digest
    published = wrapper.ensure(candidate, specs, deadline_ms=int(time.time()*1000)+10000)
    assert publisher.called and published.candidate_digest == candidate.candidate_digest
    record = json.loads((wrapper.output / 'graph-reference.json').read_text())
    assert record['requestId'] == '/request/1'
    assert record['runtimeCandidateDigest'] == 'sha256:'+'f'*64
    graph = record['certifiedGraph']
    assert set(graph['roles']) == {'BackboneNeck', 'DetectShard0', 'DetectShard1'}
    for role, value in graph['roles'].items():
        assert value['modelManifestDigest'] == publisher.model_manifest_digest
        assert value['modelManifestDigest'] != specs[0].model_manifest_digest
        assert value['artifactDigest'] == candidate.fragments_by_role[role]
        assert graph['referenceProvenance'][role]['assembledModelDigest'] == case['expected']['modelDigest']
    validate_certified_graph_provenance(graph)
    expected = dict(run_id='reference-test', request_id='/request/1',
        runtime_candidate_digest='sha256:'+'f'*64,
        placement_candidate_digest=candidate.candidate_digest, graph_digest=candidate.graph_digest)
    path = wrapper.output / 'graph-reference.json'
    assert read_request_reference(path, **expected) == graph
    # Reuse the same independently assembled record: identity checks do not
    # require another ORT initialization or inference campaign.
    for key, wrong in [('run_id', 'other'), ('request_id', '/request/2'),
                       ('runtime_candidate_digest', 'sha256:'+'0'*64),
                       ('placement_candidate_digest', 'sha256:'+'0'*64),
                       ('graph_digest', 'sha256:'+'0'*64)]:
        with pytest.raises(ValueError, match='REQUEST_REFERENCE_IDENTITY|REQUEST_REFERENCE_GRAPH'):
            read_request_reference(path, **dict(expected, **{key: wrong}))
    assert [p.name for p in wrapper.output.iterdir()] == ['graph-reference.json']
    with pytest.raises(ValueError, match='REQUEST_REFERENCE_REUSED'):
        wrapper.ensure(candidate, specs, deadline_ms=int(time.time()*1000)+10000)


@pytest.mark.parametrize('fault', ['artifact', 'missing-role', 'source', 'deadline', 'recipe'])
def test_reference_failure_prevents_publication_and_receipt(tmp_path, fault):
    wrapper, publisher, specs, candidate, _ = fixture(tmp_path)
    deadline = int(time.time()*1000)+10000
    if fault == 'artifact':
        specs = (replace(specs[0], artifact_digest='sha256:'+'0'*64), *specs[1:])
    elif fault == 'missing-role':
        specs = specs[:-1]
    elif fault == 'source':
        (wrapper.package / 'canonical/yolo26n.onnx').write_bytes(b'corrupt')
    elif fault == 'deadline':
        deadline = 1
    else:
        specs = (replace(specs[0], recipe_digest='sha256:'+'0'*64), *specs[1:])
    with pytest.raises((ValueError, TimeoutError)):
        wrapper.ensure(candidate, specs, deadline_ms=deadline)
    assert not publisher.called and not list(wrapper.output.iterdir())
