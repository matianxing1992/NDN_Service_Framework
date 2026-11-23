#!/usr/bin/env python3
"""Check cancellation and expiry at the installed Wan Requester boundary."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def requester_state(proof, prefix):
    proc = Path('/proc') / str(proof['pid'])
    fields = (proc / 'stat').read_text().rsplit(')', 1)[1].split()
    require(int(fields[19]) == proof['start_ticks'] and proc.stat().st_uid == proof['uid'] and
            (proc / 'exe').resolve(strict=True) == prefix / 'bin/DI_NativeRequester',
            'live requester identity differs from this boot')
    return fields[0]


def retained_publication(launches, request_id):
    for actor in launches:
        if actor['id'] not in ('provider-0', 'provider-1'):
            continue
        with Path(actor['log']).open() as log:
            for line in log:
                if ('NDNSF_DI_DEPENDENCY_OBJECT session=' + request_id + ' ' in line and
                        ' direction=publish-exact-ndn ' in line and line.endswith(' status=ok\n')):
                    return line.strip()
    return None


def check_preparation_cancellation(args):
    source = args.source_deployment.resolve(strict=True)
    prefix = args.prefix.resolve(strict=True)
    raw = json.loads(source.read_text())
    require(raw['platform']['kind'] == 'minindn' and
            raw['requester']['model_input']['mode'] == 'source' and
            'wan_generation' in raw['requester'], 'expected a cold MiniNDN Wan deployment')
    state_path = source.parent / raw['storage']['state_root'] / raw['deployment_id'] / 'state.json'
    require(not state_path.exists(), 'source cancellation requires a fresh operation root')
    model = raw['requester']['model_input']
    journal = source.parent / model['preparation_state_file']
    handle = source.parent / model['private_handle_file']
    destination = source.parent / raw['requester']['wan_generation']['output_directory']
    require(not journal.exists() and not handle.exists(), 'source preparation is not cold')
    args.logs.mkdir(mode=0o700)
    logs = args.logs.resolve()
    command = [str(args.cluster_cli), 'cluster']
    config = ['--config', str(source)]
    with (logs / 'check.log').open('x') as log:
        subprocess.run(command + ['check'] + config, check=True, stdout=log,
                       stderr=subprocess.STDOUT, cwd=logs, timeout=30)
    with (logs / 'up.log').open('x') as log:
        up = subprocess.Popen(command + ['up'] + config + ['--timing', '--timeout', '120'],
                              stdout=log, stderr=subprocess.STDOUT, cwd=logs)
        injected = None
        try:
            deadline = time.monotonic() + 75
            while up.poll() is None and time.monotonic() < deadline:
                try:
                    state = json.loads(state_path.read_text())
                    require(state['manager']['pid'] == up.pid, 'source operation manager differs')
                    actor = next((row for row in state['launches']
                                  if row['id'] == 'requester'), None)
                    if actor is not None and journal.is_file() and not handle.exists():
                        requester_state(actor['proof'], prefix)
                        injected = dict(boot=state['boot_id'], pid=actor['proof']['pid'],
                                        timestamp_ns=time.time_ns(), journal_bytes=journal.stat().st_size)
                        require(injected['journal_bytes'] > 0, 'preparation journal is empty')
                        os.kill(injected['pid'], signal.SIGTERM)
                        break
                except (FileNotFoundError, ProcessLookupError):
                    pass
                time.sleep(0.01)
            require(injected is not None, 'source preparation injection boundary was not reached')
            up.wait(timeout=45)
            terminal = json.loads(state_path.read_text())
            require(terminal['boot_id'] == injected['boot'], 'source terminal boot differs')
            requester = next(row for row in terminal['launches'] if row['id'] == 'requester')
            lines = Path(requester['log']).read_text().splitlines()
            failures = [line for line in lines if 'NATIVE_REQUEST_STAGE_FAILED code=' in line]
            require(failures and 'code=WAN_GENERATION_CANCELLED ' in failures[0],
                    f'wrong source preparation failure: {failures[:1]}')
            require(terminal['requester']['exit_code'] == 130, 'source cancellation exit differs')
            require(not any('event=REQUEST_PUBLISHED' in line and
                            'requestId=/NDNSF/DI/REQUEST/' in line for line in lines),
                    'source cancellation submitted an inference child')
            require(journal.is_file() and not handle.exists(), 'source journal/handle boundary differs')
            require(not destination.exists() and not any(destination.parent.glob(destination.name + '-*')),
                    'source cancellation produced a clip or staging directory')
            require(terminal['cleanup'] and all(row['status'] == 'ZERO' and
                    row.get('forced') is False for row in terminal['cleanup']), 'source cleanup failed')
            require(terminal['facility']['status'] == 'STOPPED' and
                    not terminal['facility']['remaining_interfaces'] and
                    not terminal['facility']['namespace_survivors'], 'source facility remains active')
            result = dict(status='PASS', scope='native Wan source-preparation cancellation',
                          injection=injected, operation=str(Path(terminal['evidence_root']) / 'operation.json'),
                          requester=terminal['requester'], cleanup=terminal['cleanup'])
        finally:
            with (logs / 'down.log').open('x') as output:
                subprocess.run(command + ['down'] + config + ['--timeout', '60'], check=True,
                               stdout=output, stderr=subprocess.STDOUT, cwd=logs, timeout=75)
            if up.poll() is None:
                up.wait(timeout=45)
    final = json.loads(state_path.read_text())
    require(not final.get('needs_closeout'), 'source cancellation closeout remains pending')
    with (logs / 'result.json').open('x') as output:
        json.dump(result, output, sort_keys=True, indent=2)
    print(json.dumps(result, sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--cluster-cli', type=Path, help='installed ndnsf-di host launcher')
    input_group = parser.add_mutually_exclusive_group(required=True)
    input_group.add_argument('--warm-deployment', type=Path,
                             help='completed warm TP deployment')
    input_group.add_argument('--source-deployment', type=Path,
                             help='fresh source-mode deployment; cancel when its journal is created')
    parser.add_argument('--logs', type=Path, required=True, help='fresh log directory')
    fault_group = parser.add_mutually_exclusive_group()
    fault_group.add_argument('--expire-inflight', action='store_true',
                             help='hold the first negative DiT request across the deadline')
    fault_group.add_argument('--cancel-after-publication', action='store_true',
                             help='cancel after the first DiT stores a signed exact object')
    args = parser.parse_args()
    args.cluster_cli = (args.cluster_cli or args.prefix / 'libexec/ndnsf-python/bin/ndnsf-di')
    args.cluster_cli = args.cluster_cli.resolve(strict=True)
    if args.source_deployment:
        if args.expire_inflight or args.cancel_after_publication:
            parser.error('source preparation cancellation does not select an inference fault')
        check_preparation_cancellation(args)
        return

    warm = args.warm_deployment.resolve(strict=True)
    tp = warm.parent
    prefix = args.prefix.resolve(strict=True)
    raw = json.loads(warm.read_text())
    require(raw['deployment_id'] == 'wan-video-tp' and
            raw['requester']['model_input']['mode'] in ('source-cache', 'stored'),
            'expected a completed warm TP deployment')
    args.logs.mkdir(mode=0o700)
    logs = args.logs.resolve()
    run_name = logs.name
    gen = raw['requester']['wan_generation']
    fault = 'expired' if args.expire_inflight else 'cancelled'
    child_number = 4 if args.expire_inflight else 3
    if args.cancel_after_publication:
        providers = [actor for actor in raw['actors'] if actor['kind'] == 'provider']
        require(len(providers) == 2 and all(
            actor['parameters'].get('exact-packet-disk-mib', 0) > 0 for actor in providers),
            'publication cut requires file-backed exact Data on both TP Providers')
    if args.expire_inflight:
        require(raw['requester'].get('test_wan_fixture_trace'),
                'expiry check requires the existing fixture trace')
        gen['timeout_ms'] = 30_000
    destination = tp / 'evidence' / f'clip-{fault}-{run_name}'
    gen['output_directory'] = str(destination)
    gen['generation_id'] += f'-{fault}-{run_name}'
    source = tp / f'deployment-{fault}-{run_name}.json'
    with source.open('x') as stream:
        json.dump(raw, stream, sort_keys=True)
    state_path = tp / raw['storage']['state_root'] / raw['deployment_id'] / 'state.json'
    old_boot = json.loads(state_path.read_text())['boot_id']
    command = [str(args.cluster_cli), 'cluster']
    config = ['--config', str(source)]
    with (logs / 'check.log').open('x') as stream:
        subprocess.run(command + ['check'] + config, check=True, stdout=stream,
                       stderr=subprocess.STDOUT, cwd=logs, timeout=30)
    up_log = (logs / 'up.log').open('x')
    up = subprocess.Popen(command + ['up'] + config + ['--timing', '--timeout', '120'],
                          stdout=up_log, stderr=subprocess.STDOUT, cwd=logs)
    paused = None
    try:
        deadline = time.monotonic() + 75
        injected = None
        log_path = None
        while up.poll() is None and time.monotonic() < deadline:
            try:
                state = json.loads(state_path.read_text())
                if state['boot_id'] == old_boot or state['manager']['pid'] != up.pid:
                    time.sleep(0.01)
                    continue
                actor = next((row for row in state['launches'] if row['id'] == 'requester'), None)
                if actor is None:
                    time.sleep(0.01)
                    continue
                log_path = Path(actor['log'])
                lines = log_path.read_text().splitlines()
                match = next((line for line in lines if 'event=SELECTION_PUBLISHED' in line and
                              re.search(rf'requestId=/NDNSF/DI/REQUEST/[0-9a-f]+-{child_number} ',
                                        line)), None)
                if match:
                    request_id = match.split('requestId=', 1)[1].split()[0]
                    publication = None
                    if args.cancel_after_publication:
                        publication = retained_publication(state['launches'], request_id)
                        if publication is None:
                            time.sleep(0.01)
                            continue
                    proof = actor['proof']
                    requester_state(proof, prefix)
                    if args.expire_inflight:
                        os.kill(proof['pid'], signal.SIGSTOP)
                        paused = proof
                    else:
                        os.kill(proof['pid'], signal.SIGTERM)
                    injected = dict(boot=state['boot_id'], pid=proof['pid'], event=match,
                                    request_id=request_id,
                                    timestamp_ns=time.time_ns())
                    if publication is not None:
                        injected['retained_publication'] = publication
                    break
            except (FileNotFoundError, ProcessLookupError):
                pass
            time.sleep(0.01)
        if injected is None:
            raise RuntimeError('no observed fault boundary in the owned denoiser execution')
        print(json.dumps(injected, sort_keys=True), flush=True)
        if paused is not None:
            stop_deadline = time.monotonic() + 2
            while requester_state(paused, prefix) != 'T':
                require(time.monotonic() < stop_deadline, 'requester did not pause')
                time.sleep(0.01)
            staging_paths = list(destination.parent.glob(destination.name + '-*'))
            require(len(staging_paths) == 1, 'expected one private clip staging directory')
            staged = staging_paths[0]
            require(sorted(path.name for path in staged.glob('trace-*.bundle')) ==
                    ['trace-0.bundle', 'trace-1.bundle', 'trace-2.bundle'] and
                    (staged / 'frames.rgb24').stat().st_size == 0 and
                    not (staged / 'manifest.json').exists() and not destination.exists(),
                    'unexpected clip or sampler progress before the deadline injection')
            # The generation deadline started before this Selection.
            time.sleep(gen['timeout_ms'] / 1000)
            require(requester_state(paused, prefix) == 'T', 'requester resumed before expiry')
            os.kill(paused['pid'], signal.SIGCONT)
            paused = None
        up.wait(timeout=45)
        terminal = json.loads(state_path.read_text())
        lines = log_path.read_text().splitlines()
        issued = [re.search(r'requestId=(/NDNSF/DI/REQUEST/[0-9a-f]+-([0-9]+)) ', line)
                  for line in lines if 'event=REQUEST_PUBLISHED' in line]
        issued = {int(match.group(2)) for match in issued if match is not None}
        require(issued == set(range(1, child_number + 1)),
                f'unexpected submitted child requests: {sorted(issued)}')
        expected = (('WAN_GENERATION_TIMEOUT', 'NATIVE_REQUEST_TIMEOUT')
                    if args.expire_inflight else ('WAN_GENERATION_CANCELLED',))
        failures = [line for line in lines if 'NATIVE_REQUEST_STAGE_FAILED code=' in line]
        require(failures and failures[0].split('code=', 1)[1].split()[0] in expected,
                f'wrong first failure: {failures[:1]}')
        request_field = 'requestId=' + injected['request_id'] + ' '
        child_events = [line for line in lines if request_field in line]
        require(not any('event=CALLBACK_FIRED' in line for line in child_events),
                'the selected child delivered a result')
        if not args.expire_inflight:
            require(any('event=CANCELLED_OR_DROPPED' in line for line in child_events),
                    'the selected child did not reach the Core cancellation boundary')
        staging = any(path.name.startswith(destination.name + '-')
                      for path in destination.parent.iterdir())
        require(not destination.exists() and not staging,
                'cancelled execution exposed clip output or left staging files')
        require(terminal['boot_id'] == injected['boot'] and terminal['requester']['exit_code'] != 0,
                'the injected requester did not fail')
        cleanup = terminal['cleanup'] + terminal['facility']['cleanup']
        expected_actors = {actor['id'] for actor in raw['actors']}
        expected_actors.update('nfd.' + node['id'] for node in raw['nodes']
                               if node['nfd']['mode'] == 'managed')
        require(len(terminal['cleanup']) == len(expected_actors) and
                {row['id'] for row in terminal['cleanup']} == expected_actors and
                len(terminal['facility']['cleanup']) == len(raw['nodes']),
                'incomplete TP deployment cleanup')
        require(all(row['forced'] is False and row['status'] == 'ZERO'
                    for row in cleanup) and
                not terminal['facility']['remaining_interfaces'] and
                not terminal['facility']['namespace_survivors'], 'deployment cleanup was incomplete')
        print(json.dumps(dict(issued=sorted(issued), first_failure=failures[0],
                              cleanup=len(cleanup), wall=terminal['requester']['wall_seconds'],
                              config=terminal['configuration_sha256']), sort_keys=True), flush=True)
    finally:
        if paused is not None:
            try:
                if requester_state(paused, prefix) == 'T':
                    os.kill(paused['pid'], signal.SIGCONT)
            except (FileNotFoundError, ProcessLookupError, RuntimeError):
                pass
        up_log.close()
        with (logs / 'down.log').open('x') as stream:
            subprocess.run(command + ['down'] + config + ['--timeout', '30'], stdout=stream,
                           stderr=subprocess.STDOUT, cwd=logs, timeout=65, check=True)

if __name__ == '__main__':
    main()
