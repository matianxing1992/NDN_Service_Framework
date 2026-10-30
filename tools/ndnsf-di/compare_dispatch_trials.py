#!/usr/bin/env python3
"""Summarize four archived trials without pooling diagnostic and quiet pairs."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics


def distribution(values):
    values = sorted(values)
    if not values:
        raise ValueError('missing samples')
    return dict(count=len(values), mean=statistics.mean(values),
                median=statistics.median(values), p95=values[math.ceil(.95 * len(values)) - 1],
                minimum=values[0], maximum=values[-1])


def compare(root):
    trials, outputs = {}, {}
    reference_workload = None
    for label in ('baseline-r1', 'baseline-r2', 'treatment-r1', 'treatment-r2'):
        path = root / label
        analysis = json.loads((path / 'analysis.json').read_text())
        measurement = json.loads((path / 'capture/measurement.json').read_text())
        receipt = json.loads((path / 'capture/receipt.json').read_text())
        assert receipt['status'] == 'PASS' and receipt['failure'] is None, label
        deployment = json.loads((path / 'deployment.json').read_text())
        workload = dict(model=deployment['model'],
            topology_sha256=deployment['platform']['topology_sha256'],
            requester={key: deployment['requester'][key] for key in
                       ('catalog', 'request', 'fixed_placement', 'limits')},
            turns=[{key: turn[key] for key in ('input_file', 'options_file')}
                   for turn in deployment['requester']['turns']],
            archived_input_sha256={f'{index}-{key}': hashlib.sha256(
                (path / 'inputs' / f'{index}-{key}').read_bytes()).hexdigest()
                for index in (0, 1) for key in ('input_file', 'options_file')})
        if reference_workload is None:
            reference_workload = workload
        assert workload == reference_workload, (label, 'workload mismatch')
        qdisc = json.loads((path / 'capture/link-qdisc.json').read_text())
        assert qdisc['exit_code'] == 0, label
        netem = [row for row in json.loads(qdisc['stdout']) if row['kind'] == 'netem']
        assert len(netem) == 1 and netem[0]['options']['delay']['delay'] == .005, label
        assert measurement['dispatch_timing'] == label.endswith('r1'), label
        turns = []
        for turn in analysis['turns']:
            index = turn['turn']
            selected = [obj for obj in analysis['objects'] if obj['turn'] == index and
                        obj['producer_log'] == 'provider-0.log' and
                        obj.get('ready_to_admitted_ms') is not None]
            # Native activation records label both prefill/decode as PIPELINE.
            # This frozen workload has one >100KB prefill and 32 <10KB decode objects.
            decode = [obj for obj in selected if
                      obj['packed_publication_payload_bytes'] < 10000]
            assert len(selected) == 33 and len(decode) == 32, (label, index)
            assert turn['missing_producer_observations'] == 0
            assert turn['missing_complete_clocks'] == 0
            assert turn['backend_calls'] == 66 and turn['admitted_objects'] == 65
            output = json.loads((path / f'outputs/turn-{index}.bin').read_text())
            assert len(output['tokenIds']) == 32 and output['finishReason'] == 'max_tokens'
            outputs[(label, index)] = output
            turns.append(dict(turn=index, wall_seconds=turn['wall_seconds'],
                backend_union_seconds=turn['backend_union_seconds'],
                backend_wall_share_percent=100 * turn['backend_union_seconds'] / turn['wall_seconds'],
                other_wall_seconds=turn['wall_seconds'] - turn['backend_union_seconds'],
                requester_cpu_seconds=turn['requester_cpu_seconds'],
                all_activation_latency_ms=distribution([o['ready_to_admitted_ms'] for o in selected]),
                decode_activation_latency_ms=distribution([o['ready_to_admitted_ms'] for o in decode]),
                decode_packed_bytes=distribution([o['packed_publication_payload_bytes'] for o in decode]),
                semantic_bytes=turn['semantic_bytes']))
        trials[label] = dict(dispatch_timing=measurement['dispatch_timing'], turns=turns,
            wrapper_wall_seconds=receipt['wall_seconds'],
            wrapper_child_cpu_seconds=receipt['child_cpu_seconds'],
            wrapper_outside_request_loops_seconds=receipt['wall_seconds'] -
                sum(turn['wall_seconds'] for turn in turns))
    equality = []
    for index in (0, 1):
        reference = outputs[('baseline-r1', index)]
        equality.append(dict(turn=index, all_four_output_json_equal=all(
            outputs[(label, index)] == reference for label in trials)))
    assert all(row['all_four_output_json_equal'] for row in equality), equality
    pairs = []
    for suffix in ('r1', 'r2'):
        before, after = trials['baseline-' + suffix], trials['treatment-' + suffix]
        for b, a in zip(before['turns'], after['turns']):
            pairs.append(dict(pair=suffix, turn=b['turn'],
                wall_change_seconds=a['wall_seconds'] - b['wall_seconds'],
                backend_union_change_seconds=a['backend_union_seconds'] - b['backend_union_seconds'],
                other_wall_change_seconds=a['other_wall_seconds'] - b['other_wall_seconds'],
                decode_latency_mean_change_ms=a['decode_activation_latency_ms']['mean'] -
                    b['decode_activation_latency_ms']['mean']))
    return dict(schema='ndnsf-di-dispatch-comparison-v1', trials=trials,
        output_equality=equality, paired_changes=pairs, workload_and_5ms_qdisc_equal=True,
        limits=['r1 diagnostics and r2 quiet are separate matched pairs, not pooled.',
                'One quiet pair does not establish statistical significance.',
                'Other wall time is the additive complement of backend interval union; it is not network time.',
                'Requester CPU overlaps wall and backend time and is not an additive partition.',
                'Wrapper outside request loops includes material preparation, launch, readiness and teardown; finer split unavailable.',
                'Output-ready epoch milliseconds are quantized; cross-clock results have one capture anchor.',
                'Decode subset uses the frozen workload size separation: 32 <10KB objects, one large prefill.',
                'p95 is nearest-rank ceil(.95*n); these are 32-token object-latency probes, not capacity tests.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = compare(args.root)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps(dict(output=str(args.output), paired_changes=result['paired_changes'],
                          output_equality=result['output_equality'])))
