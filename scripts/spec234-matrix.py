#!/usr/bin/env python3
"""Run one frozen Spec 234 matrix cell through the maintained MiniNDN owner.

Each cell is one deployment (frozen layout and network) executed as N independent
boots (blocks). The first boot prepares the source package; later boots reuse the
preparation state and keep the Provider assembly cache warm, exactly as E1/E2
require. Every boot produces one JSONL result row with the per-turn timings,
probe summary and failure boundary.
"""

import argparse
import json
import shutil
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FIXTURE = REPO / 'tests/fixtures/spec242/prepare-deployment.py'
OPTS = '-m ndnsf_distributed_inference.ops.cli'
MODEL = Path('/home/ndn/models/spec233/Qwen3-0.6B-ONNX/onnx/model_q4.onnx')
TOKENIZER = Path('/home/ndn/models/spec233/Qwen3-0.6B-ONNX/tokenizer.json')
SOURCE_DIGEST = 'sha256:d43d836fc5e240df9013733ccd214972c5d21bd9ec47e574e4f1e359cf90aed0'
PREFIX = Path('/usr/local')
MANIFEST = Path('/usr/local/share/ndnsf/stack-install-manifest.json')

# The prepared workload freezes each layout's geometry (stages, cuts, degrees and
# rank weights); these labels only name the cell family in the results file.
LAYOUTS = {name: {} for name in ('L1', 'L2', 'L3', 'L4a', 'L4b', 'L5', 'L6', 'L7a', 'L7b')}
NETWORKS = {
    'N0': dict(bw=100, delay=2, loss=None),
    'N1': dict(bw=10, delay=2, loss=None),
    'N2': dict(bw=1000, delay=2, loss=None),
    'N3': dict(bw=100, delay=10, loss=None),
    'N4': dict(bw=100, delay=30, loss=None),
    'N5': dict(bw=100, delay=2, loss=0.1),
    'N6': dict(bw=100, delay=2, loss=1),
    'N7': dict(bw=10, delay=10, loss=0.1),
}


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, **kwargs)


def generate(root, workload, layout, network, journal, storage_budget,
             provider_cpus=None, dynamic_shaping=False):
    spec = LAYOUTS[layout]
    link = NETWORKS[network]
    # The prepared workload already froze the layer cut and rank weights in its
    # catalog/splitter; only the network and the workload path change per cell.
    del spec
    argv = [sys.executable, str(FIXTURE), '--output', str(root), '--prefix', str(PREFIX),
            '--release-manifest', str(MANIFEST), '--journal-root', str(journal),
            '--prepared-workload', str(workload),
            '--link-bw', str(link['bw']), '--link-delay', str(link['delay']),
            '--no-kv-capture', '--storage-budget', str(storage_budget)]
    if provider_cpus:
        argv += ['--provider-cpus', *[str(value) for value in provider_cpus]]
    if dynamic_shaping:
        argv += ['--dynamic-shaping']
    if link['loss'] is not None:
        argv += ['--link-loss', str(link['loss'])]
    run(argv)


def boot(root, timeout):
    log = root / 'boot.log'
    env = dict(os.environ, PYTHONPATH=str(REPO / 'NDNSF-DistributedInference'),
               NDNSF_DI_DIAGNOSTIC_LOG_FILTER='ndn.*=NONE:ndn_service_framework.*=WARN:ndnsf.*=DEBUG')
    started = time.monotonic()
    with open(log, 'wb') as output:
        result = subprocess.run(
            [sys.executable, *OPTS.split(), 'cluster', 'up', '--config',
             str(root / 'deployment.json'), '--timing', '--debug', '--timeout', str(timeout)],
            env=env, stdout=output, stderr=subprocess.STDOUT)
    return result.returncode, time.monotonic() - started, log


def close(root, timeout=120):
    env = dict(os.environ, PYTHONPATH=str(REPO / 'NDNSF-DistributedInference'))
    subprocess.run([sys.executable, *OPTS.split(), 'cluster', 'down', '--config',
                    str(root / 'deployment.json')], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)


