#!/usr/bin/env python3
"""Analyze native JSONL and single-endpoint pcap without inventing missing data."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path
import random
import re
import statistics
import struct


def read_rows(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def quantile(values, probability):
    if not values:
        return None
    ordered = sorted(values)
    index = (len(ordered) - 1) * probability
    low = int(index)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (index - low)


def pcap_counts(path, anchor, ready, t0, t1, end):
    scopes = {scope: {'packets': 0, 'wire_bytes': 0} for scope in
              ['readiness', 'warmup', 'measurement', 'drain', 'post_drain', 'total']}
    datagrams = {}
    recent = {}
    completed = 0
    def account(frames):
        for mono, wire in frames:
            scope = 'readiness' if mono < ready else 'warmup' if mono < t0 else 'measurement' if mono < t1 else 'drain' if mono <= end else 'post_drain'
            for name in ['total', scope]:
                scopes[name]['packets'] += 1
                scopes[name]['wire_bytes'] += wire
    with path.open('rb') as stream:
        header = stream.read(24)
        variants = {b'\xd4\xc3\xb2\xa1': ('<', 1000), b'\xa1\xb2\xc3\xd4': ('>', 1000),
                    b'\x4d\x3c\xb2\xa1': ('<', 1), b'\xa1\xb2\x3c\x4d': ('>', 1)}
        if header[:4] not in variants or len(header) != 24:
            raise ValueError('unsupported/truncated pcap')
        endian, fraction = variants[header[:4]]
        if struct.unpack(endian + 'I', header[20:24])[0] != 1:
            raise ValueError('capture requires Ethernet framing')
        while True:
            record = stream.read(16)
            if not record:
                break
            if len(record) != 16:
                raise ValueError('truncated pcap record')
            seconds, subsecond, captured, wire = struct.unpack(endian + 'IIII', record)
            packet = stream.read(captured)
            if len(packet) != captured or captured != wire:
                raise ValueError('truncated capture')
            mono = seconds * 1000000000 + subsecond * fraction - anchor['wall_ns'] + anchor['mono_ns']
            if len(packet) < 34 or packet[12:14] != b'\x08\x00':
                continue
            ip = packet[14:]
            size = (ip[0] & 15) * 4
            total = int.from_bytes(ip[2:4], 'big')
            if ip[0] >> 4 != 4 or size < 20 or total < size or len(ip) < total:
                raise ValueError('truncated/invalid IPv4 packet')
            if ip[9] != 17:
                continue
            flags = int.from_bytes(ip[6:8], 'big')
            offset, more = (flags & 8191) * 8, bool(flags & 8192)
            payload = ip[size:total]
            key = (ip[12:20], ip[9], ip[4:6])
            recent = {k: v for k, v in recent.items() if mono - v['completed_ns'] <= 30000000000}
            if not offset and not more:
                if len(payload) < 8 or int.from_bytes(payload[4:6], 'big') != len(payload):
                    raise ValueError('invalid UDP length')
                if 6363 in struct.unpack('!HH', payload[:4]):
                    account([(mono, wire)])
                continue
            previous = recent.get(key)
            if previous and previous['parts'].get(offset) == payload:
                if previous['ndn']:
                    account([(mono, wire)])
                continue
            if previous:
                # A new initial fragment establishes IPv4-ID reuse. A changed
                # noninitial fragment is ambiguous and cannot prove coverage.
                if offset:
                    raise ValueError('ambiguous reused ID or conflicting late fragment')
                del recent[key]
            state = datagrams.setdefault(key, {'parts': {}, 'frames': [], 'last': None, 'first_ns': mono})
            if abs(mono - state['first_ns']) > 30000000000:
                raise ValueError('fragment ID reuse or stale incomplete datagram')
            if more and (not payload or len(payload) % 8):
                raise ValueError('invalid fragment alignment')
            for pos, data in state['parts'].items():
                lo, hi = max(pos, offset), min(pos + len(data), offset + len(payload))
                if lo < hi and data[lo-pos:hi-pos] != payload[lo-offset:hi-offset]:
                    raise ValueError('conflicting IPv4 fragments')
            if offset in state['parts'] and state['parts'][offset] != payload:
                raise ValueError('conflicting fragment extent')
            state['parts'][offset] = payload
            state['frames'].append((mono, wire))
            if not more:
                last = offset + len(payload)
                if state['last'] is not None and state['last'] != last:
                    raise ValueError('conflicting final fragments')
                state['last'] = last
            if state['last'] is None or 0 not in state['parts']:
                continue
            extent = 0
            for pos, data in sorted(state['parts'].items()):
                if pos > extent:
                    break
                extent = max(extent, pos + len(data))
            if extent != state['last']:
                continue
            assembled = bytearray(extent)
            for pos, data in state['parts'].items():
                assembled[pos:pos+len(data)] = data
            if extent < 8 or int.from_bytes(assembled[4:6], 'big') != extent:
                raise ValueError('reassembled UDP length mismatch')
            state['ndn'] = 6363 in struct.unpack('!HH', assembled[:4])
            if state['ndn']:
                account(state['frames'])
            state['completed_ns'] = mono
            recent[key] = state
            completed += 1
            del datagrams[key]
    if datagrams:
        raise ValueError('incomplete IPv4 datagrams: ' + str(len(datagrams)))
    scopes['coverage'] = {'status': 'PASS', 'reassembled_datagrams': completed}
    return scopes


def summarize_cell(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    failures = []
    wire_failures = []
    if (manifest.get('history_isolation') !=
            'fresh-sync-group-and-ready-provider-pair-per-performance-run' or
            not manifest.get('provider_records')):
        failures.append('clean-history qualification requires isolated groups and provider owners')
    provider_records = manifest.get('provider_records', [
        {'label': name, 'records': name + '.jsonl'} for name in ['drone1', 'drone2']])
    for owner in provider_records:
        name = owner['label']
        provider_rows = read_rows(directory / owner['records'])
        if sum(r['event'] == 'ready' for r in provider_rows) != 1 or sum(
                r['event'] == 'shutdown' and r.get('ready') is True for r in provider_rows) != 1:
            failures.append(name + ': missing complete independent provider records')
        exits = [child['exit'] for child in manifest.get('children', []) if child['label'] == name]
        if exits != [0]:
            failures.append(name + ': missing/nonzero child exit')
    executions = [row for owner in provider_records for row in
                  read_rows(directory / owner['records']) if row['event'] == 'execution']
    by_command = defaultdict(list)
    for row in executions:
        by_command[row['command']].append(row)
    results = []
    groups_seen = set()
    for run in manifest['runs']:
        if manifest.get('history_isolation'):
            group = run.get('sync_group')
            owners = [o for o in provider_records if o.get('group') == group]
            if (not group or len(owners) != 2 or
                    {o['identity'] for o in owners} != {'/example/hello/provider/A', '/example/hello/provider/B'}):
                failures.append(run['id'] + ': missing isolated provider pair')
            if run.get('purpose') == 'performance':
                if group in groups_seen:
                    failures.append(run['id'] + ': reused performance Sync history')
                groups_seen.add(group)
        rows = read_rows(directory / run['records'])
        ready = [row for row in rows if row['event'] == 'ready']
        shutdown = [row for row in rows if row['event'] == 'shutdown']
        terminals = [row for row in rows if row['event'] == 'terminal']
        if len(ready) != 1 or len(shutdown) != 1:
            failures.append(run['id'] + ': missing readiness/shutdown')
            continue
        end = shutdown[0]
        if end['attempts'] != len(terminals) or len({r['command'] for r in terminals}) != len(terminals):
            failures.append(run['id'] + ': terminal cardinality mismatch')
        for row in terminals:
            deadline = row.get('deadline_ns')
            if deadline != row['begin_ns'] + manifest['timeout_ms'] * 1000000:
                failures.append(run['id'] + ': missing/incorrect independent deadline')
            elif row['terminal'] == 'success' and row['terminal_ns'] > deadline:
                failures.append(run['id'] + ': success after deadline')
        measured = [r for r in terminals if r['measured']]
        t0, t1 = ready[0]['t0_ns'], ready[0]['t1_ns']
        cold = run['arm'] == 'TARGETED_FIRST_CALL'
        if not cold and any(not (t0 <= r['begin_ns'] < t1) for r in measured):
            failures.append(run['id'] + ': submission outside cohort')
        if not cold and any(r['terminal_ns'] > t1 + 7000000000 for r in measured):
            failures.append(run['id'] + ': terminal exceeded bounded drain')
        counts = Counter(r['terminal'] for r in measured)
        successful = [r for r in measured if r['terminal'] == 'success']
        latency = [(r['terminal_ns'] - r['begin_ns']) / 1000000 for r in successful]
        handler_ms, modes = [], Counter()
        spans = []
        for row in terminals:
            handlers = by_command[row['command']]
            if any(h['provider'] != row['target'] for h in handlers):
                failures.append(run['id'] + ': wrong target execution')
            if row['terminal'] == 'success':
                if len(handlers) != 1 or not row.get('payload_verified') or not row.get('wire_digest'):
                    failures.append(run['id'] + ': missing/duplicate/invalid independent handler evidence')
                if handlers and row['measured']:
                    handler_ms.append((handlers[0]['end_ns'] - handlers[0]['begin_ns']) / 1000000)
                    modes[handlers[0]['actual_mode']] += 1
                    h = handlers[0]
                    span = [(h['begin_ns'] - row['begin_ns']) / 1000000,
                            (h['end_ns'] - h['begin_ns']) / 1000000,
                            (row['terminal_ns'] - h['end_ns']) / 1000000]
                    if min(span) < 0:
                        failures.append(run['id'] + ': invalid monotonic span ordering')
                    spans.append(span)
        if cold and successful and all(by_command[r['command']][0]['actual_mode'] == 2 for r in successful
                                       if len(by_command[r['command']]) == 1):
            run['arm'] = 'TARGETED_BOOTSTRAP'
        duration = manifest['window_seconds']
        result = {'cell': directory.name, 'run': run['id'], 'arm': run['arm'],
                  'seed': manifest['block_seed'], 'delay_ms': manifest['delay_ms'],
                  'loss_percent': manifest['loss_percent'], 'smoke': manifest['smoke'],
                  'purpose': run.get('purpose', 'performance'),
                  'attempts': len(measured), 'success': len(successful),
                  'timeout': counts['timeout'], 'error': len(measured) - len(successful) - counts['timeout'],
                  'p50_ms': quantile(latency, .5), 'p95_ms': quantile(latency, .95),
                  'p95_quality': 'LOW_SAMPLE_COUNT' if len(successful) < 100 else 'SUFFICIENT',
                  'success_ratio': len(successful) / len(measured) if measured else None,
                  'completion_rate': len(successful) / duration if not cold else None,
                  'in_window_rate': sum(r['terminal_ns'] < t1 for r in successful) / duration if not cold else None,
                  'handler_p50_ms': quantile(handler_ms, .5), 'actual_mode_counts': dict(modes),
                  'caller_observed_late_callbacks': sum(r['event'] == 'late_native_terminal' for r in rows),
                  'caller_observed_duplicate_callbacks': sum(r['event'] in ['duplicate_observed', 'duplicate_native_terminal'] for r in rows),
                  'deadline_dispatch_lateness_max_ms': max(
                      [max(0, (r['terminal_ns'] - r['deadline_ns']) / 1000000)
                       for r in measured if r['terminal'] == 'timeout' and 'deadline_ns' in r], default=None),
                  'fine_protocol_counts': 'NOT_MEASURED', 'fast_subset': 'NOT_MEASURED'}
        result['elapsed_span_mean_ms'] = dict(zip(
            ['submit_to_handler', 'handler', 'handler_to_terminal'],
            [statistics.mean(s[i] for s in spans) for i in range(3)])) if spans else None
        if len(measured) == 0:
            failures.append(run['id'] + ': empty measurement cohort')
        if counts['invalid_response'] or end['invalid_responses']:
            failures.append(run['id'] + ': invalid response')
        if run.get('purpose') == 'oracle-negative':
            if len(measured) != 1 or counts['response_error'] != 1 or successful or counts['timeout']:
                failures.append(run['id'] + ': expected negative response not preserved')
            elif len(by_command[measured[0]['command']]) != 1 or by_command[measured[0]['command']][0]['payload_valid']:
                failures.append(run['id'] + ': missing independent negative handler evidence')
        capture = manifest.get('capture', {}).get(run['id'])
        required_capture = not cold and not manifest['smoke'] and manifest['delay_ms'] == 10 and manifest['loss_percent'] == 0
        if capture:
            log = (directory / (run['id'] + '-capture.log')).read_text(errors='replace')
            drops = re.findall(r'(\d+) packets dropped by kernel', log)
            if len(drops) != 1 or int(drops[0]) != 0:
                wire_failures.append(run['id'] + ': unknown/nonzero capture drop count')
            result['capture_drop_count'] = int(drops[0]) if drops else None
            ready_ns = t0 - manifest['warmup_seconds'] * 1000000000
            end_ns = max([r['terminal_ns'] for r in terminals] + [t1]) + 2000000000
            try:
                if (capture.get('filter') != 'ip' or capture.get('guard_ns') != 250000000
                        or capture.get('started_ns', ready_ns) + 250000000 > ready_ns
                        or capture.get('stopped_ns', 0) < end_ns + 250000000):
                    raise ValueError('capture filter/guard coverage not proven')
                result['wire'] = pcap_counts(directory / capture['pcap'], manifest['clock_anchor'],
                                             ready_ns, t0, t1, end_ns)
                result['window_bytes_per_attempt'] = result['wire']['measurement']['wire_bytes'] / len(measured)
                result['window_bytes_per_second'] = result['wire']['measurement']['wire_bytes'] / duration
            except ValueError as error:
                wire_failures.append(run['id'] + ': ' + str(error))
                result['wire_status'] = 'PARTIAL'
        elif required_capture:
            wire_failures.append(run['id'] + ': required capture missing')
        results.append(result)
    if manifest.get('isolation'):
        isolation = manifest['isolation']
        positive = [r for r in isolation['positive'] if r['event'] == 'terminal']
        offline = [r for r in isolation['offline'] if r['event'] == 'terminal']
        commands = {r['command'] for r in positive + offline}
        wrong = [r for r in executions if r['command'] in commands and r['provider'] != '/example/hello/provider/A']
        if len(positive) != 1 or positive[0]['terminal'] != 'success' or len(offline) != 1 or offline[0]['terminal'] == 'success' or wrong:
            failures.append('target isolation failed')
    if manifest['status'] != 'RUN_COMPLETE' or manifest.get('cleanup_errors'):
        failures.append('incomplete run/cleanup')
    probe_rows = read_rows(directory / 'rtt.jsonl')
    probe = [r for r in probe_rows if r['event'] == 'rtt_terminal' and r['success']]
    if len(probe_rows) != 10 or not probe:
        failures.append('missing ten completed probes or signed Data baseline')
    if any(r.get('invalid', r.get('success') is not True) for r in probe_rows):
        failures.append('invalid signed Data probe response')
    return {'directory': str(directory), 'qualified': not failures and not wire_failures,
            'measurement_family': manifest.get('measurement_family', 'primary'),
            'latency_qualified': not failures, 'failures': failures, 'wire_failures': wire_failures,
            'candidate_hash': hashlib.sha256(json.dumps(manifest['candidate'], sort_keys=True).encode()).hexdigest(),
            'isolation': 'PASS' if manifest.get('isolation') and not failures else 'NOT_MEASURED',
            'rtt_p50_ms': quantile([(r['terminal_ns'] - r['begin_ns']) / 1000000 for r in probe], .5),
            'rtt_attempts': len(probe_rows), 'rtt_successes': len(probe),
            'rtt_errors': len(probe_rows) - len(probe),
            'runs': results}


def paired_statistics(cells):
    cells = [cell for cell in cells if cell.get('measurement_family')
             not in {'diagnostic', 'descriptor-qualification'}]
    candidates = {cell['candidate_hash'] for cell in cells}
    if len(candidates) > 1:
        raise ValueError('cannot combine different candidates')
    grouped = defaultdict(dict)
    for cell in cells:
        if not cell['latency_qualified']:
            continue
        for row in cell['runs']:
            if not row['smoke'] and row['arm'] in ['BASIC', 'TARGETED_STEADY']:
                key = (row['delay_ms'], row['loss_percent'], row['seed'])
                if row['arm'] in grouped[key]:
                    raise ValueError('duplicate candidate cell')
                grouped[key][row['arm']] = row
    differences = defaultdict(lambda: defaultdict(list))
    for (delay, loss, seed), arms in sorted(grouped.items()):
        if set(arms) != {'BASIC', 'TARGETED_STEADY'}:
            continue
        for metric in ['p50_ms', 'p95_ms', 'success_ratio', 'completion_rate']:
            basic, targeted = arms['BASIC'][metric], arms['TARGETED_STEADY'][metric]
            if basic is not None and targeted is not None:
                differences[(delay, loss)][metric].append({'seed': seed, 'basic_minus_targeted': basic - targeted})
    result = []
    for (delay, loss), metrics in sorted(differences.items()):
        for metric, pairs in metrics.items():
            values = [pair['basic_minus_targeted'] for pair in pairs]
            rng = random.Random(20399)
            means = [statistics.mean(rng.choices(values, k=len(values))) for _ in range(10000)]
            result.append({'delay_ms': delay, 'loss_percent': loss, 'metric': metric,
                           'pairs': pairs, 'paired_mean_difference': statistics.mean(values),
                           'ci95': [quantile(means, .025), quantile(means, .975)],
                           'resampling_unit': 'paired block', 'bootstrap_seed': 20399,
                           'bootstrap_iterations': 10000,
                           'quality': 'COMPLETE' if len(values) == 5 else 'PARTIAL'})
    return result


def completion_audit(cells, wire_cells=None):
    expected = {(d, loss, seed) for d, loss in [(1, 0), (10, 0), (50, 0), (10, 1)]
                for seed in range(20301, 20306)}
    found, reasons = set(), []
    for cell in cells:
        if cell.get('measurement_family') in {'diagnostic', 'descriptor-qualification'}:
            reasons.append(cell['directory'] + ': non-performance evidence family')
            continue
        if not cell['latency_qualified']:
            reasons.append(cell['directory'] + ': invalid evidence')
            continue
        rows = [row for row in cell['runs'] if not row['smoke'] and row['purpose'] == 'performance']
        steady = [row for row in rows if row['arm'] in ['BASIC', 'TARGETED_STEADY']]
        if len(steady) != 2 or {row['arm'] for row in steady} != {'BASIC', 'TARGETED_STEADY'}:
            reasons.append(cell['directory'] + ': incomplete arms')
            continue
        key = (steady[0]['delay_ms'], steady[0]['loss_percent'], steady[0]['seed'])
        if key in found:
            reasons.append('duplicate matrix cell')
        found.add(key)
        manifest = json.loads((Path(cell['directory']) / 'manifest.json').read_text())
        if manifest['window_seconds'] != 60 or manifest['warmup_seconds'] != 10:
            reasons.append(cell['directory'] + ': non-contract window')
        if key[1] == 0:
            cold = [row for row in rows if row['arm'] in ['TARGETED_FIRST_CALL', 'TARGETED_BOOTSTRAP']]
            if len(cold) != 10 or any(row['attempts'] != 1 for row in cold):
                reasons.append(cell['directory'] + ': missing ten independent first calls')
        if cell['rtt_p50_ms'] is None:
            reasons.append(cell['directory'] + ': missing signed Data baseline')
    if found != expected:
        reasons.append('missing required cells: ' + str(sorted(expected - found)))
    if not any(cell['isolation'] == 'PASS' for cell in cells):
        reasons.append('target isolation not proven')
    if wire_cells is None:
        reasons.extend(failure for cell in cells for failure in cell['wire_failures'])
    else:
        wire_keys = set()
        if len({c['candidate_hash'] for c in wire_cells}) != 1:
            reasons.append('wire supplement candidate identity mismatch')
        primary_manifests = [json.loads((Path(c['directory']) / 'manifest.json').read_text()) for c in cells]
        def execution_identity(m):
            candidate = m['candidate']
            return {**{k: candidate[k] for k in ['binaries', 'libraries', 'headers', 'policy', 'trust', 'inherited_environment']},
                    'native_sources': {k: v for k, v in candidate['sources'].items() if not k.endswith('.py')},
                    **{k: m[k] for k in ['timeout_ms', 'business_delay_ms', 'payload_bytes', 'trust_profile', 'identities', 'history_isolation']}}
        identities = [execution_identity(m) for m in primary_manifests]
        if any(identity != identities[0] for identity in identities):
            reasons.append('primary execution identity mismatch')
        for cell in wire_cells:
            manifest = json.loads((Path(cell['directory']) / 'manifest.json').read_text())
            if execution_identity(manifest) != identities[0]:
                reasons.append(cell['directory'] + ': wire execution compatibility mismatch')
            key = (manifest['delay_ms'], manifest['loss_percent'], manifest['block_seed'])
            if (not cell['qualified'] or manifest.get('measurement_family') != 'wire-supplement'
                    or len(cell['runs']) != 2 or key in wire_keys
                    or manifest['warmup_seconds'] != 10 or manifest['window_seconds'] != 60
                    or any('wire' not in r for r in cell['runs'])):
                reasons.append(cell['directory'] + ': invalid independent wire supplement')
            wire_keys.add(key)
        if wire_keys != {(10, 0, seed) for seed in range(20301, 20306)}:
            reasons.append('missing five wire supplement pairs')
    return {'status': 'PASS' if not reasons else 'PARTIAL', 'reasons': reasons,
            'required_cells': len(expected), 'observed_cells': len(found)}


def first_call_statistics(cells):
    groups = defaultdict(list)
    for cell in cells:
        rows = [r for r in cell['runs'] if r['purpose'] == 'performance'
                and r['loss_percent'] == 0
                and r['arm'] in ['TARGETED_FIRST_CALL', 'TARGETED_BOOTSTRAP']]
        if not rows:
            continue
        successful = [r['p50_ms'] for r in rows if r['success'] and r['p50_ms'] is not None]
        groups[rows[0]['delay_ms']].append({
            'seed': rows[0]['seed'], 'attempts': sum(r['attempts'] for r in rows),
            'success': sum(r['success'] for r in rows),
            'timeout': sum(r['timeout'] for r in rows), 'error': sum(r['error'] for r in rows),
            'successful_p50_ms': quantile(successful, .5) if successful else None})
    result = []
    for delay, blocks in sorted(groups.items()):
        blocks.sort(key=lambda b: b['seed'])
        medians = [b['successful_p50_ms'] for b in blocks if b['successful_p50_ms'] is not None]
        attempts = sum(b['attempts'] for b in blocks)
        successes = sum(b['success'] for b in blocks)
        result.append({'delay_ms': delay, 'attempts': attempts, 'success': successes,
                       'timeout': sum(b['timeout'] for b in blocks),
                       'error': sum(b['error'] for b in blocks),
                       'success_ratio': successes / attempts if attempts else None,
                       'successful_block_median_mean_ms': statistics.mean(medians) if medians else None,
                       'contributing_blocks': len(medians), 'blocks': blocks,
                       'latency_scope': 'successful first calls only; all attempts retained in reliability'})
    return result


def figures(summary, output):
    """One manuscript figure; detailed statistics remain in the result table."""
    if summary['status'] != 'PASS':
        return
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
    plt.rcParams.update({'font.family': 'serif',
                         'font.serif': ['Liberation Serif', 'DejaVu Serif'],
                         'font.size': 8, 'axes.labelsize': 8,
                         'xtick.labelsize': 8, 'ytick.labelsize': 8,
                         'legend.fontsize': 7, 'axes.linewidth': .6,
                         'patch.linewidth': .6, 'hatch.linewidth': .5,
                         'pdf.fonttype': 42, 'xtick.direction': 'in',
                         'ytick.direction': 'in'})
    groups = defaultdict(lambda: defaultdict(list))
    for cell in summary['cells']:
        for row in cell['runs']:
            if (row['purpose'] == 'performance' and row['loss_percent'] == 0
                    and row['arm'] in ['BASIC', 'TARGETED_STEADY']
                    and row['p50_ms'] is not None):
                groups[row['delay_ms']][row['arm']].append(row['p50_ms'])
    delays = [1, 10, 50]
    fig, ax = plt.subplots(figsize=(3.4, 2.5), constrained_layout=True)
    series = [('BASIC', 'NDNSF basic', 'white', '////'),
              ('TARGETED_STEADY', 'NDNSF targeted', '.65', '')]
    for index, (arm, label, fill, hatch) in enumerate(series):
        values = [statistics.mean(groups[d][arm]) for d in delays]
        locations = np.arange(3) + (index - .5) * .32
        ax.bar(locations, values, width=.30, label=label,
               color=fill, hatch=hatch, edgecolor='black')
        for location, value in zip(locations, values):
            ax.annotate(f'{value:.0f}', (location, value), xytext=(0, 3),
                        textcoords='offset points', ha='center', fontsize=7)
    ax.set_xticks(range(3)); ax.set_xticklabels(delays)
    ax.set_xlabel('One-way delay per link (ms)')
    ax.set_ylabel('Mean block median latency (ms)')
    ax.set_ylim(0, max(statistics.mean(groups[d][arm])
                       for d in delays for arm, *_ in series) * 1.25)
    ax.legend(loc='upper left', frameon=False)
    fig.savefig(output / 'targeted-latency.pdf')
    fig.savefig(output / 'targeted-latency.png', dpi=200)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--wire-input', type=Path)
    args = parser.parse_args()
    directories = [args.input] if (args.input / 'manifest.json').exists() else sorted(
        p.parent for p in args.input.glob('*/manifest.json'))
    cells = [summarize_cell(path) for path in directories]
    args.output.mkdir(parents=True, exist_ok=True)
    wire_cells = [summarize_cell(p.parent) for p in sorted(args.wire_input.glob('*/manifest.json'))] if args.wire_input else None
    audit = completion_audit(cells, wire_cells)
    summary = {'status': audit['status'], 'completion_audit': audit,
               'cells': cells, 'paired': paired_statistics(cells),
               'wire_cells': wire_cells,
               'first_calls': first_call_statistics(cells),
               'analysis_identity': {'source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                                     'source': str(Path(__file__).resolve()),
                                     'input': str(args.input.resolve())}}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    rows = [r for cell in cells for r in cell['runs']]
    with (args.output / 'runs.csv').open('w', newline='') as file:
        fields = sorted(set(k for r in rows for k in r))
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        writer.writerows({k: json.dumps(v) if isinstance(v, (dict, list)) else v for k, v in r.items()} for r in rows)
    figures(summary, args.output)
    print(json.dumps({'cells': len(cells), 'qualified_cells': sum(c['qualified'] for c in cells),
                      'latency_qualified_cells': sum(c['latency_qualified'] for c in cells),
                      'wire_qualified_cells': sum(c['qualified'] for c in wire_cells) if wire_cells is not None else None,
                      'failures': [c['failures'] for c in cells if c['failures']],
                      'summary': str(args.output / 'summary.json')}))


if __name__ == '__main__':
    main()
