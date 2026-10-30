#!/usr/bin/env python3
"""Join opt-in native feedback boundaries by request, attempt and exact logical name."""
import argparse
import json
from pathlib import Path
import re

from analyze_kernel_measurement import distribution


def analyze(root):
    native = root / 'native'
    boundaries, completes, backend = {}, {}, {}
    providers = [json.loads(line) for path in native.glob('provider-*-statistics.jsonl')
                 for line in path.read_text().splitlines()]
    for row in providers:
        if 'runtimeMetrics' not in row:
            continue
        for metric in row['runtimeMetrics']:
            timing = metric['lastBackendExecution']
            key = (row['requestId'], str(timing['attemptEpoch']), row['role'], metric['inferenceEpoch'])
            assert key not in backend, 'duplicate backend identity'
            backend[key] = timing
    for path in native.glob('provider-*.log'):
        for line in path.read_text().splitlines():
            if 'NDNSF_DI_FEEDBACK_BOUNDARY ' in line:
                row = dict(re.findall(r'(\w+)=([^ \n]+)', line))
                key = (row['requestId'], row['attempt'], row['planned_name'])
                group = boundaries.setdefault(key, {})
                assert row['boundary'] not in group, 'duplicate feedback boundary'
                group[row['boundary']] = row
            elif 'NDNSF_DI_PROVIDER_STAGE ' in line:
                row = dict(re.findall(r'(\w+)=([^ \n]+)', line))
                if row.get('stage') == 'DEPENDENCY_FETCH' and row.get('status') == 'complete':
                    key = (row['requestId'], row['attemptEpoch'], row['plannedName'])
                    assert key not in completes, 'duplicate consumer completion'
                    completes[key] = row
    assert boundaries, 'feedback boundary clocks unavailable'
    requests = [json.loads(line) for line in (native / 'requester-statistics.jsonl').read_text().splitlines()]
    requests = {row['requestId']: row for row in requests if row.get('type') == 'request'}
    intervals = []
    for (request, attempt, name), group in boundaries.items():
        assert set(group) == {'ready', 'publish_begin', 'publish_done'}, 'incomplete feedback publication'
        ready, begin, done = [group[key] for key in ('ready', 'publish_begin', 'publish_done')]
        for row in (begin, done):
            assert all(row[key] == ready[key] for key in ('requestId', 'attempt', 'role', 'epoch',
                       'planned_name', 'manifest_name', 'tid')), 'feedback binding mismatch'
        complete = completes[(request, attempt, name)]
        epoch = int(ready['epoch'])
        producer = backend[(request, attempt, ready['role'], epoch)]
        consumer = backend[(request, attempt, complete['role'], epoch + 1)]
        clocks = [producer['finishedSteadyNs'], int(ready['steady_ns']), int(begin['steady_ns']),
                  int(done['steady_ns']), int(complete['steady_us']) * 1000, consumer['startedSteadyNs']]
        labels = ['backend_end_to_feedback_ready', 'ready_to_publish_begin',
                  'publish_call', 'publish_return_to_consumer_complete', 'consumer_complete_to_backend']
        values = [(last - first) / 1e6 for first, last in zip(clocks, clocks[1:])]
        status = 'ADDITIVE' if all(value >= 0 for value in values) else 'OVERLAPPING_OR_QUANTIZED'
        intervals.append(dict(turn=requests[request]['turn'], request_id=request, attempt=attempt,
            planned_name=name, manifest_name=ready['manifest_name'], producer_role=ready['role'],
            consumer_role=complete['role'], producer_epoch=epoch, consumer_epoch=epoch + 1,
            producer_tid=int(ready['tid']), started_ns=clocks[0], finished_ns=clocks[-1],
            clocks_ns=clocks, status=status, parts_ms=dict(zip(labels, values)),
            gap_ms=(clocks[-1] - clocks[0]) / 1e6))
    turns = []
    for request, stats in requests.items():
        rows = [row for row in intervals if row['request_id'] == request]
        generated = len(json.loads((root / f"outputs/turn-{stats['turn']}.bin").read_text())['tokenIds'])
        assert len(rows) == generated
        assert sorted(row['producer_epoch'] for row in rows) == list(range(generated))
        valid = [row for row in rows if row['status'] == 'ADDITIVE']
        parts = {key: distribution([row['parts_ms'][key] for row in valid])
                 for key in rows[0]['parts_ms']}
        turns.append(dict(turn=stats['turn'], generated_tokens=generated, samples=len(rows),
            additive_samples=len(valid), gap_ms=distribution([row['gap_ms'] for row in rows]),
            additive_parts_ms=parts))
    return dict(schema='ndnsf-di-feedback-boundaries-v1', turns=sorted(turns, key=lambda x: x['turn']),
        intervals=sorted(intervals, key=lambda x: (x['turn'], x['producer_epoch'])),
        scope=['Logical name is joined exactly; manifest_name is a distinct transport Data name.',
               'Producer generation epoch maps to the next consumer epoch by verified native source.',
               'publish_done is publishOutput return, not socket transmission or remote admission.',
               'Consumer completion has microsecond resolution; other clocks use nanoseconds.',
               'Negative/overlapping component boundaries are preserved and excluded from additive summaries.',
               'Tracing is explicit; this measures boundaries and does not establish a speedup.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.root)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
    print(json.dumps(dict(output=str(args.output), turns=result['turns'])))
