"""Real retained lifecycle validation; handle and shutdown are boundary fixtures."""
import json
from types import SimpleNamespace

import pytest

from test_yolo_lifecycle_result import D, journal
from runtime import yolo_negative as negative
from runtime.yolo_result import validate_lifecycle


def fixture(tmp_path, monkeypatch, outcomes):
    rows = journal()[:-1]
    for row in rows:
        row['caseId'] = 'negative-dependency'
    path = tmp_path / 'lifecycle.jsonl'
    path.write_text('\n'.join(json.dumps(row) for row in rows) + '\n')
    clock = [100.0]
    monkeypatch.setattr(negative.time, 'monotonic', lambda: clock[0])
    calls = []

    def response(timeout):
        calls.append(timeout)
        value = outcomes[len(calls) - 1]
        if isinstance(value, Exception):
            raise value
        return value

    observer = negative.NegativeUserObserver(run_id='negative-test',
        request_id='/run/request/1', candidate_digest='sha256:' + '2' * 64,
        output=tmp_path, placement_id='shared-backbone-two-shard-v1',
        placement_digest=D, deadline_ms=10000)
    args = SimpleNamespace(request_id=observer.request_id,
        lifecycle_case='negative-dependency', timeout_ms=10000,
        lifecycle_output_dir=str(tmp_path))
    binding = dict(handle=SimpleNamespace(execution_plan_digest=D, response=response),
        journal=SimpleNamespace(request_id=observer.request_id, attempt_id='attempt-1'),
        args=args, request_started_at=99.0)
    return observer, binding, calls, clock, rows


def test_timeout_remains_observation_only(tmp_path, monkeypatch):
    observer, binding, calls, clock, _ = fixture(tmp_path, monkeypatch,
        [TimeoutError('wait'), RuntimeError('shutdown')])
    assert observer.terminal(**binding) == 0
    assert not (tmp_path / 'negative-user.json').exists()
    clock[0] += 0.5
    value = observer.finish_after_shutdown(0)
    assert calls == [6500, 1]
    assert value['qualification'] == 'OBSERVATION_ONLY'
    assert value['response'] == dict(present=False, success=False, bytes=0, sha256=None)
    assert value['elapsedMs'] == 1500
    assert json.loads((tmp_path / 'negative-user.json').read_text()) == value
    with pytest.raises(ValueError, match='OWNER_NOT_COMPLETE'):
        observer.finish_after_shutdown(0)


@pytest.mark.parametrize('arrival', ['initial', 'shutdown'])
def test_late_success_cannot_be_recorded_as_absent(tmp_path, monkeypatch, arrival):
    response = SimpleNamespace(status=True, payload=b'real result')
    outcomes = ([response, RuntimeError('stopped')] if arrival == 'initial'
                else [TimeoutError('wait'), response])
    observer, binding, _, _, _ = fixture(tmp_path, monkeypatch, outcomes)
    observer.terminal(**binding)
    value = observer.finish_after_shutdown(0)
    assert value['response']['present'] is True
    assert value['response']['success'] is True
    assert value['response']['bytes'] == len(response.payload)
    assert value['qualification'] == 'OBSERVATION_ONLY'


@pytest.mark.parametrize('fault', ['missing-selection', 'duplicate-selection', 'wrong-plan',
                                  'wrong-request', 'wrong-attempt', 'expired', 'future'])
def test_binding_or_incomplete_selection_rejects_before_wait(tmp_path, monkeypatch, fault):
    observer, binding, calls, _, rows = fixture(tmp_path, monkeypatch, [])
    if fault == 'missing-selection': rows.pop(7)
    if fault == 'duplicate-selection': rows.insert(8, rows[7])
    if fault in ('missing-selection', 'duplicate-selection'):
        (tmp_path / 'lifecycle.jsonl').write_text('\n'.join(json.dumps(r) for r in rows) + '\n')
    if fault == 'wrong-plan': binding['handle'].execution_plan_digest = 'sha256:' + '3' * 64
    if fault == 'wrong-request': binding['args'].request_id = '/other'
    if fault == 'wrong-attempt': binding['journal'].attempt_id = 'attempt-2'
    if fault == 'expired': binding['request_started_at'] = 1.0
    if fault == 'future': binding['request_started_at'] = 101.0
    with pytest.raises((ValueError, TimeoutError)):
        observer.terminal(**binding)
    assert calls == []
    assert not (tmp_path / 'negative-user.json').exists()


