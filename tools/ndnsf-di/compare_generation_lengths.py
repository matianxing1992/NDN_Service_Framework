#!/usr/bin/env python3
"""Compare quiet native warm lengths after an identical 32-token priming turn."""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import re
import statistics


def stats(values):
    values = sorted(values)
    if not values:
        return dict(count=0, status='UNAVAILABLE')
    return dict(count=len(values), mean=statistics.mean(values),
                median=statistics.median(values), p95=values[math.ceil(.95 * len(values)) - 1],
                minimum=values[0], maximum=values[-1], total=sum(values))


def backend_chain_partition(start, finish, calls):
    """Partition a sequential two-stage backend chain; never call its gaps network time."""
    calls = sorted(calls, key=lambda x: x['started_ns'])
    assert calls and all(start <= x['started_ns'] <= x['finished_ns'] <= finish for x in calls)
    if any(b['started_ns'] < a['finished_ns'] for a, b in zip(calls, calls[1:])):
        return dict(status='UNAVAILABLE_OVERLAPPING_BACKENDS')
    gaps = [dict(kind='before_first_backend', started_ns=start,
                 finished_ns=calls[0]['started_ns'])]
    for previous, following in zip(calls, calls[1:]):
        kind = 'other_between_backends'
        if (previous['role'] == '/LLM/Pipeline/Stage/0' and
                following['role'] == '/LLM/Pipeline/Stage/1' and
                previous['epoch'] == following['epoch']):
            kind = 'stage0_to_stage1_same_epoch'
        elif (previous['role'] == '/LLM/Pipeline/Stage/1' and
                following['role'] == '/LLM/Pipeline/Stage/0' and
                following['epoch'] == previous['epoch'] + 1):
            kind = 'stage1_to_stage0_next_epoch'
        gaps.append(dict(kind=kind, started_ns=previous['finished_ns'],
                         finished_ns=following['started_ns'], from_epoch=previous['epoch'],
                         to_epoch=following['epoch']))
    gaps.append(dict(kind='after_last_backend', started_ns=calls[-1]['finished_ns'], finished_ns=finish))
    backend_ns = sum(x['finished_ns'] - x['started_ns'] for x in calls)
    other_ns = sum(x['finished_ns'] - x['started_ns'] for x in gaps)
    assert backend_ns + other_ns == finish - start
    kinds = sorted({x['kind'] for x in gaps})
    return dict(status='ADDITIVE_SEQUENTIAL_CHAIN', backend_seconds=backend_ns / 1e9,
                other_seconds=other_ns / 1e9, gaps=gaps,
                categories_ms={kind: stats([(x['finished_ns'] - x['started_ns']) / 1e6
                                           for x in gaps if x['kind'] == kind]) for kind in kinds},
                scope='Gaps include all work/waits outside backend calls, not just transport; '
                      'fixed request overhead and step transitions are separate.')


def verify_context(turns, checkpoint):
    assert checkpoint['prefixTokenCount'] == 57, 'cold checkpoint prefix must be 25+32'
    assert turns[0]['generated_tokens'] == 32, 'cold output must have exactly32tokens'
    for turn in turns:
        for role in turn['roles']:
            initial = role['cache_observations'][0]
            cold = turn['turn'] == 0
            assert initial['representedPrefixTokenCount'] == (25 if cold else 81)
            assert initial['prefixWorkAvoided'] == (0 if cold else 57)
            assert initial['conversationStateHit'] == (not cold)
            if role['role'] == '/LLM/Pipeline/Stage/0':
                assert initial['actualNewInputExtent'] == (25 if cold else 24)


