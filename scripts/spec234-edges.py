#!/usr/bin/env python3
"""Reduce one frozen Spec 234 boot into per-stage and per-edge evidence.

Reads the provider/requester logs and the before/after link probes and prints a
compact JSON summary: dependency classes (TP partial, PP feedback, activation,
application input), payload and signed-object byte distributions, publish and
fetch timings, retry/failure counts, and the per-node link byte deltas.
"""

import argparse
import json
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path


def class_of(scope):
    if 'hybrid-' in scope:
        return 'TP_PARTIAL'
    if 'token-feedback' in scope:
        return 'PP_FEEDBACK'
    if scope == 'application-input':
        return 'APPLICATION_INPUT'
    if scope == 'final-response':
        return 'FINAL_RESPONSE'
    return 'ACTIVATION'


def median(values):
    return round(statistics.median(values), 3) if values else None


def parse_edges(evidence):
    edges = defaultdict(lambda: {'payload': [], 'encoded': [], 'publish': [],
                                 'fetch': [], 'decode': [], 'crypto': [],
                                 'directions': Counter(), 'statuses': Counter()})
    obj = re.compile(r'NDNSF_DI_DEPENDENCY_OBJECT .*?scope=(\S+) .*?direction=(\S+) '
                     r'payload_bytes=(\d+) encoded_payload_bytes=(\d+).*?status=(\S+)')
    out = re.compile(r'NDNSF_DI_DEPENDENCY_OUTPUT_TIMING .*?scope=(\S+) publish_ms=([\d.]+) bytes=(\d+)')
    inp = re.compile(r'NDNSF_DI_DEPENDENCY_INPUT_TIMING .*?scope=(\S+) .*?fetch_ms=([\d.]+) decode_ms=([\d.]+)')
    env = re.compile(r'NDNSF_DI_TENSOR_ENVELOPE (\{.*\})')
    envelope_crypto = {}
    for log in sorted(evidence.glob('provider-*.log')):
        for line in log.read_text(errors='replace').splitlines():
            found = obj.search(line)
            if found:
                key = class_of(found.group(1))
                edges[key]['payload'].append(int(found.group(3)))
                edges[key]['encoded'].append(int(found.group(4)))
                edges[key]['directions'][found.group(2)] += 1
                edges[key]['statuses'][found.group(5)] += 1
                continue
            found = out.search(line)
            if found:
                key = class_of(found.group(1))
                edges[key]['publish'].append(float(found.group(2)))
                continue
            found = inp.search(line)
            if found:
                key = class_of(found.group(1))
                edges[key]['fetch'].append(float(found.group(2)))
                edges[key]['decode'].append(float(found.group(3)))
                continue
            found = env.search(line)
            if found:
                try:
                    payload = json.loads(found.group(1))
                except json.JSONDecodeError:
                    continue
                bundle = payload.get('bundleBytes')
                crypto = payload.get('cryptoSegmentBytes')
                if isinstance(bundle, int) and isinstance(crypto, int):
                    envelope_crypto[bundle] = crypto
    summary = {}
    for key, value in edges.items():
        payload_median = median(value['payload'])
        crypto = [envelope_crypto[value['payload'][index]]
                  for index in range(len(value['payload']))
                  if value['payload'][index] in envelope_crypto]
        summary[key] = {
            'events': len(value['payload']),
            'payload_bytes_median': payload_median,
            'encoded_bytes_median': median(value['encoded']),
            'crypto_segment_bytes_median': median(crypto),
            'publish_ms_median': median(value['publish']),
            'fetch_ms_median': median(value['fetch']),
            'decode_ms_median': median(value['decode']),
            'directions': dict(value['directions']),
            'statuses': dict(value['statuses']),
        }
    return summary


def link_deltas(evidence):
    before = evidence / 'probes.json'
    after = evidence / 'probes-after.json'
    if not before.exists() or not after.exists():
        return None
    def sample(path):
        data = json.loads(path.read_text())
        result = {}
        for node, entry in (data.get('nodes', data)).items():
            text = entry.get('links', {}).get('output', '')
            sent = 0
            mode = None
            for line in text.splitlines():
                stripped = line.strip()
                if stripped.startswith('RX:'):
                    mode = 'rx'
                    continue
                if stripped.startswith('TX:'):
                    mode = 'tx'
                    continue
                if mode is None:
                    continue
                match = re.match(r'(\d+)', stripped)
                if match:
                    if mode == 'tx':
                        sent += int(match.group(1))
                    mode = None
            result[node] = sent
        return result
    b = sample(before)
    a = sample(after)
    return {node: {'tx_bytes_delta': a[node] - b.get(node, 0)} for node in a}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = {'edges': parse_edges(args.evidence), 'links': link_deltas(args.evidence)}
    text = json.dumps(result, indent=2, sort_keys=True)
    print(text)
    if args.output:
        args.output.write_text(text + '\n')


if __name__ == '__main__':
    main()
