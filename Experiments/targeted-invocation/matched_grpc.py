#!/usr/bin/env python3
"""64-byte echo measurement using the repository's Greeter schema."""
import argparse
from concurrent import futures
import json
from pathlib import Path
import signal
import sys
import threading
import time
import grpc

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gRPC'))
import helloworld_pb2 as messages
import helloworld_pb2_grpc as bindings


class Records:
    def __init__(self, path):
        self.file = Path(path).open('w')
        self.lock = threading.Lock()

    def write(self, row):
        row['schema'] = 'ndnsf-spec203-record-v1'
        with self.lock:
            self.file.write(json.dumps(row) + '\n')
            self.file.flush()


def server(args, records):
    class Echo(bindings.GreeterServicer):
        def SayHello(self, request, context):
            if context.auth_context().get('x509_common_name') != [b'gs']:
                context.abort(grpc.StatusCode.PERMISSION_DENIED, 'client identity mismatch')
            if len(request.name.encode()) != 64:
                context.abort(grpc.StatusCode.INVALID_ARGUMENT, 'payload length')
            begin = time.monotonic_ns()
            time.sleep(args.handler_ms / 1000)
            records.write({'event': 'execution', 'command': request.name,
                           'provider': 'drone1', 'begin_ns': begin,
                           'end_ns': time.monotonic_ns(), 'payload_valid': True})
            context.send_initial_metadata((('provider-id', 'drone1'),))
            return messages.HelloReply(message='wrong' if args.bad_echo else request.name)
    pool = futures.ThreadPoolExecutor(max_workers=2)
    app = grpc.server(pool)
    bindings.add_GreeterServicer_to_server(Echo(), app)
    credentials = grpc.ssl_server_credentials(
        [(args.key.read_bytes(), args.cert.read_bytes())],
        root_certificates=args.ca.read_bytes(), require_client_auth=True)
    if not app.add_secure_port(args.address, credentials):
        raise RuntimeError('TLS listener failed')
    app.start()
    records.write({'event': 'ready', 'provider': 'drone1', 'security': 'mutual TLS'})
    print('SPEC203_GRPC_PROVIDER_READY', flush=True)
    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    stop.wait()
    app.stop(7).wait()
    pool.shutdown(wait=True)
    records.write({'event': 'shutdown'})


def client(args, records):
    credentials = grpc.ssl_channel_credentials(
        args.ca.read_bytes(), args.key.read_bytes(), args.cert.read_bytes())
    with grpc.secure_channel(args.address, credentials, options=[
            ('grpc.ssl_target_name_override', args.server_name),
            ('grpc.default_authority', args.server_name)]) as channel:
        grpc.channel_ready_future(channel).result(timeout=10)
        stub = bindings.GreeterStub(channel)
        ready_ns = time.monotonic_ns()
        t0 = ready_ns + args.warmup_ms * 1000000
        t1 = t0 + args.window_ms * 1000000
        records.write({'event': 'ready', 'ready_ns': ready_ns, 't0_ns': t0, 't1_ns': t1,
                       'rate_rps': args.rate_rps})
        print('SPEC203_GRPC_READY', flush=True)
        sequence = 0
        pending = {}
        completed = threading.Condition()
        errors = []

        def record_reply(call, row):
            try:
                reply = call.result()
                metadata = dict(call.initial_metadata())
                if reply.message != row['command'] or metadata.get('provider-id') != 'drone1':
                    raise RuntimeError('gRPC response payload/target mismatch')
                terminal = time.monotonic_ns()
                row['terminal'] = 'success' if terminal < row['deadline_ns'] else 'timeout'
                row['payload_verified'] = True
            except grpc.RpcError as error:
                terminal = time.monotonic_ns()
                row['terminal'] = 'timeout' if error.code() == grpc.StatusCode.DEADLINE_EXCEEDED else 'response_error'
                row['error'] = str(error.code())
            row['terminal_ns'] = terminal
            records.write(row)

        def finished(call, request_id, row):
            try:
                record_reply(call, row)
            except Exception as error:
                with completed:
                    if not errors:
                        errors.append(error)
            finally:
                with completed:
                    del pending[request_id]
                    completed.notify_all()

        try:
            while time.monotonic_ns() < t1:
                planned = (ready_ns + sequence * 1000000000 // args.rate_rps
                           if args.rate_rps else time.monotonic_ns())
                if planned >= t1:
                    break
                if args.rate_rps:
                    time.sleep(max(0, planned - time.monotonic_ns()) / 1000000000)
                command = (args.run_id + ':' + str(sequence + 1)).ljust(64, '_')
                if len(command.encode()) != 64:
                    raise ValueError('command must be 64 bytes')
                begin = time.monotonic_ns()
                if begin >= t1:
                    break
                sequence += 1
                row = {'event': 'terminal', 'command': command, 'begin_ns': begin,
                       'planned_ns': planned, 'deadline_ns': begin + 5000000000,
                       'measured': begin >= t0, 'target': 'drone1',
                       'payload_verified': False}
                with completed:
                    if errors:
                        raise errors[0]
                    if len(pending) >= 512:
                        raise RuntimeError('fixed-rate client exceeded 512 pending calls')
                call = stub.SayHello.future(messages.HelloRequest(name=command), timeout=5)
                if not args.rate_rps:
                    record_reply(call, row)
                    continue
                with completed:
                    pending[sequence] = call
                call.add_done_callback(
                    lambda result, request_id=sequence, record=row:
                    finished(result, request_id, record))
        finally:
            # Each RPC has a five-second deadline; drain callbacks before closing records.
            with completed:
                if not completed.wait_for(lambda: not pending, timeout=6):
                    raise RuntimeError('fixed-rate gRPC callbacks did not drain')
        if errors:
            raise errors[0]
        time.sleep(2)
        records.write({'event': 'shutdown', 'attempts': sequence})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('role', choices=['server', 'client'])
    for name in ['key', 'cert', 'ca', 'records']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--address', required=True)
    parser.add_argument('--run-id', default='grpc')
    parser.add_argument('--warmup-ms', type=int, default=10000)
    parser.add_argument('--window-ms', type=int, default=60000)
    parser.add_argument('--rate-rps', type=int, default=0,
                        help='Fixed offered requests per second; zero keeps serial calls')
    parser.add_argument('--bad-echo', action='store_true')
    parser.add_argument('--handler-ms', type=int, default=5)
    parser.add_argument('--server-name', default='drone1')
    args = parser.parse_args()
    if args.warmup_ms < 0 or args.window_ms <= 0 or args.handler_ms < 0 or args.rate_rps < 0:
        raise ValueError('invalid window')
    records = Records(args.records)
    try:
        (server if args.role == 'server' else client)(args, records)
    finally:
        records.file.close()


if __name__ == '__main__':
    main()