def compare(root, replicate='r1'):
    trials, outputs, reference = {}, {}, None
    for cap in (32, 128, 256):
        path = root / f'tokens{cap}-{replicate}'
        analysis = json.loads((path / 'analysis.json').read_text())
        measurement = json.loads((path / 'capture/measurement.json').read_text())
        receipt = json.loads((path / 'capture/receipt.json').read_text())
        assert receipt['status'] == 'PASS' and receipt['failure'] is None
        assert not measurement['dispatch_timing'] and not measurement['diagnostic_logging']
        assert 'at most32768frames' in measurement['capture']
        deployment = json.loads((path / 'deployment.json').read_text())
        identities = json.loads((path / 'binary-identities.json').read_text())
        options = [json.loads((path / f'inputs/{i}-options_file').read_text()) for i in (0, 1)]
        assert [option.pop('maxNewTokens') for option in options] == [32, cap]
        request = dict(deployment['requester']['request'])
        request.pop('options_file')
        matched = dict(model=deployment['model'], catalog=deployment['requester']['catalog'],
            placement=deployment['requester']['fixed_placement'], request=request,
            limits=deployment['requester']['limits'], options_without_cap=options,
            input_sha256=[hashlib.sha256((path / f'inputs/{i}-input_file').read_bytes()).hexdigest()
                          for i in (0, 1)], identities=identities,
            installed_manifest_sha256=hashlib.sha256(
                (path / 'installed-release-manifest.json').read_bytes()).hexdigest(),
            observer_sha256=measurement['source_sha256'],
            topology=deployment['platform']['topology_sha256'])
        if reference is None:
            reference = matched
        assert matched == reference, (cap, 'cohort inputs/options/code/config mismatch')
        qdisc = json.loads((path / 'capture/link-qdisc.json').read_text())
        assert qdisc['exit_code'] == 0
        netem = [x for x in json.loads(qdisc['stdout']) if x['kind'] == 'netem']
        assert len(netem) == 1 and netem[0]['options']['delay']['delay'] == .005
        providers = [json.loads(line) for f in (path / 'native').glob('provider-*-statistics.jsonl')
                     for line in f.read_text().splitlines()]
        requests = {row['turn']: row for row in
                    (json.loads(line) for line in (path / 'native/requester-statistics.jsonl').read_text().splitlines())
                    if row.get('type') == 'request'}
        phase_rows, onnx_rows = [], []
        for log in (path / 'native').glob('provider-*.log'):
            for line in log.read_text().splitlines():
                if 'NDNSF_DI_NATIVE_WORK_PHASE ' in line:
                    fields = dict(re.findall(r'(\w+)=([^ \n]+)', line))
                    phase_rows.append((log.name, fields))
                elif 'NDNSF_DI_ONNX_TIMING ' in line:
                    onnx_rows.append(dict(re.findall(r'(\w+)=([^ \n]+)', line)))
        turns = []
        for turn in analysis['turns']:
            index = turn['turn']
            output = json.loads((path / f'outputs/turn-{index}.bin').read_text())
            generated = len(output['tokenIds'])
            assert 0 < generated <= (32 if index == 0 else cap)
            outputs[(cap, index)] = output
            assert turn['backend_calls'] == 2 * (generated + 1)
            assert turn['admitted_objects'] == 2 * generated + 1
            assert turn['missing_producer_observations'] == turn['missing_complete_clocks'] == 0
            activation = [obj for obj in analysis['objects'] if obj['turn'] == index and
                          obj['producer_log'] == 'provider-0.log' and
                          obj.get('ready_to_admitted_ms') is not None]
            small = [obj for obj in activation if obj['packed_publication_payload_bytes'] < 10000]
            assert len(activation) == generated + 1 and len(small) == generated
            role_stats, backend_calls = [], []
            for row in providers:
                if row.get('requestId') != turn['request_id']:
                    continue
                metrics = row['runtimeMetrics']
                assert len(metrics) == generated + 1
                steps = []
                for metric in metrics:
                    timing = metric['lastBackendExecution']
                    wall = (timing['finishedSteadyNs'] - timing['startedSteadyNs']) / 1e6
                    cpu = (timing['finishedThreadCpuNs'] - timing['startedThreadCpuNs']) / 1e6
                    assert wall >= 0 and cpu >= 0
                    steps.append(dict(epoch=metric['inferenceEpoch'], wall_ms=wall, thread_cpu_ms=cpu))
                    backend_calls.append(dict(role=row['role'], epoch=metric['inferenceEpoch'],
                        started_ns=timing['startedSteadyNs'], finished_ns=timing['finishedSteadyNs']))
                steps.sort(key=lambda x: x['epoch'])
                assert [x['epoch'] for x in steps] == list(range(generated + 1))
                decode = steps[1:]
                quarter = max(1, math.ceil(len(decode) / 4))
                runner_timings = [x for x in onnx_rows if x['session'] == turn['request_id'] and
                                  x['role'] == row['role']]
                assert len(runner_timings) == generated + 1
                role_stats.append(dict(provider=row['provider'], role=row['role'], steps=steps,
                    provider_process_cpu_seconds=(row['finishedProcessCpuNs'] - row['startedProcessCpuNs']) / 1e9,
                    prefill_backend_ms=steps[0]['wall_ms'],
                    decode_backend_ms=stats([x['wall_ms'] for x in decode]),
                    first_quarter_decode_backend_ms=stats([x['wall_ms'] for x in decode[:quarter]]),
                    last_quarter_decode_backend_ms=stats([x['wall_ms'] for x in decode[-quarter:]]),
                    runner_nested_timing_ms={key: stats([float(x[key]) for x in runner_timings])
                                             for key in ('collect_ms', 'run_ms', 'delay_ms', 'publish_ms')},
                    cache_observations=row['cacheObservations']))
            assert len(role_stats) == 2
            phases = defaultdict(list)
            for log, fields in phase_rows:
                if fields.get('requestId') == turn['request_id']:
                    phases[(log, fields['phase'])].append(fields)
            phase_stats = [dict(provider_log=log, phase=phase,
                wall_us=stats([int(x['duration_us']) for x in values]),
                thread_cpu_us=stats([int(x['thread_cpu_us']) for x in values if 'thread_cpu_us' in x]))
                for (log, phase), values in sorted(phases.items())]
            wall, backend = turn['wall_seconds'], turn['backend_union_seconds']
            partition = backend_chain_partition(requests[index]['startedSteadyNs'],
                                                requests[index]['finishedSteadyNs'], backend_calls)
            if partition['status'] == 'ADDITIVE_SEQUENTIAL_CHAIN':
                assert abs(partition['backend_seconds'] - backend) < 1e-8
            turns.append(dict(turn=index, generated_tokens=generated, finish_reason=output['finishReason'],
                requested_cap=32 if index == 0 else cap, wall_seconds=wall,
                backend_union_seconds=backend, backend_share_percent=100 * backend / wall,
                other_wall_seconds=wall - backend, requester_cpu_seconds=turn['requester_cpu_seconds'],
                wall_ms_per_generated_token=1000 * wall / generated,
                backend_ms_per_generated_token=1000 * backend / generated,
                other_ms_per_generated_token=1000 * (wall - backend) / generated,
                activation_latency_ms=stats([x['ready_to_admitted_ms'] for x in activation]),
                small_activation_latency_ms=stats([x['ready_to_admitted_ms'] for x in small]),
                semantic_bytes=turn['semantic_bytes'], native_phases=phase_stats,
                roles=role_stats, backend_chain_partition=partition))
        checkpoint = json.loads((path / 'outputs/checkpoint-0.json').read_text())
        verify_context(turns, checkpoint)
        turns[0]['checkpoint_prefix_count'] = checkpoint['prefixTokenCount']
        turns[0]['checkpoint_prefix_digest'] = checkpoint['logicalPrefixDigest']
        trials[str(cap)] = dict(turns=turns, wrapper_wall_seconds=receipt['wall_seconds'],
            wrapper_child_cpu_seconds=receipt['child_cpu_seconds'],
            wrapper_outside_request_loops_seconds=receipt['wall_seconds'] - sum(t['wall_seconds'] for t in turns))
    assert all(outputs[(cap, 0)] == outputs[(32, 0)] for cap in (128, 256)), 'priming output mismatch'
    assert len({trials[str(cap)]['turns'][0]['checkpoint_prefix_digest']
                for cap in (32, 128, 256)}) == 1, 'cold checkpoint context digest mismatch'
    cache_starts = {cap: sorted((row['role'], row['cache_observations'][0])
                   for row in trials[str(cap)]['turns'][1]['roles']) for cap in (32, 128, 256)}
    assert cache_starts[32] == cache_starts[128] == cache_starts[256], 'warm initial cache context mismatch'
    prefix_checks = []
    for small_cap, large_cap in ((32, 128), (128, 256)):
        small, large = outputs[(small_cap, 1)]['tokenIds'], outputs[(large_cap, 1)]['tokenIds']
        prefix_checks.append(dict(short_cap=small_cap, long_cap=large_cap,
                                  token_prefix_equal=large[:len(small)] == small))
    assert all(row['token_prefix_equal'] for row in prefix_checks), 'warm generation prefix mismatch'
    common_prefix_backend = []
    for count in (31, 127):
        selected = {}
        for cap in (32, 128, 256):
            if cap <= count:
                continue
            steps = [step for role in trials[str(cap)]['turns'][1]['roles']
                     for step in role['steps'] if 1 <= step['epoch'] <= count]
            assert len(steps) == 2 * count
            selected[str(cap)] = dict(backend_call_wall_seconds=sum(x['wall_ms'] for x in steps) / 1000,
                backend_thread_cpu_seconds=sum(x['thread_cpu_ms'] for x in steps) / 1000)
        common_prefix_backend.append(dict(first_decode_epoch=1, last_decode_epoch=count,
                                         excludes_priming_prefill_and_terminal_pass=True, caps=selected))
    return dict(schema='ndnsf-di-warm-length-comparison-v1', replicate=replicate, trials=trials,
        context_contract=dict(cold_input_tokens=25, cold_generated_tokens=32,
                              checkpoint_prefix_tokens=57, warm_added_tokens=24,
                              warm_initial_context_tokens=81),
        priming_outputs_equal=True, warm_initial_cache_equal=True, warm_prefix_checks=prefix_checks,
        full_requested_lengths=all(trials[str(cap)]['turns'][1]['generated_tokens'] == cap
                                   for cap in (32, 128, 256)),
        common_prefix_backend=common_prefix_backend,
        limits=['One trial per cap; no statistical significance claim.',
                'Backend union and other partition request wall; CPU/native phases overlap and are not additive.',
                'Epoch0 is prefill; small steps include the final checkpoint-state pass.',
                'Fixed priming output/context is checked; warm output length is the intended variable.',
                'Wall per token includes prefill and request overhead, not pure steady decode.',
                'Runner collect/package timings are outside engine; run_ms is a broader wrapper, not backend union.',
                'Native phase timing records only existing instrumented phases, not all forwarding/scheduler waits.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--replicate', choices=('r1', 'r2'), default='r1')
    args = parser.parse_args()
    result = compare(args.root, args.replicate)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps(dict(output=str(args.output), full_requested_lengths=result['full_requested_lengths'],
        warm=[dict(cap=cap, **{k:v for k,v in row['turns'][1].items() if k in
                  ('generated_tokens', 'finish_reason', 'wall_seconds', 'backend_union_seconds',
                   'backend_share_percent', 'other_wall_seconds', 'wall_ms_per_generated_token')})
              for cap,row in result['trials'].items()])))
