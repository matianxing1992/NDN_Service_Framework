#!/usr/bin/env python3
"""One monochrome four-arm figure from the qualified matched campaign."""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--metric', choices=['p50_ms', 'mean_ms'], default='p50_ms')
    args = parser.parse_args()
    summary = json.loads(args.summary.read_text())
    if summary['status'] != 'PASS':
        raise ValueError('full matched qualification required')
    rows = {(r['delay_ms'], r['loss'], r['arm']): r for r in summary['aggregates']}
    conditions = [(1, 0), (10, 0), (50, 0), (10, 1)]
    series = [('BASIC', 'NDNSF basic', 'white', '//'),
              ('TARGETED_STEADY', 'NDNSF targeted', '0.7', ''),
              ('GRPC', 'gRPC', 'white', 'xx'), ('NSC', 'NSC', '0.9', '..')]
    plt.rcParams.update({'font.family': 'serif', 'font.size': 8,
                         'axes.linewidth': .6, 'patch.linewidth': .6,
                         'hatch.linewidth': .5, 'pdf.fonttype': 42})
    fig, ax = plt.subplots(figsize=(3.5, 2.25))
    width = .19
    for i, (arm, label, fill, hatch) in enumerate(series):
        values = [rows[(d, loss, arm)][args.metric] for d, loss in conditions]
        if any(v is None for v in values):
            raise ValueError('missing successful latency; cannot draw a zero bar')
        ax.bar([j+(i-1.5)*width for j in range(4)], values, width,
               label=label, color=fill, hatch=hatch, edgecolor='black')
    ax.set_xticks(range(4))
    ax.set_xticklabels(['1 ms\n0%', '10 ms\n0%', '50 ms\n0%', '10 ms\n1%'])
    ax.set_ylabel('Successful ' + ('mean' if args.metric == 'mean_ms' else 'median') + ' latency (ms)')
    ax.set_xlabel('Per-link one-way delay / packet loss')
    ax.set_axisbelow(True)
    ax.yaxis.grid(True, color='0.85', linewidth=.4)
    ax.legend(ncol=2, fontsize=7, loc='upper left', frameon=False)
    ax.margins(y=.23)
    fig.tight_layout(pad=.45)
    args.output.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output/'targeted-four-way.pdf')
    fig.savefig(args.output/'targeted-four-way.png', dpi=200)
    plt.close(fig)


if __name__ == '__main__':
    main()
