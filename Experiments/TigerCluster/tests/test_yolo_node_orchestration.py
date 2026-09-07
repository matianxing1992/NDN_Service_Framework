"""Node owner ordering through explicit runtime doubles, not a launch gate."""
from pathlib import Path
import sys
from types import SimpleNamespace as NS

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps import yolo
from runtime import yolo_result


def setup(tmp_path, monkeypatch, fail=False):
    events = []
    plan = {'case': 'local-cpu', 'runId': 'test-run', 'requests': [0,1]}
    binding = dict(runId='test-run', candidateDigest='sha256:'+'1'*64, probeId='a'*32)
    startup = NS(binding=binding, rank=0, ranks=(0,), directory=tmp_path/'startup')
    completion = NS(binding=binding.copy(), rank=0, ranks=(0,), directory=tmp_path/'completion',
        remaining=lambda: 10, publish=lambda *a: events.append(('publish',a)),
        wait=lambda *a: {0: {'requestCount': 2}})
    state = NS(mode='local-cpu', rank=0, output=tmp_path,
        _preparation_binding=(plan, None, None),
        close=lambda: events.append('close') or [])
    monkeypatch.setattr(yolo, 'configure_network', lambda *a, **k: events.append('network'))
    monkeypatch.setattr(yolo, 'start_workload', lambda *a, **k: events.append('startup'))
    def requests(*args, **kwargs):
        events.append('requests')
        if fail: raise ValueError('request failed')
        kwargs['accept_request']({}, tmp_path)
    monkeypatch.setattr(yolo, 'run_requests', requests)
    monkeypatch.setattr(yolo_result, 'write_worker_receipt', lambda *a: events.append('receipt') or {'done': True})
    return state, startup, completion, events


def test_normal_owner_orders_start_requests_cleanup_receipt(tmp_path, monkeypatch):
    state, startup, completion, events = setup(tmp_path, monkeypatch)
    value = yolo.run_normal_node(state, startup, completion_factory=lambda: completion,
        endpoints=[], startup_options={}, request_options={}, accept_request=lambda *a: events.append('accept'))
    assert value == {'done': True}
    assert events[:4] == ['network','startup','requests','accept']
    assert events[-2:] == ['close','receipt']


def test_failure_still_closes_notifies_and_preserves_local_record(tmp_path, monkeypatch):
    state, startup, completion, events = setup(tmp_path, monkeypatch, fail=True)
    startup.publish = lambda *args: events.append(('startup-failed', args))
    with pytest.raises(ValueError, match='request failed'):
        yolo.run_normal_node(state, startup, completion_factory=lambda: completion,
            endpoints=[], startup_options={}, request_options={}, accept_request=lambda *a: None)
    assert 'close' in events and 'receipt' not in events
    assert (tmp_path/'node-failure.json').is_file()
    assert any(isinstance(e, tuple) and e[0] == 'publish' and e[1][0] == 'failed' for e in events)


def test_completion_lane_cannot_reuse_startup_directory(tmp_path, monkeypatch):
    state, startup, completion, events = setup(tmp_path, monkeypatch)
    completion.directory = startup.directory
    with pytest.raises(ValueError, match='COMPLETION_BINDING'):
        yolo.run_normal_node(state, startup, completion_factory=lambda: completion,
            endpoints=[], startup_options={}, request_options={}, accept_request=lambda *a: None)
    assert 'close' in events and 'requests' not in events
