#!/usr/bin/env python3
"""Qualify one separately orchestrated, same-User Provider restart run."""
import argparse
import json
from pathlib import Path
import re
from run_minindn import records


def qualify(root):
    manifest = json.loads((root / 'manifest.json').read_text())
    failures = []
    def require(condition, reason):
        if not condition:
            failures.append(reason)
    require(manifest.get('status') == 'RUN_COMPLETE', 'run/cleanup incomplete')
    require(manifest.get('measurement_family') == 'recovery', 'not a recovery run')
    require(not manifest.get('first_failure') and not manifest.get('cleanup_errors'), 'first failure or cleanup error')
    cut = manifest.get('recovery', {})
    require(manifest.get('window_seconds') == 45 and manifest.get('warmup_seconds') == 0
            and manifest.get('timeout_ms') == 5000 and cut.get('interval_ms') == 1000,
            'recovery workload configuration mismatch')
    require(cut.get('new_session') == cut.get('old_session', -2) + 1, 'SVS session not incremented')
    require(cut.get('old_provider_pid') != cut.get('new_provider_pid'), 'Provider process not replaced')
    rotation = bool(cut.get('certificate_rotation'))
    require(bool(cut.get('requester_pid')) and (rotation or cut.get('credentials_unchanged')), 'same-User/credential cut missing')
    users = records(root / 'provider-recovery.jsonl')
    ready = [r for r in users if r.get('event') == 'ready']
    require(len(ready) == 1 and ready[0].get('interval_ms') == 1000
            and ready[0]['t1_ns'] - ready[0]['t0_ns'] == 45000000000,
            'same-User readiness/window/pacing mismatch')
    terminals = [r for r in users if r.get('event') == 'terminal']
    require(bool(terminals), 'no User terminals')
    require(len({r['command'] for r in terminals}) == len(terminals), 'duplicate User terminal')
    require(len({r['request_id'] for r in terminals}) == len(terminals), 'duplicate request identity')
    require(all(r['terminal'] in ('success', 'timeout', 'response_error') for r in terminals), 'invalid/submit terminal')
    require(all(r['terminal_ns'] < r['deadline_ns'] for r in terminals if r['terminal'] == 'success'), 'late success')
    require(all(r['terminal_ns'] - r['deadline_ns'] <= 250000000 for r in terminals if r['terminal'] == 'timeout'), 'unbounded timeout dispatch')
    require(not any(r.get('event') == 'late_response_checked' and r.get('invalid') for r in users), 'invalid late response')
    success = {r['command']: r for r in terminals if r['terminal'] == 'success'}
    require(all(r.get('payload_verified') and r.get('response_identity_verified') and r.get('wire_digest')
                for r in success.values()), 'native success oracle missing')
    before = records(root / 'drone1-provider-recovery.jsonl')
    after = records(root / 'drone1-provider-recovery-restart.jsonl')
    other = records(root / 'drone2-provider-recovery.jsonl')
    old_exec = [r for r in before if r.get('event') == 'execution']
    new_exec = [r for r in after if r.get('event') == 'execution']
    require(not any(r.get('event') == 'execution' for r in other), 'non-target Provider executed')
    all_exec = old_exec + new_exec
    require(len({r['command'] for r in all_exec}) == len(all_exec), 'business execution replayed across restart')
    require(all(r.get('payload_valid') for r in all_exec), 'invalid handler payload')
    terminal_by_command = {r['command']: r for r in terminals}
    execution_by_command = {r['command']: r for r in all_exec}
    require(all(command in execution_by_command for command in success), 'success without handler execution')
    for execution in all_exec:
        terminal = terminal_by_command.get(execution['command'])
        require(terminal is not None, 'execution outside recorded invocations')
        if terminal:
            require(execution['request_id'] == terminal['request_id'], 'handler/request identity mismatch')
            require(terminal['begin_ns'] <= execution['begin_ns'] <= execution['end_ns'], 'handler timeline mismatch')
            if terminal['terminal'] == 'success':
                require(execution['end_ns'] <= terminal['terminal_ns'], 'success preceded handler completion')
    def verified(rows, mode):
        return [success[r['command']] for r in rows if r['actual_mode'] == mode and r['command'] in success]
    pre = verified(old_exec, 1)
    post = verified(new_exec, 1)
    bootstrap = verified(new_exec, 2)
    require(len(pre) >= 3 and len(post) >= 3, 'missing verified warm calls before/after restart')
    require(bool(bootstrap), 'no successful new bootstrap')
    if pre and post and bootstrap:
        old_boot = {r.get('provider_boot') for r in pre}
        new_boot = {r.get('provider_boot') for r in post + bootstrap}
        require(len(old_boot) == len(new_boot) == 1 and None not in old_boot | new_boot
                and old_boot != new_boot, 'missing or unchanged authenticated boot')
        old_profiles = {r.get('provider_security_profile') for r in pre}
        new_profiles = {r.get('provider_security_profile') for r in post + bootstrap}
        profiles = old_profiles | new_profiles
        require(len(old_profiles) == len(new_profiles) == 1 and None not in profiles, 'profile identity missing or unstable')
        require((old_profiles != new_profiles) if rotation else (old_profiles == new_profiles),
                'rotated profile did not change' if rotation else 'unchanged credentials did not reuse profile identity')
        first_bootstrap = min(r['begin_ns'] for r in bootstrap)
        require(all(r['begin_ns'] > first_bootstrap for r in post), 'warm recovery preceded bootstrap')
        log = (root / 'drone1-provider-recovery-restart.log').read_text(errors='replace')
        received = set(re.findall(r'NDNSF_PROTECTED_TARGETED_TOKEN_REJECTED[^\n]*requestId=(\S+) reason=Targeted ProviderToken is unknown or expired', log))
        executed = {r['request_id'] for r in new_exec}
        old_bound = {r['request_id'] for r in terminals if r['begin_ns'] < first_bootstrap}
        rejected = (received & old_bound) - executed
        require(bool(rejected), 'old-token receipt without execution not observed')
    else:
        rejected = set()
        profiles = set()
    return {'qualified': not failures, 'failures': failures, 'measurement_family': 'recovery',
            'pre_warm_successes': len(pre), 'post_warm_successes': len(post),
            'post_bootstrap_successes': len(bootstrap), 'old_token_received_without_execution': len(rejected),
            'timeouts': sum(r['terminal'] == 'timeout' for r in terminals),
            'response_errors': sum(r['terminal'] == 'response_error' for r in terminals),
            'certificate_rotation': rotation, 'profile_names': sorted(p for p in profiles if p),
            'session_before': cut.get('old_session'), 'session_after': cut.get('new_session')}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = qualify(args.directory.resolve())
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['qualified'] else 1)
