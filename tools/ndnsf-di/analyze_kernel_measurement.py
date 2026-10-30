#!/usr/bin/env python3
"""Decode this observer's uncompressed little-endian perf trace and warm intervals.

Raw skb pointers are necessary: tracepoint pretty-printers use different pointer
formats. Fail closed for another sample layout instead of pairing printed hashes.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import signal
import statistics
import struct
import subprocess


def distribution(values):
    values = sorted(values)
    if not values:
        return dict(count=0, mean=None, median=None, p95=None, maximum=None, total=0)
    return dict(count=len(values), mean=statistics.mean(values), median=statistics.median(values),
                p95=values[min(len(values) - 1, (95 * len(values) + 99) // 100 - 1)],
                maximum=values[-1], total=sum(values))


def formats(rows):
    result = {}
    for name, text in rows.items():
        fields = {}
        for declaration, offset, size, signed in re.findall(
                r'field:([^;]+);\s*offset:(\d+);\s*size:(\d+);\s*signed:(\d+);', text):
            field = declaration.split()[-1].split('[')[0].lstrip('*')
            fields[field] = dict(offset=int(offset), size=int(size), signed=int(signed),
                                 dynamic='__data_loc' in declaration,
                                 string='char' in declaration)
        result[int(re.search(r'^ID: (\d+)', text, re.M)[1])] = (name.replace('/', ':'), fields)
    return result


def decode_fields(raw, schema):
    result = {}
    for name, field in schema.items():
        start, size = field['offset'], field['size']
        assert start + size <= len(raw), 'short trace payload'
        data = raw[start:start + size]
        if field['dynamic']:
            loc = int.from_bytes(data, 'little')
            start, size = loc & 65535, loc >> 16
            assert start + size <= len(raw), 'short dynamic trace field'
            data = raw[start:start + size]
        result[name] = (data.split(b'\0')[0].decode() if field['string'] else
                        int.from_bytes(data, 'little', signed=bool(field['signed'])))
    return result


def read_samples(path, schemas):
    data = path.read_bytes()
    assert data[:8] == b'PERFILE2', 'only native little-endian perf files supported'
    offset, length = struct.unpack_from('<QQ', data, 40)
    end = offset + length
    assert end <= len(data)
    samples, lost, unknown = [], 0, Counter()
    while offset < end:
        kind, misc, size = struct.unpack_from('<IHH', data, offset)
        assert size >= 8 and offset + size <= end, 'invalid perf record boundary'
        record = data[offset:offset + size]
        if kind == 2:
            lost += struct.unpack_from('<Q', record, 16)[0]
        elif kind == 13:
            lost += struct.unpack_from('<Q', record, 8)[0]
        elif kind in (5, 6):
            unknown['PERF_RECORD_THROTTLE' if kind == 5 else 'PERF_RECORD_UNTHROTTLE'] += 1
        elif kind == 9:
            assert size >= 60, 'unsupported sample record layout'
            raw_size = struct.unpack_from('<I', record, 56)[0]
            assert raw_size >= 8 and raw_size + 60 <= size, 'unsupported raw sample layout'
            raw = record[60:60 + raw_size]
            type_id = struct.unpack_from('<H', raw)[0]
            if type_id not in schemas:
                unknown[type_id] += 1
            else:
                name, schema = schemas[type_id]
                samples.append(dict(time_ns=struct.unpack_from('<Q', record, 32)[0],
                    event=name, fields=decode_fields(raw, schema)))
        offset += size
    return sorted(samples, key=lambda x: x['time_ns']), lost, dict(unknown)


def pair_samples(samples, tids):
    queue, transmit, runnable = {}, {}, {}
    residence, driver, waits = [], [], []
    counts = Counter()
    for sample in samples:
        name, ns, fields = sample['event'], sample['time_ns'], sample['fields']
        counts[name] += 1
        if name.startswith('net:'):
            key = (fields['name'], fields['skbaddr'], fields['len'])
            row = dict(interface=fields['name'], bytes=fields['len'])
            if name == 'net:net_dev_queue':
                if key in queue:
                    counts['ambiguous_requeue'] += 1
                    queue[key] = None
                else:
                    queue[key] = ns
            elif name == 'net:net_dev_start_xmit':
                started = queue.pop(key, None)
                if started is None:
                    counts['start_without_unambiguous_queue'] += 1
                else:
                    residence.append(dict(row, started_ns=started, finished_ns=ns))
                if key in transmit:
                    counts['start_without_previous_xmit_end'] += 1
                    transmit[key] = None
                else:
                    transmit[key] = ns
            elif name == 'net:net_dev_xmit':
                started = transmit.pop(key, None)
                counts['xmit_rc_' + str(fields['rc'])] += 1
                if started is None:
                    counts['xmit_without_start'] += 1
                else:
                    driver.append(dict(row, started_ns=started, finished_ns=ns, rc=fields['rc']))
        elif name in ('sched:sched_wakeup', 'sched:sched_wakeup_new'):
            tid = fields['pid']
            if tid in tids:
                runnable.setdefault(tid, (ns, 'wakeup'))
        elif name == 'sched:sched_switch':
            previous, following = fields['prev_pid'], fields['next_pid']
            if previous in tids:
                if fields['prev_state'] in (0, 256):  # TASK_RUNNING or trace TASK_REPORT_MAX
                    runnable.setdefault(previous, (ns, 'preempted_or_yielded'))
                else:
                    runnable.pop(previous, None)
            if following in tids:
                started = runnable.pop(following, None)
                if started:
                    waits.append(dict(tid=following, started_ns=started[0], finished_ns=ns,
                                      reason=started[1]))
                else:
                    counts['switch_in_without_runnable_start'] += 1
    counts['unmatched_queue_at_end'] = len(queue)
    counts['unmatched_xmit_start_at_end'] = len(transmit)
    counts['unmatched_runnable_at_end'] = len(runnable)
    return dict(counts=dict(counts), queue_intervals=residence, driver_intervals=driver,
                runnable_intervals=waits)


def validate_close(close):
    assert close['status'] == 'CLOSED' and close['group']['status'] == 'ZERO'
    assert not close['forced'] and not close['group'].get('forced')
    assert not close.get('error') and not close.get('target_error')
    assert close['exit_code'] == 0 or (
        close.get('interrupted') is True and close['exit_code'] == -signal.SIGINT)


def analyze(root):
    capture = root / 'capture'
    start = json.loads((capture / 'kernel-start.json').read_text())
    close = json.loads((capture / 'kernel-close.json').read_text())
    validate_close(close)
    metadata = json.loads((capture / 'kernel-formats.json').read_text())
    perf = start['command'][0]
    evlist = subprocess.run([perf, 'evlist', '-v', '-i', str(capture / 'kernel.data')],
                            capture_output=True, text=True, timeout=15, check=True).stdout
    attributes = [line for line in evlist.splitlines() if line.startswith(('net:', 'sched:'))]
    assert len(attributes) == 6
    for line in attributes:
        match = re.search(r'sample_type: ([^,]+)', line)
        assert set(match[1].split('|')) == set('IP|TID|TIME|CPU|PERIOD|RAW|IDENTIFIER'.split('|'))
        assert 'clockid: 1' in line
    samples, lost, unknown = read_samples(capture / 'kernel.data', formats(metadata['formats']))
    assert samples, 'no kernel samples'
    if lost or unknown:
        return dict(schema='ndnsf-di-kernel-diagnosis-v1', status='UNAVAILABLE_TRACE_GAP',
                    lost_samples=lost, unknown_event_ids=unknown,
                    reason='Missing/unknown events can invalidate pairing; interval summaries suppressed.')
    threads = {x['tid']: x for x in start['threads']}
    paired = pair_samples(samples, threads)
    requests = [json.loads(x) for x in (root / 'native/requester-statistics.jsonl').read_text().splitlines()]
    warm = next(x for x in requests if x.get('type') == 'request' and x['turn'] == 1)
    low = max(samples[0]['time_ns'], warm['startedSteadyNs'])
    high = min(samples[-1]['time_ns'], warm['finishedSteadyNs'])
    assert high > low
    def contained(rows):
        return [x for x in rows if low <= x['started_ns'] <= x['finished_ns'] <= high]
    queues = contained(paired['queue_intervals'])
    waits = contained(paired['runnable_intervals'])
    by_interface = {interface['name']: distribution([(x['finished_ns'] - x['started_ns']) / 1e6
        for x in queues if x['interface'] == interface['name']]) for interface in start['interfaces']}
    by_thread = [dict(thread=row, wait_ms=distribution([(x['finished_ns'] - x['started_ns']) / 1e6
                    for x in waits if x['tid'] == tid])) for tid, row in threads.items()]
    from compare_generation_lengths import backend_chain_partition
    providers = [json.loads(line) for path in (root / 'native').glob('provider-*-statistics.jsonl')
                 for line in path.read_text().splitlines()]
    request_timings = []
    measured = json.loads((root / 'analysis.json').read_text())
    for turn in measured['turns']:
        request = next(x for x in requests if x.get('type') == 'request' and x['turn'] == turn['turn'])
        rows = [x for x in providers if x.get('requestId') == turn['request_id']]
        assert len(rows) == 2
        context = [dict(role=x['role'], initial=x['cacheObservations'][0]) for x in rows]
        for row in context:
            first = row['initial']
            assert first['representedPrefixTokenCount'] == (25 if turn['turn'] == 0 else 81)
            assert first['prefixWorkAvoided'] == (0 if turn['turn'] == 0 else 57)
            assert first['conversationStateHit'] == (turn['turn'] == 1)
        generated = len(json.loads((root / f"outputs/turn-{turn['turn']}.bin").read_text())['tokenIds'])
        assert generated == (32 if turn['turn'] == 0 else 256)
        calls = [dict(role=row['role'], epoch=metric['inferenceEpoch'],
            started_ns=metric['lastBackendExecution']['startedSteadyNs'],
            finished_ns=metric['lastBackendExecution']['finishedSteadyNs'])
            for row in rows for metric in row['runtimeMetrics']]
        request_timings.append(dict(turn=turn['turn'], generated_tokens=generated,
            wall_seconds=turn['wall_seconds'], backend_union_seconds=turn['backend_union_seconds'],
            backend_share_percent=100 * turn['backend_union_seconds'] / turn['wall_seconds'],
            other_wall_seconds=turn['wall_seconds'] - turn['backend_union_seconds'],
            requester_cpu_seconds=turn['requester_cpu_seconds'], context=context,
            provider_cpu_seconds=[dict(role=row['role'], seconds=(row['finishedProcessCpuNs'] -
                row['startedProcessCpuNs']) / 1e9) for row in rows],
            backend_chain_partition=backend_chain_partition(request['startedSteadyNs'],
                request['finishedSteadyNs'], calls)))
    return dict(schema='ndnsf-di-kernel-diagnosis-v1', status='MEASURED_WINDOW',
        kernel=metadata['kernel'], lost_samples=lost,
        capture_bindings=start['proof']['bindings'],
        unknown_event_ids=unknown, event_counts=paired['counts'],
        thread_set_unchanged=close['thread_set_unchanged'], interrupted=close['interrupted'],
        trace_first_ns=samples[0]['time_ns'], trace_last_ns=samples[-1]['time_ns'],
        warm_observed_low_ns=low, warm_observed_high_ns=high,
        warm_window_seconds=(high - low) / 1e9, queue_residence_ms=by_interface,
        request_timings=request_timings,
        runnable_wait_by_thread=by_thread, samples=paired,
        limits=['Name-filtered interfaces; net events have no network-namespace field.',
                'Thread snapshots do not prove full-lifetime thread coverage.',
                'Matched skb queue-to-start is qdisc residence, including configured netem delay.',
                'Driver duration and scheduling intervals overlap other work; no additive causal attribution.',
                'Runnable waits are matched wakeups or runnable switch-outs; boundary-unmatched samples excluded.',
                'Diagnostic tracing perturbs execution; this is not an optimization comparison.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.root)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ('samples', 'runnable_wait_by_thread', 'request_timings')}))
