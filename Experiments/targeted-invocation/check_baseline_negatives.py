#!/usr/bin/env python3
"""Check rejection causes and actual NSC replay independently of four-arm statistics."""
import argparse
from collections import Counter
import json
from pathlib import Path
from analyze import read_rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--retry-directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    causes = {'nsc-echo': 'matched NSC result payload mismatch',
              'nsc-signer': 'matched NSC ACK validation failed',
              'grpc-echo': 'gRPC response payload/target mismatch',
              'grpc-target': 'grpc.FutureTimeoutError'}
    results = []
    for mode in [*causes, 'grpc-client-identity', 'nsc-retry', 'grpc-deadline', 'nsc-deadline']:
        p = args.retry_directory if mode == 'nsc-retry' else args.evidence/('four-way-negative-'+mode)
        manifest = json.loads((p/'manifest.json').read_text())
        arm = mode.split('-')[0]
        name = 'b20301-'+arm
        rows = read_rows(p/(name+'.jsonl'))
        provider = read_rows(p/(name+'-provider.jsonl'))
        terminals = [r for r in rows if r['event'] == 'terminal']
        handlers = Counter(r['command'] for r in provider if r['event'] == 'execution')
        successes = [r for r in terminals if r['terminal'] == 'success']
        failures = []
        children = {c['label']: c['exit'] for c in manifest['children']}
        expected_client_exit = 0 if mode in ['nsc-retry', 'grpc-client-identity', 'grpc-deadline', 'nsc-deadline'] else 1
        if children.get(name) != expected_client_exit or children.get(name+'-provider') != 0:
            failures.append('unexpected actor exit')
        if manifest.get('cleanup_errors') or not manifest.get('controller_writer_released_after_wait'):
            failures.append('cleanup incomplete')
        if any(len([r for r in provider if r['event'] == event]) != 1 for event in ['ready', 'shutdown']):
            failures.append('owner lifecycle')
        if mode in causes:
            if causes[mode] not in (p/(name+'.log')).read_text() or successes:
                failures.append('missing specific rejection or accepted invalid response')
        if mode == 'grpc-target' and 'No match found for server name: another-provider' not in (p/(name+'-provider.log')).read_text():
            failures.append('missing explicit TLS name rejection')
        if mode in ['nsc-signer', 'grpc-client-identity', 'grpc-target'] and handlers:
            failures.append('unauthorized handler execution')
        if mode == 'grpc-client-identity' and (not terminals or any(
                r['terminal'] != 'response_error' or r.get('error') != 'StatusCode.PERMISSION_DENIED'
                for r in terminals)):
            failures.append('client authorization not enforced')
        reuse = [r for r in provider if r['event'] == 'input_reuse']
        if mode == 'nsc-retry':
            if (not successes or len(successes) != len(terminals) or not reuse
                    or any(n != 1 for n in handlers.values())
                    or any(handlers[r['command']] != 1 or not r['payload_verified']
                           or r['terminal_ns'] >= r['deadline_ns'] for r in successes)
                    or any(handlers[r['command']] != 1 for r in reuse)):
                failures.append('replay did not reach dedup branch or invalid result')
        if mode.endswith('-deadline'):
            if (len(terminals) != 1 or successes or sum(handlers.values()) != 1
                    or terminals[0]['terminal'] != 'timeout'
                    or terminals[0]['terminal_ns'] < terminals[0]['deadline_ns']):
                failures.append('late handler result was not rejected by deadline')
            execution = next((r for r in provider if r['event'] == 'execution'), None)
            if (execution is None or not terminals or execution['command'] != terminals[0]['command']
                    or execution['end_ns'] <= terminals[0]['deadline_ns']):
                failures.append('no actual late execution evidence')
        results.append({'mode': mode, 'directory': str(p), 'pass': not failures,
                        'rejection': causes.get(mode), 'successes': len(successes),
                        'handlers': sum(handlers.values()), 'input_reuse': len(reuse),
                        'failures': failures})
    report = {'status': 'PASS' if all(r['pass'] for r in results) else 'FAIL', 'checks': results}
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report))
    if report['status'] != 'PASS':
        raise SystemExit(1)


if __name__ == '__main__':
    main()
