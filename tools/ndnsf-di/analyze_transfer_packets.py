#!/usr/bin/env python3
"""Associate observed NDN packet Names with native object timing; no admission decisions."""
import argparse
from collections import Counter
import csv
from decimal import Decimal
import json
from pathlib import Path

from ndn.encoding import Name


def variable_number(raw, offset):
    first = raw[offset]
    width = {253: 2, 254: 4, 255: 8}.get(first, 0)
    end = offset + 1 + width
    if end > len(raw):
        raise ValueError('truncated TLV number')
    return (int.from_bytes(raw[offset + 1:end], 'big') if width else first), end


def tlvs(raw):
    offset = 0
    while offset < len(raw):
        start = offset
        kind, offset = variable_number(raw, offset)
        length, offset = variable_number(raw, offset)
        end = offset + length
        if end > len(raw):
            raise ValueError('truncated TLV value')
        yield kind, raw[offset:end], raw[start:end]
        offset = end


def packet_name(raw, depth=0):
    if depth > 2:
        raise ValueError('nested LP packet')
    packets = list(tlvs(raw))
    if len(packets) != 1:
        raise ValueError('not one complete NDN packet')
    kind, value, _ = packets[0]
    if kind == 100:
        fragments = [data for field, data, _ in tlvs(value) if field == 80]
        if len(fragments) != 1:
            raise ValueError('LP packet has no unique Fragment')
        return packet_name(fragments[0], depth + 1)
    if kind not in (5, 6):
        raise ValueError('not Interest or Data')
    names = [wire for field, _, wire in tlvs(value) if field == 7]
    if len(names) != 1:
        raise ValueError('packet has no unique Name')
    return kind, tuple(bytes(c) for c in Name.from_bytes(names[0]))


def analyze(objects, capture):
    prefixes = [(row, tuple(bytes(c) for c in Name.from_str(row['planned_name'])))
                for row in objects if row['producer_role']]
    observed = {row['planned_name']: [] for row, _ in prefixes}
    counts = Counter()
    for packet in csv.DictReader((capture / 'link-packets.tsv').open(), delimiter='\t'):
        counts['frames'] += 1
        encoded = packet.get('data.data', '')
        if not encoded:
            counts['frames_without_decoded_payload'] += 1
            continue
        try:
            kind, name = packet_name(bytes.fromhex(encoded.replace(':', '')))
        except (ValueError, IndexError, TypeError):
            counts['unparsed_payloads'] += 1
            continue
        counts['parsed_interests' if kind == 5 else 'parsed_data'] += 1
        matches = [row for row, prefix in prefixes if name[:len(prefix)] == prefix]
        if len(matches) != 1:
            counts['other_or_ambiguous_names'] += 1
            continue
        counts['matched_packets'] += 1
        observed[matches[0]['planned_name']].append(dict(
            epoch_ms=float(Decimal(packet['frame.time_epoch']) * 1000), kind=kind,
            source=packet['ip.src'], destination=packet['ip.dst']))
    rows = []
    for obj, _ in prefixes:
        packets = observed[obj['planned_name']]
        data = [p['epoch_ms'] for p in packets if p['kind'] == 6]
        interests = [p['epoch_ms'] for p in packets if p['kind'] == 5]
        ready = float(obj['output_ready_epoch_ms']) if obj['output_ready_epoch_ms'] else None
        admitted = float(obj['complete_epoch_ms']) if obj['complete_epoch_ms'] else None
        first, last = (min(data), max(data)) if data else (None, None)
        rows.append(dict(turn=obj['turn'], planned_name=obj['planned_name'],
            producer_log=obj['producer_log'], observed_data_packets=len(data),
            observed_interests=len(interests), first_data_epoch_ms=first, last_data_epoch_ms=last,
            ready_to_first_data_ms=first-ready if first is not None and ready is not None else None,
            last_data_to_admitted_ms=admitted-last if last is not None and admitted is not None else None,
            first_to_last_data_ms=last-first if first is not None else None,
            interest_seen_before_ready=any(t <= ready for t in interests) if ready is not None else None))
    return dict(packet_counts=dict(counts), objects=rows,
        scope='One capture point: activation at Producer side, feedback at consumer side. '
              'Data timestamp is the frame completing any IP reassembly. '
              'Missing/LP-fragmented payloads remain missing. No cache-cost or link-capacity inference.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--objects', type=Path, required=True)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(list(csv.DictReader(args.objects.open())), args.capture)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
    print(json.dumps(result['packet_counts']))
