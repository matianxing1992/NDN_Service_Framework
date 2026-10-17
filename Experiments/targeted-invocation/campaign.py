#!/usr/bin/env python3
"""Freeze and run exactly the required matched matrix, with a two-hour budget."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import signal
import sys
import time
from run_minindn import candidate_identity, prepare_tls


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--workspace', default='/var/tmp/ndnsf-spec203')
    parser.add_argument('--wire-only', action='store_true')
    parser.add_argument('--four-way', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    cells = [{'delay_ms': delay, 'loss': loss, 'seed': seed}
             for delay, loss in ([(10, 0)] if args.wire_only else [(1, 0), (10, 0), (50, 0), (10, 1)])
             for seed in range(20301, 20306)]
    sources = list(here.glob('*.cpp')) + list(here.glob('*.hpp')) + list(here.glob('*.py')) + [here / 'wscript']
    if args.four_way:
        sources += list((here.parent / 'NDN_NSC').glob('*.cpp'))
    budget = 10800 if args.four_way else 1800 if args.wire_only else 7200
    frozen = {'status': 'PARTIAL', 'budget_seconds': budget, 'cells': cells,
              'measurement_family': 'four-way' if args.four_way else 'wire-supplement' if args.wire_only else 'primary',
              'candidate': candidate_identity(tls=prepare_tls(Path(args.workspace)) if args.four_way else None),
              'start_wall_ns': time.time_ns(), 'bootstrap_seed': 20399,
              'source_sha256': {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
              'completed': []}
    freeze_path = root / 'campaign.json'
    freeze_path.write_text(json.dumps(frozen, indent=2) + '\n')
    deadline = time.monotonic() + budget
    try:
        for cell in cells:
            for path in sources:
                if hashlib.sha256(path.read_bytes()).hexdigest() != frozen['source_sha256'][str(path.resolve())]:
                    raise RuntimeError('candidate source changed during campaign')
            label = 'd{}-loss{}-b{}'.format(cell['delay_ms'], cell['loss'], cell['seed'])
            command = [sys.executable, str(here / 'run_minindn.py'), '--workspace', args.workspace,
                       '--output-dir', str(root / label), '--delay-ms', str(cell['delay_ms']),
                       '--loss', str(cell['loss']), '--seed', str(cell['seed'])]
            command += ['--expected-candidate', str(freeze_path)]
            if args.four_way:
                command.append('--four-way')
            elif args.wire_only:
                command.append('--wire-only')
            elif cell == {'delay_ms': 10, 'loss': 0, 'seed': 20301}:
                command.append('--isolation')
            with (root / (label + '.log')).open('wb') as log:
                proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                try:
                    code = proc.wait(timeout=max(.1, deadline - time.monotonic()))
                except BaseException:
                    # SIGINT lets the Python cell execute its child/network finally.
                    proc.send_signal(signal.SIGINT)
                    try:
                        proc.wait(timeout=60)
                    except subprocess.TimeoutExpired:
                        proc.kill(); proc.wait()
                    raise
                if code:
                    raise subprocess.CalledProcessError(code, command)
            frozen['completed'].append(label)
            freeze_path.write_text(json.dumps(frozen, indent=2) + '\n')
            print('COMPLETED', label, flush=True)
        frozen['status'] = 'RUN_COMPLETE'
    except Exception as error:
        frozen['first_failure'] = str(error)
        raise
    finally:
        frozen['end_wall_ns'] = time.time_ns()
        freeze_path.write_text(json.dumps(frozen, indent=2) + '\n')


if __name__ == '__main__':
    main()
