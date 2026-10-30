"""Delegate current experiment execution to the installed deployment owner.

Historical profile/material-preparation commands remain explicit compatibility
paths. This module owns no inference or application lifecycle.
"""
import argparse
from pathlib import Path
import subprocess


def run_deployment(argv):
    parser = argparse.ArgumentParser(
        description='Run the same installed native applications as Tiger using deployment JSON.')
    parser.add_argument('action', nargs='?', default='run', choices=('check', 'run', 'status', 'down'))
    parser.add_argument('--deployment', required=True)
    parser.add_argument('--timeout', type=float, default=1800)
    parser.add_argument('--debug', action='store_true')
    parser.add_argument('--verify', action='store_true',
                        help='verify first/warm dependency and namespace evidence with the C++ oracle')
    parser.add_argument('--oracle', type=Path, help='explicit native oracle candidate for qualification')
    args = parser.parse_args(argv)
    if args.debug and args.action != 'run':
        parser.error('--debug only applies to run')
    if (args.verify and args.action != 'run') or (args.oracle and not args.verify):
        parser.error('--verify applies to run; --oracle requires --verify')
    from ndnsf_distributed_inference.ops.cli import main
    oracle = None
    if args.verify:
        from ndnsf_distributed_inference.ops.deployment_config import load_deployment
        config = load_deployment(args.deployment)
        if config['platform']['kind'] != 'minindn':
            parser.error('this local qualification requires platform.kind=minindn')
        oracle = (args.oracle or Path(config['release']['prefix']) /
                  'bin/spec190-multiturn-oracle').resolve(strict=True)
    action = 'up' if args.action == 'run' else args.action
    command = ['cluster', action, '--config', args.deployment]
    if action != 'check':
        command.extend(['--timeout', str(args.timeout)])
    if args.debug:
        command.append('--debug')
    result = main(command)
    if result or not args.verify:
        return result
    from ndnsf_distributed_inference.ops.cluster import _locator
    _, root = _locator(args.deployment)
    completed = subprocess.run([str(oracle), '--deployment-state',
                               str(root / 'state.json'), '--require-multi-token'],
                              stdin=subprocess.DEVNULL, timeout=60)
    return completed.returncode
