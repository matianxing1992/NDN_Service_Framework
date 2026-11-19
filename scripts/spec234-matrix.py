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
import signal
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


def _prune_boot_dirs(root, archive_dir=None):
    """Archive the small receipt files, then drop the boot's disposable dirs.

    The layout root keeps the Provider identities, Repo source and the
    assembled slice cache; the per-boot evidence and operation state are
    archived (small files) and removed so later boots stay inside the storage
    budget (E6.2/E6.4).
    """
    try:
        deployment = json.loads((root / 'deployment.json').read_text())
        storage = deployment['storage']
        state_file = Path(storage['state_root']) / deployment['deployment_id'] / 'state.json'
        if not state_file.exists():
            return
        state = json.loads(state_file.read_text())
        boot = state.get('boot_id')
        evidence = Path(storage['evidence_root']) / deployment['deployment_id'] / (boot or '')
        if archive_dir is not None and evidence.exists() and boot:
            # Keep only the small receipts: the per-turn requester statistics,
            # the probe snapshots and the shaping applications. Provider and
            # requester diagnostic logs stay disposable.
            target = archive_dir / boot
            target.mkdir(parents=True, exist_ok=True)
            for name in ('requester-statistics.jsonl', 'probes.json',
                         'probes-after.json', 'cells.jsonl'):
                path = evidence / name
                if path.is_file():
                    shutil.copy2(path, target / name)
            for path in evidence.glob('shaping-*.json'):
                if path.is_file():
                    shutil.copy2(path, target / path.name)
        shutil.rmtree(evidence, ignore_errors=True)
        shutil.rmtree(Path(storage['state_root']) / deployment['deployment_id'],
                        ignore_errors=True)
    except Exception:
        return


def _start_hang(root):
    """True when the last boot stopped inside startup instead of the request."""
    try:
        deployment = json.loads((root / 'deployment.json').read_text())
        state = json.loads((Path(deployment['storage']['state_root']) /
                            deployment['deployment_id'] / 'state.json').read_text())
        failure = state.get('failure') or ''
        return state.get('status') == 'STOPPED' and (
            'STARTING' in failure or 'MININDN_HOST_ALREADY_ACTIVE' in failure)
    except Exception:
        return False


def _cleanup_hosts():
    """Drop leftover MiniNDN host shells and NFDs before a retry."""
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            cmdline = (entry / 'cmdline').read_bytes().replace(b'\0', b' ').decode(
                'utf-8', 'replace')
            comm = (entry / 'comm').read_text().strip()
        except OSError:
            continue
        if cmdline.startswith('bash --norc --noediting -is mininet:') or comm == 'nfd':
            try:
                os.kill(int(entry.name), signal.SIGKILL)
            except OSError:
                pass
    for socket in Path('/run/nfd').glob('*.sock'):
        try:
            socket.unlink()
        except OSError:
            pass


def _compare_reference(root, reference):
    """Compare every archived round token vector with the frozen reference."""
    matched = True
    observed = []
    for index, expected in enumerate(reference['rounds']):
        path = root / f'output-{index}.json'
        if not path.exists():
            return None, [None] * len(reference['rounds'])
        ids = json.loads(path.read_text()).get('tokenIds', [])
        observed.append(len(ids))
        if ids != expected['tokenIds']:
            matched = False
    return matched, observed


def _archive_outputs(root, outputs_dir, block):
    if outputs_dir is None:
        return
    directory = outputs_dir / f'block-{block}'
    directory.mkdir(parents=True, exist_ok=True)
    for path in sorted(root.glob('output-*.json')):
        shutil.copy2(path, directory / path.name)


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
              cell=None, provider_cpus=None, reference=None, outputs_dir=None,
              attempt=None):
    row = dict(layout=layout, network=network, block=block, exit_code=code,
               wall_seconds=round(wall, 2))
    if cell is not None:
        row['cell'] = cell
    if attempt is not None:
        row['attempt'] = attempt
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
    parser.add_argument('--retry-failures', type=int, default=0,
                        help='Retry a boot that stopped inside startup (attempts kept as rows)')
    parser.add_argument('--prune-boot-dirs', action='store_true',
                        help='Archive small receipts and drop per-boot evidence/state dirs')
    parser.add_argument('--archive-dir', type=Path,
                        help='Receipt archive for --prune-boot-dirs')
    parser.add_argument('--cell-label',
                        help='Row cell label (defaults to the root name)')
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
        attempt = 0
        while (code != 0 and attempt < args.retry_failures and _start_hang(args.root)):
            attempt += 1
            print(json.dumps({'cell': args.cell_label or args.root.name,
                              'retry_after_start_hang': attempt}, sort_keys=True))
            _cleanup_hosts()
            time.sleep(2)
            close(args.root)
            code, wall, log = boot(args.root, args.timeout)
        reference = None
        if args.reference is not None:
            reference = json.loads(args.reference.read_text())
        row = summarize(args.root, code, wall, args.layout, args.network, block, args.results,
                        cell=args.cell_label or args.root.name, provider_cpus=args.provider_cpus,
                        reference=reference, outputs_dir=args.outputs_dir, attempt=attempt)
        print(json.dumps(row, sort_keys=True))
        if code == 0 and args.prune_boot_dirs:
            _prune_boot_dirs(args.root, args.archive_dir)
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
