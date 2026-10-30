#!/usr/bin/env python3
"""Observe installed native Qwen transfers without implementing application logic.

One capture point on a two-node link counts each packet once. Only headers are
captured; native timing records supply object identities and admission boundaries.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deployment', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--timeout', type=float, default=1800)
    parser.add_argument('--packet-payloads', action='store_true',
                        help='retain packet bytes for local Named Data timing diagnosis')
    parser.add_argument('--packet-limit', type=int, default=8192,
                        help='bounded full-payload capture limit (8192..65536 frames)')
    parser.add_argument('--dispatch-timing', action='store_true',
                        help='explicitly measure native pending-Interest/Face dispatch boundaries')
    parser.add_argument('--kernel-timing', action='store_true',
                        help='explicitly trace owned link queues and actor scheduling for60s in warm')
    parser.add_argument('--smoke', action='store_true', help='facility/capture only; no inference claim')
    args = parser.parse_args()
    if os.getuid() != 0 or not 0 < args.timeout <= 3600:
        parser.error('root and a bounded timeout <=3600 seconds are required')
    if not 8192 <= args.packet_limit <= 65536:
        parser.error('packet limit must be between 8192 and 65536')
    if args.kernel_timing and args.smoke:
        parser.error('kernel timing requires real first/warm inference')
    from ndnsf_distributed_inference.ops import cluster
    from ndnsf_distributed_inference.ops.deployment_config import load_deployment
    from ndnsf_distributed_inference.ops.minindn import MiniNdnFacility
    from ndnsf_distributed_inference.ops.processes import process_identity, stop_owned, write_json

    config = load_deployment(args.deployment)
    if config['platform']['kind'] != 'minindn' or len(config['nodes']) != 2:
        parser.error('this measurement requires exactly two MiniNDN nodes')
    if not args.smoke:
        turn = config['requester'].get('conversation', {}).get('turn', {})
        if 'retention_deadline_ms' in turn:
            retention = turn['retention_deadline_ms']
            required = (time.time_ns() // 1000000 + int(args.timeout * 1000) + 1 +
                        config['requester']['request']['timeout_ms'])
            if (isinstance(retention, bool) or not isinstance(retention, int) or
                    retention <= required):
                parser.error('STALE_CONVERSATION_RETENTION: prepare fresh configuration before running')
    output = args.output_dir.resolve()
    if output.exists():
        parser.error('output directory already exists; preserve and inspect its evidence')
    output.mkdir(mode=0o700, parents=True)
    source = Path(__file__).resolve()
    write_json(output / 'measurement.json', dict(schema='ndnsf-di-transfer-measurement-v1',
        deployment=str(args.deployment.resolve()), source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        capture=(f'single endpoint, UDP, full snaplen/raw payloads, at most{args.packet_limit}frames' if args.packet_payloads
                 else 'single endpoint, UDP, 96-byte snaplen; frame.len/UDP lengths are original lengths'),
        diagnostic_logging=False, dispatch_timing=args.dispatch_timing,
        kernel_timing=args.kernel_timing,
        native_timing='ndnsf.di.RuntimeEvidence=WARN; NDNSF_DI_RUNTIME_TIMING=1',
        backend='unchanged installed C++ applications', smoke=args.smoke, timeout=args.timeout))
    original_environment = cluster._environment
    original_start, original_close = MiniNdnFacility.start, MiniNdnFacility.close
    original_check = cluster._check_running
    original_cluster_close = cluster._close
    kernel = None
    if args.kernel_timing:
        from ndnsf_di_kernel_measurement import KernelMeasurement
        kernel = KernelMeasurement(output, config)

    def checked_running(children):
        original_check(children)
        if kernel:
            kernel.observe(children)

    def close_cluster(state, root, facility=None):
        try:
            if kernel:
                kernel.close()
        finally:
            closed = original_cluster_close(state, root, facility)
        return closed

    captures = []

    def measured_environment(home, socket, actor_id, state):
        env = original_environment(home, socket, actor_id, state)
        if actor_id in {actor['id'] for actor in config['actors'] if actor['kind'] == 'provider'}:
            env.update(NDN_LOG='*=NONE:ndnsf.di.RuntimeEvidence=WARN', NDNSF_DI_RUNTIME_TIMING='1')
            if args.dispatch_timing:
                env['NDN_LOG'] += ':ndn_service_framework.ServiceProvider=WARN'
                env['NDNSF_PENDING_IMS_TIMING'] = '1'
        return env

    def start_capture(facility, guard):
        original_start(facility, guard)
        if kernel:
            kernel.facility_ready(facility)
        host = facility.hosts[config['nodes'][0]['host']]
        interfaces = [interface.name for interface in host.intfList() if interface.name != 'lo']
        if len(interfaces) != 1:
            raise RuntimeError('MEASUREMENT_REQUIRES_ONE_LINK_PER_NODE')
        command = ['nsenter', '-t', str(host.pid), '-n', '/usr/bin/tshark',
                   '-i', interfaces[0], '-f', 'udp', '-s',
                   '65535' if args.packet_payloads else '96', '-B', '16',
                   '-a', 'duration:' + str(int(args.timeout) + 30),
                   '-c', str(args.packet_limit) if args.packet_payloads else '1000000',
                   '-l', '-n', '-T', 'fields', '-E', 'header=y']
        for field in ['frame.time_epoch', 'frame.len', 'frame.cap_len', 'ip.len',
                      'udp.length', 'ip.src', 'ip.dst', 'udp.srcport', 'udp.dstport']:
            command.extend(['-e', field])
        if args.packet_payloads:
            for field in ['data.data', 'ip.id', 'ip.frag_offset', 'ip.flags.mf']:
                command.extend(['-e', field])
        qdisc = subprocess.run(['nsenter', '-t', str(host.pid), '-n',
                                '/sbin/tc', '-j', 'qdisc', 'show', 'dev', interfaces[0]],
                               capture_output=True, text=True, timeout=5)
        write_json(output / 'link-qdisc.json', dict(command=qdisc.args,
                   exit_code=qdisc.returncode, stdout=qdisc.stdout, stderr=qdisc.stderr))
        if qdisc.returncode:
            raise RuntimeError('MEASUREMENT_QDISC_PROBE_FAILED')
        stdout = (output / 'link-packets.tsv').open('wb')
        stderr = (output / 'capture.log').open('wb')
        capture_environment = dict(os.environ)
        bindings = dict(NDNSF_DEPLOYMENT_ACTOR_ID='measurement.capture',
                        NDNSF_DEPLOYMENT_BOOT_ID=facility.state['boot_id'],
                        NDNSF_DEPLOYMENT_CONFIG_SHA256=facility.state['configuration_sha256'])
        capture_environment.update(bindings)
        try:
            child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout,
                                     stderr=stderr, start_new_session=True, env=capture_environment)
        except BaseException:
            stdout.close()
            stderr.close()
            raise
        row = dict(child=child, stdout=stdout, stderr=stderr, command=command)
        captures.append(row)
        deadline = time.monotonic() + 10
        while True:
            guard()
            if child.poll() is not None:
                raise RuntimeError('CAPTURE_EXIT_BEFORE_READY:' + str(child.returncode))
            proof = process_identity(child.pid)
            if ('Capturing on' in (output / 'capture.log').read_text(errors='replace') and
                    proof and proof.get('exe') == '/usr/bin/tshark' and
                    proof.get('bindings') == bindings):
                row['proof'] = proof
                write_json(output / 'capture-start.json', dict(command=command, proof=proof,
                    network_namespace=facility.namespace(config['nodes'][0]['id']), interface=interfaces[0],
                    ready_epoch_ns=time.time_ns(), ready_steady_ns=time.monotonic_ns()))
                # The startup notice precedes the packet reader's first flush.
                # Give it a bounded settling interval before applications start.
                time.sleep(1)
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('CAPTURE_READY_TIMEOUT')
            time.sleep(0.05)

    def close_captures():
        rows = []
        for row in captures:
            child = row['child']
            if 'closed' in row:
                rows.append(row['closed'])
                continue
            forced, error = False, None
            before = child.poll()
            if before is not None:
                error = 'CAPTURE_EXITED_BEFORE_MEASUREMENT_CLOSE'
            try:
                if before is None:
                    os.killpg(child.pid, signal.SIGINT)
                child.wait(timeout=10)
            except BaseException as failure:
                error = repr(failure)
                forced = True
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait(timeout=5)
            finally:
                row['stdout'].close()
                row['stderr'].close()
            group = stop_owned(row['proof']) if row.get('proof') else dict(status='UNKNOWN')
            closed = dict(pid=child.pid, exit_code=child.returncode, forced=forced or group.get('forced', False), error=error,
                          exited_before_close=before is not None, reaped=child.poll() is not None,
                          group_state=group['status'], group_cleanup=group)
            row['closed'] = closed
            rows.append(closed)
        write_json(output / 'capture-close.json', rows)
        return rows

    def close_facility(facility):
        try:
            try:
                if kernel:
                    kernel.close()
            finally:
                close_captures()
        finally:
            closed = original_close(facility)
        return closed

    def interrupted(signum, frame):
        raise KeyboardInterrupt('MEASUREMENT_SIGNAL:' + str(signum))

    handlers = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
    cluster._environment = measured_environment
    cluster._check_running = checked_running
    cluster._close = close_cluster
    MiniNdnFacility.start, MiniNdnFacility.close = start_capture, close_facility
    started, before = time.monotonic(), os.times()
    result, failure, oracle = None, None, None
    try:
        if args.smoke:
            state = dict(boot_id='e' * 32, configuration_sha256='f' * 64,
                         evidence_root=str(output), launches=[])
            facility = MiniNdnFacility(config, output, state, time.monotonic() + 20)
            try:
                facility.start(lambda: None)
                first, second = [facility.hosts[node['host']] for node in config['nodes']]
                code = ('import socket,time; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); '
                        '[ (s.sendto(b"capture-smoke", (' + repr(second.IP()) +
                        ',6363)), time.sleep(0.2)) for _ in range(5) ]')
                probe = facility.launcher(config['nodes'][0]['id'])(
                    ['/usr/bin/python3', '-c', code])
                if probe.wait(timeout=3):
                    raise RuntimeError('SMOKE_UDP_PROBE_FAILED')
                time.sleep(0.25)
            finally:
                complete = facility.close()
            if not complete:
                raise RuntimeError('SMOKE_FACILITY_CLOSE_FAILED')
            result = dict(status='SMOKE_OK', inference='NOT_RUN')
        else:
            result = cluster.run_cluster('up', args.deployment, timeout=args.timeout)
            write_json(output / 'result.json', result)
            if result.get('execution') != 'COMPLETE':
                raise RuntimeError('NATIVE_EXECUTION_NOT_COMPLETE:' + str(result.get('failure')))
            _, root = cluster._locator(args.deployment)
            command = [str(Path(config['release']['prefix']) / 'bin/spec190-multiturn-oracle'),
                       '--deployment-state', str(root / 'state.json'), '--require-multi-token']
            check = subprocess.run(command, capture_output=True, text=True, timeout=60)
            oracle = dict(command=command, exit_code=check.returncode, stdout=check.stdout, stderr=check.stderr)
            write_json(output / 'oracle.json', oracle)
            if check.returncode:
                raise RuntimeError('NATIVE_ORACLE_FAILED')
    except BaseException as error:
        failure = repr(error)
    finally:
        cleanup = close_captures()
        if not cleanup or any(row['error'] or row['forced'] or row['group_state'] != 'ZERO' for row in cleanup):
            failure = failure or 'CAPTURE_CLOSE_UNVERIFIED'
        cluster._environment = original_environment
        cluster._check_running = original_check
        cluster._close = original_cluster_close
        MiniNdnFacility.start, MiniNdnFacility.close = original_start, original_close
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
        after = os.times()
        write_json(output / 'receipt.json', dict(status='PASS' if failure is None else 'FAILED',
            failure=failure, result=result, oracle=oracle, capture_cleanup=cleanup,
            wall_seconds=time.monotonic() - started,
            child_cpu_seconds=after.children_user + after.children_system - before.children_user - before.children_system,
            measurement_scope='single local first/warm trial; instrumentation enabled; not a link capacity benchmark'))
    if failure:
        raise SystemExit(failure)
    if not cleanup or any(row['error'] or row['forced'] or row['group_state'] != 'ZERO' for row in cleanup):
        raise SystemExit('CAPTURE_CLOSE_UNVERIFIED')
    if args.smoke and len((output / 'link-packets.tsv').read_text().splitlines()) < 2:
        write_json(output / 'smoke-verification.json', dict(status='FAILED', reason='NO_CAPTURED_UDP_SAMPLE'))
        raise SystemExit('NO_CAPTURED_UDP_SAMPLE')
    print('SMOKE_OK' if args.smoke else 'NDNSF_DI_TRANSFER_MEASUREMENT_EXECUTION_PASS')


if __name__ == '__main__':
    main()
