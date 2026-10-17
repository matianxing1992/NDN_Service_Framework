#!/usr/bin/env python3
"""Validate and compare the new four-way cohort without historical pooling."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import statistics
import random
from analyze import quantile, read_rows

ARMS = ['BASIC', 'TARGETED_STEADY', 'GRPC', 'NSC']


def cell(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    failures, results = [], []
    if manifest['status'] != 'RUN_COMPLETE' or manifest.get('cleanup_errors'):
        failures.append('incomplete run or cleanup')
    if manifest.get('measurement_family') != 'four-way':
        failures.append('wrong measurement family')
    if (manifest['timeout_ms'] != 5000 or manifest['payload_bytes'] != 64
            or manifest['business_delay_ms'] != 5
            or not manifest.get('smoke') and (manifest['window_seconds'] != 60 or manifest['warmup_seconds'] != 10)):
        failures.append('workload contract mismatch')
    runs = [r for r in manifest['runs'] if r.get('purpose') == 'performance']
    expected_order = ['basic', 'targeted', 'grpc', 'nsc']
    offset = (manifest['block_seed'] - 20301) % 4
    expected_order = expected_order[offset:] + expected_order[:offset]
    if manifest.get('arm_order') != expected_order:
        failures.append('unbalanced four-arm order')
    arm_names = {'BASIC': 'basic', 'TARGETED_STEADY': 'targeted', 'GRPC': 'grpc', 'NSC': 'nsc'}
    if [arm_names.get(r['arm']) for r in runs] != expected_order:
        failures.append('actual performance order mismatch')
    if Counter(r['arm'] for r in runs) != Counter(ARMS):
        failures.append('missing/duplicate four-arm cohort')
    ready_times = []
    for run in runs:
        rows = read_rows(directory / run['records'])
        ready = [r for r in rows if r['event'] == 'ready']
        shutdown = [r for r in rows if r['event'] == 'shutdown']
        terminals = [r for r in rows if r['event'] == 'terminal']
        if len(ready) != 1 or len(shutdown) != 1:
            failures.append(run['id'] + ': readiness/shutdown')
            continue
        if shutdown[0]['attempts'] != len(terminals) or len({r['command'] for r in terminals}) != len(terminals):
            failures.append(run['id'] + ': terminal cardinality')
        t0, t1 = ready[0]['t0_ns'], ready[0]['t1_ns']
        ready_times.append(ready[0].get('ready_ns', t0))
        if run['arm'] in ['BASIC', 'TARGETED_STEADY'] and ready[0].get('mode') != arm_names[run['arm']]:
            failures.append(run['id'] + ': native mode mismatch')
        if t0 - ready[0].get('ready_ns', t0) != manifest['warmup_seconds']*1000000000:
            failures.append(run['id'] + ': actual warmup mismatch')
        if t1-t0 != manifest['window_seconds']*1000000000:
            failures.append(run['id'] + ': window mismatch')
        if run['arm'] in ['GRPC', 'NSC']:
            provider_rows = read_rows(directory / run['provider_records'])
            target = 'drone1' if run['arm'] == 'GRPC' else '/muas/drone1'
            if (len([r for r in provider_rows if r['event'] == 'ready']) != 1
                    or len([r for r in provider_rows if r['event'] == 'shutdown']) != 1):
                failures.append(run['id'] + ': provider readiness')
            if run['arm'] == 'GRPC' and manifest.get('tls_public_sha256') != manifest['candidate']['python_runtime'].get('tls_certificates'):
                failures.append(run['id'] + ': TLS identity mismatch')
        else:
            owners = [o for o in manifest['provider_records'] if o['group'] == run['sync_group']]
            if len(owners) != 2:
                failures.append(run['id'] + ': missing native provider pair')
            provider_rows = [r for o in owners for r in read_rows(directory / o['records'])]
            for owner in owners:
                evidence = read_rows(directory / owner['records'])
                if (len([r for r in evidence if r['event'] == 'ready']) != 1
                        or len([r for r in evidence if r['event'] == 'shutdown']) != 1):
                    failures.append(owner['label'] + ': native owner lifecycle')
            target = '/example/hello/provider/A'
        executions = defaultdict(list)
        for r in provider_rows:
            if r['event'] == 'execution':
                executions[r['command']].append(r)
        measured = [r for r in terminals if r['measured']]
        modes = Counter()
        if not measured:
            failures.append(run['id'] + ': empty cohort')
        if len([c for c in manifest['children'] if c['label'] == run['id'] and c['exit'] == 0]) != 1:
            failures.append(run['id'] + ': requester exit')
        for r in terminals:
            handlers = executions[r['command']]
            if r['target'] != target or any(h['provider'] != target for h in handlers):
                failures.append(run['id'] + ': wrong target')
            if len(r['command'].encode()) != 64:
                failures.append(run['id'] + ': payload size')
            if r['deadline_ns'] != r['begin_ns'] + 5000000000:
                failures.append(run['id'] + ': deadline mismatch')
            if r['terminal_ns'] < r['begin_ns']:
                failures.append(run['id'] + ': terminal before submission')
            if r['measured'] != (t0 <= r['begin_ns'] < t1) or r['begin_ns'] >= t1:
                failures.append(run['id'] + ': cohort mismatch')
            if r['begin_ns'] < ready[0].get('ready_ns', t0):
                failures.append(run['id'] + ': submission before reported readiness')
            if r['terminal_ns'] > t1 + 7000000000:
                failures.append(run['id'] + ': drain exceeded')
            if r['terminal'] == 'success':
                if (r['terminal_ns'] >= r['deadline_ns'] or not r.get('payload_verified')
                        or len(handlers) != 1 or not handlers[0]['payload_valid']):
                    failures.append(run['id'] + ': invalid successful result oracle')
                if len(handlers) == 1 and r['measured'] and 'actual_mode' in handlers[0]:
                    modes[handlers[0]['actual_mode']] += 1
            if len(handlers) > 1:
                failures.append(run['id'] + ': duplicate handler execution')
        counts = Counter(r['terminal'] for r in measured)
        successes = [r for r in measured if r['terminal'] == 'success']
        latencies = [(r['terminal_ns']-r['begin_ns'])/1000000 for r in successes]
        observed_successes = [r for r in terminals if r['terminal'] == 'success']
        observed_counts = Counter(r['terminal'] for r in terminals)
        first = min(terminals, key=lambda r: r['begin_ns']) if terminals else None
        first_ms = (first['terminal_ns']-first['begin_ns'])/1000000 if first else None
        def mean_delay(records):
            return statistics.mean((r['terminal_ns']-r['begin_ns'])/1000000
                                   for r in records) if records else None
        spans = []
        for r in successes:
            handlers = executions[r['command']]
            if len(handlers) == 1:
                h = handlers[0]
                if not r['begin_ns'] <= h['begin_ns'] <= h['end_ns'] <= r['terminal_ns']:
                    failures.append(run['id'] + ': nonmonotonic correlated span')
                spans.append([(h['begin_ns']-r['begin_ns'])/1000000,
                              (h['end_ns']-h['begin_ns'])/1000000,
                              (r['terminal_ns']-h['end_ns'])/1000000])
        results.append({'arm': run['arm'], 'attempts': len(measured), 'success': len(successes),
                        'timeout': counts['timeout'], 'error': len(measured)-len(successes)-counts['timeout'],
                        'p50_ms': quantile(latencies, .5), 'p95_ms': quantile(latencies, .95),
                        'mean_ms': mean_delay(successes),
                        'terminal_mean_ms': mean_delay(measured),
                        'observation_mean_ms': mean_delay(observed_successes),
                        'observation_terminal_mean_ms': mean_delay(terminals),
                        'observation_attempts': len(terminals),
                        'observation_success': len(observed_successes),
                        'observation_timeout': observed_counts['timeout'],
                        'observation_error': len(terminals)-len(observed_successes)-observed_counts['timeout'],
                        'first_call_terminal': first['terminal'] if first else None,
                        'first_call_latency_ms': first_ms,
                        'first_success_ms': first_ms if first and first['terminal'] == 'success' else None,
                        'rate': len(successes)/manifest['window_seconds'],
                        'elapsed_spans_ms': [statistics.mean(s[i] for s in spans) for i in range(3)] if spans else None,
                        'max_timeout_lateness_ms': max([max(0, r['terminal_ns']-r['deadline_ns'])/1000000
                            for r in measured if r['terminal'] == 'timeout'], default=0),
                        'actual_mode_counts': dict(modes),
                        'p95_quality': 'LOW_SAMPLE_COUNT' if len(successes) < 100 else 'SUFFICIENT'})
    if any(b <= a for a, b in zip(ready_times, ready_times[1:])):
        failures.append('observed ready timestamps contradict arm order')
    if not manifest.get('controller_writer_released_after_wait'):
        failures.append('Controller writer cleanup')
    for child in manifest['children']:
        if child['exit'] != (-2 if child['label'] == 'controller' else 0):
            failures.append(child['label'] + ': abnormal child exit')
    return {'directory': str(directory), 'delay_ms': manifest['delay_ms'],
            'loss': manifest['loss_percent'], 'seed': manifest['block_seed'], 'smoke': manifest['smoke'],
            'candidate': hashlib.sha256(json.dumps(manifest['candidate'], sort_keys=True).encode()).hexdigest(),
            'qualified': not failures, 'failures': failures, 'runs': results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    paths = [args.input] if (args.input/'manifest.json').exists() else sorted(p.parent for p in args.input.glob('*/manifest.json'))
    cells = [cell(p) for p in paths]
    expected = {(d, loss, seed) for d, loss in [(1,0),(10,0),(50,0),(10,1)] for seed in range(20301,20306)}
    observed = {(c['delay_ms'], c['loss'], c['seed']) for c in cells if c['qualified'] and not c['smoke']}
    campaign_path = args.input/'campaign.json'
    campaign = json.loads(campaign_path.read_text()) if campaign_path.exists() else {}
    expected_names = {'d{}-loss{}-b{}'.format(d, loss, seed) for d, loss, seed in expected}
    frozen_candidate = hashlib.sha256(json.dumps(campaign.get('candidate'), sort_keys=True).encode()).hexdigest()
    campaign_complete = (campaign.get('status') == 'RUN_COMPLETE'
                         and campaign.get('measurement_family') == 'four-way'
                         and len(campaign.get('completed', [])) == 20
                         and set(campaign.get('completed', [])) == expected_names
                         and all(c['candidate'] == frozen_candidate for c in cells)
                         and not campaign.get('first_failure'))
    status = 'PASS' if (campaign_complete and observed == expected and len(cells) == 20
                        and len({c['candidate'] for c in cells}) == 1) else 'PARTIAL'
    groups = defaultdict(list)
    for c in cells:
        if c['qualified']:
            for row in c['runs']:
                groups[(c['delay_ms'], c['loss'], row['arm'])].append(row)
    aggregates = []
    for (delay, loss, arm), rows in sorted(groups.items()):
        aggregates.append({'delay_ms': delay, 'loss': loss, 'arm': arm, 'blocks': len(rows),
            'latency_blocks': sum(r['p50_ms'] is not None for r in rows),
            'first_success_blocks': sum(r['first_success_ms'] is not None for r in rows),
            'first_call_outcomes': dict(Counter(r['first_call_terminal'] for r in rows)),
            'p95_quality': 'LOW_SAMPLE_COUNT' if any(r['p95_quality'] == 'LOW_SAMPLE_COUNT' for r in rows) else 'SUFFICIENT',
            'elapsed_spans_ms': [statistics.mean(r['elapsed_spans_ms'][i] for r in rows if r['elapsed_spans_ms'] is not None)
                for i in range(3)] if any(r['elapsed_spans_ms'] is not None for r in rows) else None,
            'max_timeout_lateness_ms': max(r['max_timeout_lateness_ms'] for r in rows),
            **{key: sum(r[key] for r in rows) for key in ['attempts', 'success', 'timeout', 'error',
                'observation_attempts', 'observation_success', 'observation_timeout', 'observation_error']},
            **{key: statistics.mean(r[key] for r in rows if r[key] is not None)
               if any(r[key] is not None for r in rows) else None for key in ['p50_ms', 'p95_ms', 'rate',
                'mean_ms', 'terminal_mean_ms', 'observation_mean_ms', 'observation_terminal_mean_ms',
                'first_call_latency_ms', 'first_success_ms']}})
    paired = []
    paired_means = []
    for delay, loss in [(1,0),(10,0),(50,0),(10,1)]:
        selected = [c for c in cells if c['qualified'] and not c['smoke'] and (c['delay_ms'],c['loss']) == (delay,loss)]
        for reference in ['BASIC', 'GRPC', 'NSC']:
            for metric, description in [('p50_ms', 'block median'),
                                        ('mean_ms', 'successful block mean'),
                                        ('terminal_mean_ms', 'all-attempt terminal block mean')]:
                values = []
                for c in selected:
                    arms = {r['arm']: r for r in c['runs']}
                    if arms[reference][metric] is not None and arms['TARGETED_STEADY'][metric] is not None:
                        values.append(arms[reference][metric]-arms['TARGETED_STEADY'][metric])
                if values:
                    rng = random.Random(20399)
                    resamples = [statistics.mean(rng.choices(values, k=len(values))) for _ in range(10000)]
                    (paired if metric == 'p50_ms' else paired_means).append({
                        'delay_ms': delay, 'loss': loss, 'reference': reference, 'metric_key': metric,
                        'metric': 'reference minus targeted ' + description + ' (ms)', 'blocks': len(values),
                        'mean': statistics.mean(values), 'ci95': [quantile(resamples,.025), quantile(resamples,.975)],
                        'resampling_unit': 'paired block', 'bootstrap_seed': 20399})
    summary = {'status': status, 'cells': cells, 'aggregates': aggregates, 'paired': paired,
               'paired_means': paired_means,
               'mean_scope': {'aggregation': 'equal-weight arithmetic mean of contributing block means',
                              'measured': '60s submission window, successes and all-attempt terminals separate',
                              'observation': 'all submissions after reported readiness including 10s warmup',
                              'first_call': 'first API submission after readiness; prior setup is excluded'},
               'analysis_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps({'status': status, 'cells': len(cells), 'qualified': sum(c['qualified'] for c in cells),
                      'failures': [c['failures'] for c in cells if c['failures']]}))


if __name__ == '__main__':
    main()
