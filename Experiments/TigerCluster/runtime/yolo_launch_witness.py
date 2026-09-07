"""Emit a launch-bound PID inside the container, then exec the Provider.

The host launcher owns the output FD and generates the nonce. This is process
correlation, not authentication against a malicious container. Never equate a
host Apptainer PID with the Provider PID inside --containall's PID namespace.
"""
import argparse
import json
import os
import re


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--nonce', required=True)
    parser.add_argument('--role', required=True,
        choices=('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'))
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if re.fullmatch(r'[0-9a-f]{64}', args.nonce) is None or not command:
        parser.error('a 64-hex launch nonce and Provider command are required')
    record = dict(schema='tiger-provider-process-v1', nonce=args.nonce,
                  role=args.role, pid=os.getpid())
    print('TIGER_PROVIDER_PROCESS_STARTED '+json.dumps(record, sort_keys=True), flush=True)
    # exec preserves this namespace PID; a subprocess would not establish the
    # identity of the actual Provider that emits native getpid() evidence.
    os.execvp(command[0], command)


if __name__ == '__main__':
    main()