def summarize(root, code, wall, layout, network, block, result_path,
              cell=None, provider_cpus=None, reference=None, outputs_dir=None):
    row = dict(layout=layout, network=network, block=block, exit_code=code,
               wall_seconds=round(wall, 2))
    if cell is not None:
        row['cell'] = cell
    if provider_cpus:
        row['provider_cpus'] = list(provider_cpus)
    try:
        deployment = json.loads((root / 'deployment.json').read_text())
        state = json.loads((Path(deployment['storage']['state_root']) /
                            deployment['deployment_id'] / 'state.json').read_text())
        row['boot_id'] = state.get('boot_id')
        row['status'] = state.get('status')
        row['failure'] = state.get('failure')
        row['requester_wall_seconds'] = state.get('requester', {}).get('wall_seconds')
        probes = state.get('probes') or {}
        row['probe_pings'] = probes.get('ping')
        row['probe_status'] = probes.get('status')
        evidence = Path(state.get('evidence_root', ''))
        stats = evidence / 'requester-statistics.jsonl'
        turns = []
        if stats.exists():
            for line in stats.read_text().splitlines():
                if not line.strip():
                    continue
                entry = json.loads(line)
                if entry.get('type') != 'request':
                    continue
                submit = entry.get('requestSubmittedSteadyNs')
                first = entry.get('firstEventSteadyNs')
                last = entry.get('lastEventSteadyNs')
                turns.append(dict(
                    turn=entry.get('turn'), observed=entry.get('observedEvents'),
                    completed=entry.get('completed'),
                    ttft_seconds=round((first - submit) / 1e9, 3) if submit and first else None,
                    span_seconds=round((last - first) / 1e9, 3) if first and last else None,
                    cpu_seconds=round((entry.get('finishedProcessCpuNs') or 0) / 1e9 -
                                      (entry.get('startedProcessCpuNs') or 0) / 1e9, 3)
                    if entry.get('startedProcessCpuNs') is not None else None))
        row['turns'] = turns
        if reference is not None and code == 0:
            matched, observed = _compare_reference(root, reference)
            row['referenceMatch'] = matched
            row['referenceObserved'] = observed
        _archive_outputs(root, outputs_dir, block)
    except Exception as error:  # a failed cell still produces a row
        row['result_error'] = str(error)
    with open(result_path, 'a') as output:
        output.write(json.dumps(row, sort_keys=True) + '\n')
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--layout', required=True, choices=sorted(LAYOUTS))
    parser.add_argument('--network', required=True, choices=sorted(NETWORKS))
    parser.add_argument('--blocks', type=int, default=1)
    parser.add_argument('--root', required=True, type=Path,
                        help='Cell directory; one deployment lives here')
    parser.add_argument('--workload', required=True, type=Path,
                        help='Prepared workload directory (32-token profile)')
    parser.add_argument('--journal', required=True, type=Path)
    parser.add_argument('--results', required=True, type=Path)
    parser.add_argument('--storage-budget', type=int, default=6 << 30)
    parser.add_argument('--keep-root', action='store_true',
                        help='Keep the cell root after a successful cell')
    parser.add_argument('--provider-cpus', nargs='+', type=int,
                        help='Per-provider CPU quota (placement-interaction cells)')
    parser.add_argument('--dynamic-shaping', action='store_true',
                        help='Shape Provider data links after material assembly')
    parser.add_argument('--reference', type=Path,
                        help='Frozen reference token vectors (JSON) for per-round comparison')
    parser.add_argument('--outputs-dir', type=Path,
                        help='Archive each block token outputs under this directory')
    parser.add_argument('--timeout', type=int, default=2400)
    args = parser.parse_args()
    args.root.parent.mkdir(parents=True, exist_ok=True)
    args.journal.mkdir(parents=True, exist_ok=True)
    if not (args.root / 'deployment.json').exists():
        generate(args.root, args.workload, args.layout, args.network, args.journal,
                 args.storage_budget, args.provider_cpus, args.dynamic_shaping)
    for block in range(args.blocks):
        close(args.root)
        code, wall, log = boot(args.root, args.timeout)
        reference = None
        if args.reference is not None:
            reference = json.loads(args.reference.read_text())
        row = summarize(args.root, code, wall, args.layout, args.network, block, args.results,
                        cell=args.root.name, provider_cpus=args.provider_cpus,
                        reference=reference, outputs_dir=args.outputs_dir)
        print(json.dumps(row, sort_keys=True))
        if code != 0:
            close(args.root)
            return 1
    close(args.root)
    if not args.keep_root:
        shutil.rmtree(args.root, ignore_errors=True)
        shutil.rmtree(args.journal, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
