"""Node owner ordering through explicit runtime doubles, not a launch gate."""
from pathlib import Path
import sys
from types import SimpleNamespace as NS
from concurrent.futures import ThreadPoolExecutor
import threading

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps import yolo
from runtime import yolo_result
from runtime.yolo_worker import StartupBarrier


def setup(tmp_path, monkeypatch, fail=False):
    events = []
    plan = {'case': 'local-cpu', 'runId': 'test-run', 'requests': [0,1]}
    binding = dict(runId='test-run', candidateDigest='sha256:'+'1'*64, probeId='a'*32)
    startup = NS(binding=binding, rank=0, ranks=(0,), directory=tmp_path/'startup', _read=lambda *a: None)
    completion = NS(binding=binding.copy(), rank=0, ranks=(0,), directory=tmp_path/'completion',
        remaining=lambda: 10, check=lambda: None, publish=lambda *a: events.append(('publish',a)),
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


@pytest.mark.parametrize('failure', [False, True])
def test_gpu_probe_precedes_network_and_provider_start(tmp_path, monkeypatch, failure):
    state, startup, completion, events = setup(tmp_path, monkeypatch)
    state.mode = state._preparation_binding[0]['case'] = 'single-node-gpu'
    startup.remaining = lambda: 7
    state.verify_allocation = lambda *a, **k: events.append('allocation')
    def probe(**kwargs):
        assert kwargs['seconds'] == 7
        events.append('gpu-probe')
        if failure: raise ValueError('GPU unavailable')
    state.probe_gpu_device = probe
    def run():
        return yolo.run_normal_node(state, startup, completion_factory=lambda: completion,
            endpoints=[], startup_options={}, request_options={}, accept_request=lambda *a: None)
    if failure:
        with pytest.raises(ValueError, match='GPU unavailable'): run()
        assert events == ['allocation', 'gpu-probe', 'close']
    else:
        run()
        assert events[:4] == ['allocation', 'gpu-probe', 'network', 'startup']


def test_failure_still_closes_notifies_and_preserves_local_record(tmp_path, monkeypatch):
    state, startup, completion, events = setup(tmp_path, monkeypatch, fail=True)
    startup.publish = lambda *args: events.append(('startup-failed', args))
    with pytest.raises(ValueError, match='request failed'):
        yolo.run_normal_node(state, startup, completion_factory=lambda: completion,
            endpoints=[], startup_options={}, request_options={}, accept_request=lambda *a: None)
    assert 'close' in events and 'receipt' not in events
    assert (tmp_path/'node-failure.json').is_file()
    assert any(isinstance(e, tuple) and e[0] == 'publish' and e[1][0] == 'failed' for e in events)


def test_allocation_failure_prevents_even_gpu_probe(tmp_path, monkeypatch):
    state, startup, completion, events = setup(tmp_path, monkeypatch)
    state.mode = state._preparation_binding[0]['case'] = 'single-node-gpu'
    startup.remaining = lambda: 7
    def reject(*args, **kwargs):
        events.append('allocation')
        raise ValueError('wrong allocation')
    state.verify_allocation = reject
    state.probe_gpu_device = lambda **k: events.append('GPU MUST NOT RUN')
    with pytest.raises(ValueError, match='wrong allocation'):
        yolo.run_normal_node(state, startup, completion_factory=lambda: completion,
            endpoints=[], startup_options={}, request_options={}, accept_request=lambda *a: None)
    assert events == ['allocation', 'close']


def test_completion_lane_cannot_reuse_startup_directory(tmp_path, monkeypatch):
    state, startup, completion, events = setup(tmp_path, monkeypatch)
    completion.directory = startup.directory
    with pytest.raises(ValueError, match='COMPLETION_BINDING'):
        yolo.run_normal_node(state, startup, completion_factory=lambda: completion,
            endpoints=[], startup_options={}, request_options={}, accept_request=lambda *a: None)
    assert 'close' in events and 'requests' not in events


def test_completion_observes_failure_before_peer_completion_exists(tmp_path, monkeypatch):
    state, startup, completion, events = setup(tmp_path, monkeypatch)
    startup._read = lambda *a: {'errorType': 'factory failure'}
    def wait(*args):
        completion.check()
    completion.wait = wait
    with pytest.raises(RuntimeError, match='YOLO_PEER_FAILED'):
        yolo.run_normal_node(state, startup, completion_factory=lambda: completion,
            endpoints=[], startup_options={}, request_options={}, accept_request=lambda *a: None)
    assert 'close' in events and 'receipt' not in events


@pytest.mark.parametrize('fail_request', [False, True])
def test_two_rank_real_barriers_keep_peer_alive_and_propagate_failure(tmp_path, monkeypatch, fail_request):
    for name in ('startup', 'completion', 'node0', 'node1'):
        (tmp_path / name).mkdir()
    release, entered, head_waiting = threading.Event(), threading.Event(), threading.Event()
    closed = [threading.Event(), threading.Event()]
    plan = dict(case='two-node-gpu', runId='test-run', requests=list(range(4)))
    def barrier(name, rank):
        return StartupBarrier(tmp_path/name, run_id='test-run', probe_id='a'*32,
            candidate_digest='sha256:'+'1'*64, ranks=(0,1), rank=rank,
            seconds=5, check=lambda: None)
    def state(rank):
        return NS(mode='two-node-gpu', rank=rank, output=tmp_path/('node'+str(rank)),
            _preparation_binding=(plan,None,None), close=lambda: closed[rank].set() or [],
            probe_gpu_device=lambda **kwargs: None, verify_allocation=lambda *a, **k: None)
    monkeypatch.setattr(yolo, 'configure_network', lambda *a, **k: None)
    monkeypatch.setattr(yolo, 'start_workload', lambda *a, **k: None)
    def requests(worker, plan, **kwargs):
        assert worker.rank == 0
        entered.set()
        assert release.wait(3)
        if fail_request:
            raise ValueError('injected request failure')
        kwargs['accept_request']({}, worker.output)
    monkeypatch.setattr(yolo, 'run_requests', requests)
    monkeypatch.setattr(yolo_result, 'write_worker_receipt', lambda w, rows: {'rank': w.rank})
    def run(rank):
        completion = barrier('completion',rank)
        if rank == 1:
            original_wait = completion.wait
            def wait(*a, **k):
                head_waiting.set()
                return original_wait(*a, **k)
            completion.wait = wait
        return yolo.run_normal_node(state(rank), barrier('startup',rank),
            completion_factory=lambda: completion, endpoints=[], startup_options={},
            request_options={}, accept_request=lambda *a: None)
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(run, rank) for rank in (0,1)]
        try:
            assert entered.wait(3) and head_waiting.wait(3)
            assert not any(event.is_set() for event in closed)
        finally:
            release.set()
        if fail_request:
            with pytest.raises(ValueError, match='injected request failure'):
                futures[0].result(timeout=5)
            with pytest.raises(RuntimeError, match='PEER_FAILED'):
                futures[1].result(timeout=5)
            assert all((tmp_path/('node'+str(rank))/'node-failure.json').is_file() for rank in (0,1))
        else:
            assert [f.result(timeout=5) for f in futures] == [{'rank':0}, {'rank':1}]
    assert all(event.is_set() for event in closed)