@pytest.mark.parametrize('fault', ['owner-failed', 'late-deadline', 'selection-changed',
                                  'none-result', 'bad-response'])
def test_invalid_completion_retains_no_observation(tmp_path, monkeypatch, fault):
    outcomes = [TimeoutError('wait'), RuntimeError('shutdown')]
    if fault == 'none-result': outcomes[1] = None
    if fault == 'bad-response': outcomes[1] = SimpleNamespace(status=1, payload=b'')
    observer, binding, _, clock, rows = fixture(tmp_path, monkeypatch, outcomes)
    observer.terminal(**binding)
    if fault == 'late-deadline': clock[0] = 110.0
    if fault == 'selection-changed':
        rows[7]['selectionDigest'] = 'sha256:' + '4' * 64
        (tmp_path / 'lifecycle.jsonl').write_text('\n'.join(json.dumps(r) for r in rows) + '\n')
    with pytest.raises((ValueError, TimeoutError)):
        observer.finish_after_shutdown(1 if fault == 'owner-failed' else 0)
    assert not (tmp_path / 'negative-user.json').exists()


def test_normal_case_cannot_skip_terminal_validation(tmp_path):
    with pytest.raises(ValueError, match='LIFECYCLE_TERMINAL_MODE'):
        validate_lifecycle(tmp_path, case='two-node-gpu', request_id='/r',
            attempt_id='attempt-1', candidate_id='candidate', candidate_digest=D,
            require_terminal=False)


def test_composition_observes_after_owner_shutdown_without_reference(tmp_path, monkeypatch):
    import importlib.util
    from apps import yolo as app
    from runtime import yolo_graph_reference
    events = []

    class Observer:
        dependency_no_progress_ms = 2500
        def __init__(self, **kwargs):
            assert kwargs['deadline_ms'] == 5000
        def terminal(self):
            events.append('wait')
            return 0
        def finish_after_shutdown(self, rc):
            assert rc == 0 and events == ['wait', 'shutdown']
            events.append('snapshot')

    def main(argv, *, terminal_handler, dependency_no_progress_ms):
        assert dependency_no_progress_ms == 2500
        try:
            return terminal_handler()
        finally:
            events.append('shutdown')

    def no_reference(*args, **kwargs):
        pytest.fail('negative observation must not run numerical reference')

    monkeypatch.setattr(negative, 'NegativeUserObserver', Observer)
    monkeypatch.setattr(yolo_graph_reference, 'RequestReferenceBinding', no_reference)
    monkeypatch.setattr(app, 'APP_DIR', tmp_path)
    monkeypatch.setattr(importlib.util, 'spec_from_file_location', lambda *_:
        SimpleNamespace(loader=SimpleNamespace(exec_module=lambda _: None)))
    monkeypatch.setattr(importlib.util, 'module_from_spec', lambda _: SimpleNamespace(main=main))
    assert app.run_user_with_reference(['--timeout-ms', '5000'], negative=True,
        backend='CPUExecutionProvider', run_id='run', request_id='/r',
        candidate_digest=D, placement_candidate_digest='sha256:'+'b'*64,
        output=tmp_path) == 0
    assert events == ['wait', 'shutdown', 'snapshot']


@pytest.mark.parametrize('return_code', [0, 9])
def test_negative_schedule_has_one_request_and_no_numerical_reference(tmp_path, return_code):
    from apps.yolo import run_requests
    from test_yolo_application import scheduled_inputs
    worker, plan, options = scheduled_inputs(tmp_path, 'two-node-gpu')
    worker.mode = plan['case'] = 'negative-dependency'
    plan['requests'] = plan['requests'][:1]
    plan['requests'][0]['warmup'] = False
    calls, accepted = [], []
    worker.run_user = lambda index, argv, **kwargs: calls.append((index, argv, kwargs)) or return_code
    try:
        if return_code:
            with pytest.raises(RuntimeError, match='NEGATIVE_USER_PROCESS_EXIT'):
                run_requests(worker, plan, accept_request=lambda *v: accepted.append(v), **options)
            assert accepted == []
        else:
            run_requests(worker, plan, accept_request=lambda *v: accepted.append(v), **options)
            assert len(accepted) == 1
        index, argv, kwargs = calls[0]
        assert len(calls) == 1 and index == '0'
        assert argv.index('--expected-dependency-failure') < argv.index('--')
        assert '--retain-numerical-response' not in argv
        assert '--retain-public-assignments' in argv
        assert kwargs['reference_gpu'] is False
    finally:
        worker.close()
