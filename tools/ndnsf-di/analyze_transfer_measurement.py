#!/usr/bin/env python3
"""Analyze native object observations and one-point link headers; never infer missing clocks."""
import argparse
from collections import defaultdict
import csv
from decimal import Decimal
import json
import math
from pathlib import Path
import re
import statistics


def summary(values):
    values = sorted(values)
    return dict(count=len(values), minimum=min(values) if values else None,
                median=statistics.median(values) if values else None,
                p95=values[math.ceil(len(values) * .95) - 1] if values else None,
                maximum=max(values) if values else None)


def union_seconds(intervals):
    total, end = 0, None
    for first, last in sorted(intervals):
        if last < first:
            raise ValueError('negative interval')
        total += max(0, last - max(first, end if end is not None else first))
        end = max(last, end if end is not None else last)
    return total / 1e9


def integer(row, field):
    text = str(row.get(field, ''))
    return int(text) if re.fullmatch(r'[0-9]+', text) else None


def analyze(evidence, capture):
    start = json.loads((capture / 'capture-start.json').read_text())
    offset = start['ready_epoch_ns'] - start['ready_steady_ns']
    requests = [json.loads(line) for line in (evidence / 'requester-statistics.jsonl').read_text().splitlines()]
    requests = [row for row in requests if row.get('type') == 'request']
    requests.sort(key=lambda row: row['turn'])
    names = {row['requestId']: row['turn'] for row in requests}
    raw = []
    for path in sorted(evidence.glob('provider-*.log')):
        if '-identity' in path.name:
            continue
        for line in path.read_text(errors='strict').splitlines():
            marker = next((text for text in ('NDNSF_DI_DEPENDENCY_OUTPUT_TIMING',
                'NDNSF_DI_DEPENDENCY_INPUT_TIMING', 'NDNSF_DI_DEPENDENCY_OBJECT',
                'NDNSF_DI_PROVIDER_STAGE') if text in line), None)
            if marker is None:
                continue
            row = dict(re.findall(r'(\w+)=([^ \n]+)', line))
            if marker == 'NDNSF_DI_PROVIDER_STAGE' and row.get('stage') != 'DEPENDENCY_FETCH':
                continue
            prefix = re.match(r'([0-9]+\.[0-9]+)', line)
            row.update(marker=marker, file=path.name,
                       log_epoch_ns=int(Decimal(prefix[1]) * 10**9) if prefix else None)
            raw.append(row)
    outputs, inputs, completed, publications = {}, {}, {}, {}
    conflicts = []

    def identity(row):
        session = row.get('session', row.get('sessionId', row.get('requestId', '')))
        request = next((request for request in names if session == request or
                        session.startswith(request + '/attempt/')), None)
        stage = row.get('stage_identity', '').split('|')
        attempt = row.get('attemptEpoch') or (stage[1] if len(stage) > 1 else None)
        if attempt is None and '/attempt/' in session:
            attempt = session.rsplit('/attempt/', 1)[1]
        if attempt is None:
            match = re.search(r'/(?:8=)?ATTEMPT/(?:8=)?([0-9]+)(?:/|$)',
                              row.get('planned_name', row.get('plannedName', '')))
            attempt = match[1] if match else None
        return request, str(attempt) if attempt is not None else None

    for row in raw:
        name = row.get('planned_name', row.get('plannedName'))
        marker = row['marker']
        target = (outputs if marker.endswith('OUTPUT_TIMING') else
                  inputs if marker.endswith('INPUT_TIMING') else
                  publications if marker.endswith('DEPENDENCY_OBJECT') and row.get('direction', '').startswith('publish') else
                  completed if row.get('status') == 'complete' else None)
        if target is None or not name or name in ('none', 'false'):
            continue
        request, attempt = identity(row)
        key = (row['file'], request, attempt, name)
        if key in target and target[key] != row:
            conflicts.append(dict(file=row['file'], planned_name=name, marker=marker))
        target[key] = row
    objects = []
    for (consumer, request, attempt, name), received in inputs.items():
        candidates = [row for (producer, output_request, output_attempt, output_name), row in outputs.items()
                      if output_name == name and producer != consumer and request is not None and
                      output_request == request and output_attempt == attempt and attempt is not None]
        output = candidates[0] if len(candidates) == 1 else None
        published = [row for (producer, output_request, output_attempt, output_name), row in publications.items()
                     if output_name == name and producer != consumer and request is not None and
                     output_request == request and output_attempt == attempt and attempt is not None and
                     row.get('producer') == received.get('producer') and row.get('consumer') == received.get('role')]
        publication = published[0] if len(published) == 1 else None
        complete = completed.get((consumer, request, attempt, name)) if request is not None and attempt is not None else None
        ready_ms = integer(output or {}, 'output_ready_epoch_ms')
        published_ms = integer(output or {}, 'publish_done_epoch_ms')
        steady_us = integer(complete or {}, 'steady_us')
        complete_epoch_ms = (steady_us * 1000 + offset) / 1e6 if steady_us is not None else None
        publication_epoch_ms = publication['log_epoch_ns'] / 1e6 if publication and publication['log_epoch_ns'] else None
        row = dict(request_id=request, attempt_epoch=attempt, turn=names.get(request), planned_name=name,
            producer_log=output['file'] if output else publication['file'] if publication else None, consumer_log=consumer,
            edge=received.get('stage_edge', received.get('scope')), phase=received.get('stage_phase'),
            producer_role=received.get('producer'), consumer_role=received.get('role'),
            tensor_bytes=integer(received, 'tensor_bytes'),
            encoded_payload_bytes=integer(received, 'encoded_payload_bytes'),
            packed_publication_payload_bytes=integer(publication or {}, 'encoded_payload_bytes'),
            transport_payload_bytes=integer(received, 'transport_payload_bytes'),
            signed_data_wire_bytes=integer(received, 'wire_bytes'),
            interest_count=integer(received, 'interest_count'), retry_count=integer(received, 'retry_count'),
            fetch_ms=float(received['fetch_ms']) if 'fetch_ms' in received else None,
            output_ready_epoch_ms=ready_ms, publication_done_epoch_ms=published_ms,
            complete_epoch_ms=complete_epoch_ms,
            ready_to_admitted_ms=complete_epoch_ms-ready_ms if ready_ms is not None and complete_epoch_ms is not None else None,
            published_to_admitted_ms=complete_epoch_ms-published_ms if published_ms is not None and complete_epoch_ms is not None else None,
            publication_record_to_admitted_ms=complete_epoch_ms-publication_epoch_ms if publication_epoch_ms is not None and complete_epoch_ms is not None else None,
            matched_producer_count=len(candidates) if candidates else len(published))
        row['ready_to_admission_packed_bytes_per_second'] = (
            row['packed_publication_payload_bytes'] * 1000 / row['ready_to_admitted_ms']
            if row['packed_publication_payload_bytes'] is not None and row['ready_to_admitted_ms'] is not None and row['ready_to_admitted_ms'] > 0 else None)
        objects.append(row)

    windows = [(row['turn'], row['startedSteadyNs'], row['finishedSteadyNs']) for row in requests]
    packet_groups, packet_count = defaultdict(list), 0
    with (capture / 'link-packets.tsv').open() as stream:
        for row in csv.DictReader(stream, delimiter='\t'):
            if not row.get('frame.time_epoch'):
                continue
            stamp = int(Decimal(row['frame.time_epoch']) * 10**9) - offset
            phase = next((str(turn) for turn, first, last in windows if first <= stamp <= last), None)
            if phase is None:
                phase = ('pre-first-request' if windows and stamp < windows[0][1] else
                         'after-last-request' if windows and stamp > windows[-1][2] else
                         'between-requests')
            udp_length = integer(row, 'udp.length')
            packet_groups[phase].append(dict(steady_ns=stamp,
                frame_bytes=int(row['frame.len']), captured_bytes=int(row['frame.cap_len']),
                ip_bytes=integer(row, 'ip.len'), udp_bytes=udp_length,
                udp_payload_bytes=udp_length-8 if udp_length is not None else None,
                source=row['ip.src'], destination=row['ip.dst']))
            packet_count += 1

    providers = [json.loads(line) for path in evidence.glob('provider-*-statistics.jsonl')
                 for line in path.read_text().splitlines()]
    turns = []
    for request in requests:
        turn, wall = request['turn'], (request['finishedSteadyNs'] - request['startedSteadyNs']) / 1e9
        local = [row for row in objects if row['turn'] == turn and not row['producer_role']]
        selected = [row for row in objects if row['turn'] == turn and row['producer_role']]
        packets = packet_groups[str(turn)]
        totals = {field: sum(row[field] for row in selected) if selected and all(row[field] is not None for row in selected) else None
                  for field in ('tensor_bytes', 'encoded_payload_bytes', 'packed_publication_payload_bytes', 'transport_payload_bytes',
                                'signed_data_wire_bytes', 'interest_count', 'retry_count')}
        directions = defaultdict(lambda: dict(packets=0, frame_bytes=0, known_udp_header_payload_bytes=0,
                                             missing_udp_headers=0))
        for row in packets:
            flow = directions[row['source'] + ' -> ' + row['destination']]
            flow['packets'] += 1
            flow['frame_bytes'] += row['frame_bytes']
            if row['udp_payload_bytes'] is not None:
                flow['known_udp_header_payload_bytes'] += row['udp_payload_bytes']
            else:
                flow['missing_udp_headers'] += 1
        backend = {}
        for provider in providers:
            if provider.get('requestId') != request['requestId']:
                continue
            for metric in provider.get('runtimeMetrics', []):
                timing = metric.get('lastBackendExecution')
                if timing:
                    first, last = timing['startedSteadyNs'], timing['finishedSteadyNs']
                    backend[(provider['provider'], provider['role'], metric['inferenceEpoch'], first, last)] = (
                        max(first, request['startedSteadyNs']), min(last, request['finishedSteadyNs']))
        turns.append(dict(turn=turn, request_id=request['requestId'], wall_seconds=wall,
            requester_cpu_seconds=(request['finishedProcessCpuNs']-request['startedProcessCpuNs'])/1e9,
            backend_union_seconds=union_seconds(backend.values()) if backend else None, backend_calls=len(backend),
            admitted_objects=len(selected), local_application_inputs=len(local),
            missing_producer_observations=sum(row['matched_producer_count'] != 1 for row in selected),
            missing_complete_clocks=sum(row['complete_epoch_ms'] is None for row in selected),
            semantic_bytes=totals, link_packets=len(packets),
            link_frame_bytes=sum(row['frame_bytes'] for row in packets),
            link_udp_payload_bytes=sum(row['udp_payload_bytes'] for row in packets) if all(row['udp_payload_bytes'] is not None for row in packets) else None,
            link_missing_udp_headers=sum(row['udp_payload_bytes'] is None for row in packets),
            link_frame_mbit_per_second=sum(row['frame_bytes'] for row in packets)*8/wall/1e6,
            semantic_encoded_bytes_per_request_second=totals['encoded_payload_bytes']/wall if totals['encoded_payload_bytes'] is not None else None,
            ready_to_admitted_ms=summary([row['ready_to_admitted_ms'] for row in selected if row['ready_to_admitted_ms'] is not None]),
            ready_to_admission_packed_bytes_per_second=summary([row['ready_to_admission_packed_bytes_per_second'] for row in selected if row['ready_to_admission_packed_bytes_per_second'] is not None]),
            publication_record_to_admitted_ms=summary([row['publication_record_to_admitted_ms'] for row in selected if row['publication_record_to_admitted_ms'] is not None]),
            published_to_admitted_ms=summary([row['published_to_admitted_ms'] for row in selected if row['published_to_admitted_ms'] is not None]),
            fetch_ms=summary([row['fetch_ms'] for row in selected if row['fetch_ms'] is not None]),
            directions=dict(directions)))
    all_packets = [row for group in packet_groups.values() for row in group]
    outside = [row for phase, group in packet_groups.items() if not phase.isdigit() for row in group]
    return dict(schema='ndnsf-di-transfer-analysis-v1', objects=objects, raw_native_observations=raw,
        duplicate_or_conflicting_observations=conflicts, turns=turns,
        link_total=dict(packets=packet_count, frame_bytes=sum(row['frame_bytes'] for row in all_packets),
                        udp_payload_bytes=sum(row['udp_payload_bytes'] for row in all_packets) if all(row['udp_payload_bytes'] is not None for row in all_packets) else None,
                        missing_udp_headers=sum(row['udp_payload_bytes'] is None for row in all_packets)),
        link_outside_request_loops=dict(packets=len(outside), frame_bytes=sum(row['frame_bytes'] for row in outside)),
        link_phases={phase:dict(packets=len(group),frame_bytes=sum(row['frame_bytes'] for row in group))
                     for phase,group in packet_groups.items()},
        limitations=['Capture is one boundary, not intra-node UNIX traffic; UDP includes control/retries/LP fragmentation.',
            'Header capture drop count must be reviewed from capture.log; no zero-drop claim is inferred from silence.',
            'Output-ready/publication timestamps have millisecond precision; local monotonic clock maps to capture wall time.',
            'Production-to-admission includes publication, scheduling, waiting, transfer and validation.',
            'Per-request rates include computation; this is not sustained link capacity.',
            'Native signed Data bytes are receive observations, not total IP/Ethernet traffic.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', required=True, type=Path)
    parser.add_argument('--capture', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.evidence, args.capture)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
    print(json.dumps({key: result[key] for key in ('link_total', 'turns')}))


if __name__ == '__main__':
    main()
