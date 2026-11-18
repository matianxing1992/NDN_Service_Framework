#!/usr/bin/env python3
"""Reduce Spec 234 matrix rows into per-cell statistics and paired ratios.

Input is the JSONL produced by scripts/spec234-matrix.py (one row per boot).
Each row's first 16-token turn is the prefill/decode sample; continuation turns
(2 tokens) are reported separately. Cells compare the weighted layout (L3)
against the equal layout (L2) on the same network with a paired bootstrap over
blocks (fixed seed, 10,000 resamples of whole blocks).
"""

import argparse
import json
import random
import statistics
from collections import defaultdict
from pathlib import Path


def load(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        if row.get('exit_code') != 0 or row.get('failure'):
            row['valid'] = False
        else:
            turns = [t for t in row.get('turns', []) if t.get('completed')]
            row['valid'] = bool(turns)
            row['main'] = turns[0] if turns else None
            row['continuation'] = turns[1] if len(turns) > 1 else None
        rows.append(row)
    return rows


def cell_stats(rows):
    valid = [r for r in rows if r['valid']]
    stats = dict(attempts=len(rows), valid=len(valid))
    if valid:
        ttft = [r['main']['ttft_seconds'] for r in valid]
        span = [r['main']['span_seconds'] for r in valid]
        stats.update(ttft_median=round(statistics.median(ttft), 3),
                     span_median=round(statistics.median(span), 3),
                     ttft_min=round(min(ttft), 3), ttft_max=round(max(ttft), 3))
        continuations = [r['continuation']['span_seconds'] for r in valid
                         if r.get('continuation')]
        if continuations:
            stats['continuation_span_median'] = round(statistics.median(continuations), 3)
    return stats, valid


def paired_ratio(test_rows, control_rows, seed=234):
    """Treatment/control ratio of warm prefill spans over matched block pairs."""
    test = {(r['network'], r['block']): r['main']['span_seconds']
            for r in test_rows if r['valid']}
    control = {(r['network'], r['block']): r['main']['span_seconds']
               for r in control_rows if r['valid']}
    keys = sorted(set(test) & set(control))
    ratios = [test[k] / control[k] for k in keys]
    if not ratios:
        return None
    rng = random.Random(seed)
    boot = []
    for _ in range(10000):
        sample = [ratios[rng.randrange(len(ratios))] for _ in ratios]
        boot.append(statistics.median(sample))
    boot.sort()
    return dict(blocks=len(ratios), median=round(statistics.median(ratios), 4),
                ci95=[round(boot[249], 4), round(boot[9749], 4)], points=ratios)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    rows = load(args.results)
    cells = defaultdict(list)
    for row in rows:
        cells[(row['layout'], row['network'])].append(row)
    summary = {}
    for (layout, network), cell_rows in sorted(cells.items()):
        title = f'{layout}/{network}'
        summary[title] = dict(cell_stats(cell_rows)[0],
                              failures=sorted({r.get('failure') for r in cell_rows
                                               if r.get('failure')}))
    for network in sorted({row['network'] for row in rows}):
        test = [r for r in rows if r['layout'] == 'L3' and r['network'] == network]
        control = [r for r in rows if r['layout'] == 'L2' and r['network'] == network]
        ratio = paired_ratio(test, control)
        if ratio:
            summary.setdefault(f'paired L3/L2 {network}', {}).update(ratio)
    text = json.dumps(summary, indent=2, sort_keys=True)
    print(text)
    if args.output:
        args.output.write_text(text + '\n')


if __name__ == '__main__':
    main()
