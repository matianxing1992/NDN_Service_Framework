"""Production identity resolver kernel; native adapter import remains T008."""
import ast
from pathlib import Path
from types import SimpleNamespace as NS

import pytest

SOURCE = Path(__file__).resolve().parents[2] / 'NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/adapter.py'


def resolver():
    tree = ast.parse(SOURCE.read_text())
    cls = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'Yolo26Splitter')
    method = next(n for n in cls.body if isinstance(n, ast.FunctionDef)
                  and n.name == 'describe_candidate_identity')
    namespace = {}
    module = ast.Module(body=[ast.ImportFrom(module='__future__', names=[ast.alias(name='annotations')], level=0), method], type_ignores=[])
    exec(compile(ast.fix_missing_locations(module), str(SOURCE), 'exec'), namespace)
    return namespace[method.name]


def test_selected_runtime_candidate_resolves_registered_identity():
    a = NS(candidate_id='atomic-v1', candidate_digest='catalogue-a')
    b = NS(candidate_id='shared-backbone-two-shard-v1', candidate_digest='catalogue-b')
    calls = []
    def convert(model, graph, registered):
        calls.append((model, graph, registered))
        return NS(candidate_digest='runtime-' + registered.candidate_digest)
    owner = NS(registered=(a, b), _candidate=convert)
    assert resolver()(owner, 'model', 'graph', NS(candidate_digest='runtime-catalogue-b')) == (
        'shared-backbone-two-shard-v1', 'catalogue-b')
    assert len(calls) == 2


@pytest.mark.parametrize('matches', [0, 2])
def test_unknown_or_ambiguous_conversion_fails_closed(matches):
    owner = NS(registered=tuple(NS(candidate_id=str(i), candidate_digest=str(i)) for i in range(matches)),
               _candidate=lambda *a: NS(candidate_digest='runtime'))
    with pytest.raises(ValueError, match='missing or ambiguous'):
        resolver()(owner, None, None, NS(candidate_digest='runtime'))


def test_v3_emitter_uses_resolver_without_mutating_runtime_candidate():
    path = SOURCE.parents[2] / 'app_sdk/placement.py'
    tree = ast.parse(path.read_text())
    # Execute the verbatim production block between candidate resolution and
    # placement emission. Native planner execution remains a separate gate.
    parent = next(n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef)
                  and any(isinstance(x, ast.Assign) and any(isinstance(t, ast.Name)
                      and t.id == 'trace_candidate_id' for t in x.targets) for x in n.body))
    start = next(i for i,n in enumerate(parent.body) if isinstance(n, ast.Assign)
                 and any(isinstance(t, ast.Name) and t.id == 'trace_candidate_id' for t in n.targets))
    nodes = parent.body[start:start+4]
    assert isinstance(nodes[-1], ast.Expr) and isinstance(nodes[-1].value, ast.Call)
    events = []
    candidate = NS(candidate_digest='runtime', selection_priority=7)
    calls = []
    def identity(model, graph, selected):
        calls.append((model, graph, selected))
        return 'registered-name', 'registered-digest'
    namespace = dict(self=NS(lifecycle_observer=True, _emit_lifecycle=lambda *a, **k: events.append((a,k))),
        adapter=NS(splitter=NS(describe_candidate_identity=identity)), selected_candidate=candidate,
        descriptor='model', graph='graph', request_id='/request/1', _attempt=1, providers=[1,2,3,4])
    exec(compile(ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])), str(path), 'exec'), namespace)
    assert calls == [('model', 'graph', candidate)]
    assert events[0][1]['candidateId'] == 'registered-name'
    assert events[0][1]['candidateDigest'] == 'registered-digest'
    assert candidate.candidate_digest == 'runtime'
