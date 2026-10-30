"""Bounded, opt-in kernel observation of owned MiniNDN actors; no app decisions."""
import hashlib
import os
import json
from pathlib import Path
import re
import signal
import subprocess
import time


class KernelMeasurement:
    def __init__(self, output, config):
        from ndnsf_distributed_inference.ops.processes import write_json
        self.write_json = write_json
        self.output, self.config = output, config
        self.child = None
        self.interfaces = []
        self.targets = []
        self.closed = False

    def facility_ready(self, facility):
        for node in self.config['nodes']:
            host = facility.hosts[node['host']]
            interfaces = [x.name for x in host.intfList() if x.name != 'lo']
            if len(interfaces) != 1:
                raise RuntimeError('KERNEL_MEASUREMENT_REQUIRES_ONE_LINK')
            self.interfaces.append(dict(node=node['id'], name=interfaces[0],
                namespace=facility.namespace(node['id'])))

    def threads(self):
        from ndnsf_distributed_inference.ops.processes import process_identity
        rows = []
        for target in self.targets:
            proof = process_identity(target['proof']['pid'])
            if not proof or any(proof[k] != target['proof'][k]
                                for k in ('start_ticks', 'exe', 'bindings')):
                raise RuntimeError('KERNEL_MEASUREMENT_TARGET_CHANGED')
            for path in (Path('/proc') / str(proof['pid']) / 'task').iterdir():
                try:
                    stat = (path / 'stat').read_text()
                    rows.append(dict(actor=target['id'], pid=proof['pid'], tid=int(path.name),
                        name=stat.split('(', 1)[1].rsplit(')', 1)[0],
                        start_ticks=int(stat.rsplit(')', 1)[1].split()[19])))
                except FileNotFoundError:
                    continue
        return sorted(rows, key=lambda x: x['tid'])

    def observe(self, children):
        from ndnsf_distributed_inference.ops.processes import process_identity
        if self.closed:
            return
        if self.child is not None:
            if self.child.poll() is not None:
                self.close()
            return
        checkpoint = Path(self.config['requester']['turns'][0]['checkpoint_output_file'])
        if not checkpoint.is_file():
            return
        requester = next((row for actor, child, row in children if actor['kind'] == 'requester'), None)
        if requester is None:
            return
        stats = Path(requester['log']).parent / 'requester-statistics.jsonl'
        if not stats.is_file():
            return
        first_requests = [json.loads(line) for line in stats.read_text().splitlines(keepends=True)
                          if line.startswith('{') and line.endswith('\n')]
        first_requests = [row for row in first_requests
                          if row.get('type') == 'request' and row.get('turn') == 0]
        if len(first_requests) != 1:
            return
        trigger = None
        for actor, child, row in children:
            if actor['kind'] != 'provider':
                continue
            for line in Path(row['log']).read_text().splitlines(keepends=True):
                if not line.endswith('\n') or 'NDNSF_DI_FEEDBACK_BOUNDARY ' not in line:
                    continue
                fields = dict(re.findall(r'(\w+)=([^ \n]+)', line))
                if (fields.get('boundary') == 'ready' and fields.get('requestId') and
                        fields['requestId'] != first_requests[0]['requestId']):
                    trigger = fields
                    break
        if trigger is None:
            return
        self.targets = [dict(id=row['id'], proof=row['proof'])
                        for actor, child, row in children if actor['kind'] in ('provider', 'nfd')]
        if len(self.targets) != 4 or len(self.interfaces) != 2:
            raise RuntimeError('KERNEL_MEASUREMENT_TARGETS_INCOMPLETE')
        self.initial_threads = self.threads()
        tids = [str(x['tid']) for x in self.initial_threads]
        if not tids or len(tids) > 256:
            raise RuntimeError('KERNEL_MEASUREMENT_THREAD_BOUND')
        net_filter = ' || '.join('name == "' + x['name'] + '"' for x in self.interfaces)
        switch_filter = ' || '.join('prev_pid == ' + x + ' || next_pid == ' + x for x in tids)
        wake_filter = ' || '.join('pid == ' + x for x in tids)
        perf = (Path('/usr/lib/linux-tools') / os.uname().release / 'perf').resolve(strict=True)
        if perf.read_bytes()[:4] != b'\x7fELF':
            raise RuntimeError('KERNEL_MEASUREMENT_PERF_NOT_ELF')
        command = [str(perf), 'record', '-a', '--clockid', 'mono', '--no-buildid',
                   '--max-size', '32M', '-m', '64', '-o', str(self.output / 'kernel.data')]
        for event, condition in [('net:net_dev_queue', net_filter),
                                 ('net:net_dev_start_xmit', net_filter),
                                 ('net:net_dev_xmit', net_filter),
                                 ('sched:sched_switch', switch_filter),
                                 ('sched:sched_wakeup', wake_filter),
                                 ('sched:sched_wakeup_new', wake_filter)]:
            command += ['-e', event, '--filter', condition]
        command += ['--', '/usr/bin/sleep', '60']
        env = dict(os.environ)
        self.bindings = dict(self.targets[0]['proof']['bindings'])
        self.bindings['NDNSF_DEPLOYMENT_ACTOR_ID'] = 'measurement.kernel'
        env.update(self.bindings)
        self.started = time.monotonic_ns()
        self.log = (self.output / 'kernel.log').open('wb')
        self.proof = None
        try:
            self.child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=self.log,
                                          stderr=subprocess.STDOUT, start_new_session=True, env=env)
        except BaseException:
            self.log.close()
            raise
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            proof = process_identity(self.child.pid)
            if proof and proof['exe'] == str(perf) and proof['bindings'] == self.bindings:
                self.proof = proof
                break
            if self.child.poll() is not None:
                break
            time.sleep(0.01)
        self.write_json(self.output / 'kernel-start.json', dict(command=command,
            proof=self.proof, targets=self.targets, threads=self.initial_threads,
            interfaces=self.interfaces, started_steady_ns=self.started,
            perf_sha256=hashlib.sha256(perf.read_bytes()).hexdigest(),
            trigger='first warm feedback ready; early warm intervals precede trace enable',
            trigger_record=trigger,
            clock='CLOCK_MONOTONIC', window_seconds=60, max_data_bytes=33554432,
            scope='diagnostic only; same thread IDs sampled at start/end, not full lifetime coverage'))
        if not self.proof:
            raise RuntimeError('KERNEL_MEASUREMENT_IDENTITY_UNVERIFIED')

    def close(self):
        from ndnsf_distributed_inference.ops.processes import stop_owned
        if self.closed:
            return
        if self.child is None:
            self.write_json(self.output / 'kernel-close.json', dict(status='NOT_STARTED'))
            self.closed = True
            return
        target_error = None
        try:
            final_threads = self.threads()
        except (OSError, RuntimeError) as error:
            final_threads = []
            target_error = str(error)
        interrupted = self.child.poll() is None
        forced = False
        cleanup_error = None
        group = dict(status='UNKNOWN')
        try:
            if interrupted:
                try:
                    os.killpg(self.child.pid, signal.SIGINT)
                except ProcessLookupError:
                    pass
            try:
                self.child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                forced = True
                try:
                    os.killpg(self.child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.child.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired) as error:
            cleanup_error = repr(error)
        finally:
            self.log.close()
            try:
                group = stop_owned(self.proof) if self.proof else dict(status='UNKNOWN')
            except (OSError, ValueError) as error:
                cleanup_error = cleanup_error or repr(error)
            self.closed = group['status'] == 'ZERO' and self.child.poll() is not None
            self.write_json(self.output / 'kernel-close.json', dict(
                status='CLOSED' if self.closed else 'UNKNOWN', exit_code=self.child.returncode,
                interrupted=interrupted, forced=forced, group=group,
                final_threads=final_threads, target_error=target_error, error=cleanup_error,
                elapsed_seconds=(time.monotonic_ns() - self.started) / 1e9,
                thread_set_unchanged=final_threads == self.initial_threads))
        expected_exit = (self.child.returncode == 0 or
                         (interrupted and self.child.returncode == -signal.SIGINT))
        if (forced or group['status'] != 'ZERO' or group.get('forced') or
                not expected_exit or target_error or cleanup_error):
            raise RuntimeError('KERNEL_MEASUREMENT_CLOSE_FAILED')
