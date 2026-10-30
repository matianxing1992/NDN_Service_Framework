#!/usr/bin/env python3
"""Correlate opt-in Core dispatch clocks with archived activation observations."""
import argparse
from collections import Counter
from decimal import Decimal
import json
from pathlib import Path
import statistics

from ndn.encoding import Name


def name_key(text):
    return tuple(bytes(component) for component in Name.from_str(text))


def analyze(root):
    root = Path(root)
    objects = json.loads((root / 'analysis.json').read_text())['objects']
    packets = json.loads((root / 'packet-analysis.json').read_text())['objects']
    start = json.loads((root / 'capture/capture-start.json').read_text())
    offset = start['ready_epoch_ns'] - start['ready_steady_ns']
    traces, counts = [], Counter()
    for log in (root / 'native').glob('provider-*.log'):
        for line in log.read_text(errors='replace').splitlines():
            if 'NDNSF_EXACT_DATA_DISPATCH ' not in line:
                continue
            fields = dict(word.split('=', 1) for word in line.split(
                'NDNSF_EXACT_DATA_DISPATCH ', 1)[1].split() if '=' in word)
            clocks = [int(fields[key]) for key in ('queued_steady_ns', 'started_steady_ns',
                      'put_started_steady_ns', 'put_returned_steady_ns')]
            if clocks != sorted(clocks):
                raise ValueError('dispatch clocks are not monotonic')
            if fields.get('mode', 'outer-dispatch') not in ('outer-dispatch', 'native-enqueue'):
                raise ValueError('unknown dispatch mode')
            traces.append(dict(log=log.name, name=name_key(fields['name']), clocks=clocks,
                               mode=fields.get('mode', 'outer-dispatch')))
    rows, missing = [], []
    for obj in objects:
        size = obj.get('packed_publication_payload_bytes')
        if obj['producer_log'] != 'provider-0.log' or size is None or size >= 10000:
            continue
        prefix = name_key(obj['planned_name'])
        matches = [trace for trace in traces if trace['log'] == obj['producer_log'] and
                   trace['name'][:len(prefix)] == prefix]
        observed = [row for row in packets if row['planned_name'] == obj['planned_name']]
        if len(matches) != 1 or len(observed) != 1 or observed[0]['observed_data_packets'] != 1:
            counts['missing_or_ambiguous_object'] += 1
            missing.append(dict(turn=obj['turn'], planned_name=obj['planned_name'],
                                trace_count=len(matches), packet_object_count=len(observed),
                                observed_data_packets=observed[0]['observed_data_packets']
                                    if len(observed) == 1 else None))
            continue
        queued, started, put_started, returned = matches[0]['clocks']
        packet = observed[0]
        captured = int(Decimal(str(packet['first_data_epoch_ms'])) * 10**6) - offset
        rows.append(dict(turn=obj['turn'], planned_name=obj['planned_name'], payload_bytes=size,
            dispatch_mode=matches[0]['mode'],
            output_to_admitted_ms=obj['ready_to_admitted_ms'],
            queued_to_callback_ms=(started - queued) / 1e6
                if matches[0]['mode'] == 'outer-dispatch' else None,
            callback_to_put_ms=(put_started - started) / 1e6
                if matches[0]['mode'] == 'outer-dispatch' else None,
            put_call_ms=(returned - put_started) / 1e6,
            put_return_to_capture_ms=(captured - returned) / 1e6,
            capture_to_admitted_ms=packet['last_data_to_admitted_ms']))
        counts['matched_object'] += 1
    summaries = []
    for turn in sorted({row['turn'] for row in rows}):
        selected = [row for row in rows if row['turn'] == turn]
        summary = dict(turn=turn, count=len(selected),
                       modes=sorted({row['dispatch_mode'] for row in selected}))
        for key in ('output_to_admitted_ms', 'queued_to_callback_ms', 'callback_to_put_ms',
                    'put_call_ms', 'put_return_to_capture_ms', 'capture_to_admitted_ms'):
            values = [row[key] for row in selected if row[key] is not None]
            summary[key] = (dict(mean=statistics.mean(values), median=statistics.median(values),
                                minimum=min(values), maximum=max(values)) if values else
                            dict(status='NOT_APPLICABLE', count=0))
        summaries.append(summary)
    return dict(schema='ndnsf-core-dispatch-measurement-v1', counts=dict(counts),
        objects=rows, missing=missing, turns=summaries,
        scope='Small inline activation objects only. Exact NDN component matching. '
              'face.put return is enqueue/copy completion, not socket completion. '
              'native-enqueue mode has no outer callback wait; it is NOT_APPLICABLE, not measured zero. '
              'Post-put interval contains inner transport queue and outgoing netem. '
              'Core-only intervals use steady clocks; cross-clock intervals use one anchor. '
              'Missing/ambiguous and direct-reply paths are not silently filled.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.archive)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
    print(json.dumps(dict(counts=result['counts'], turns=result['turns'])))
