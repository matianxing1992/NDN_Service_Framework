import grpc
from concurrent import futures
import argparse
from bisect import bisect_right
from collections import Counter
import json
from pathlib import Path
import time
import random
import threading
import helloworld_pb2
import helloworld_pb2_grpc


def load_busy_trace(path, provider_id):
    payload = Path(path).read_bytes()
    if not payload or len(payload) > 65536:
        raise ValueError("busy trace size")

    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate busy trace key")
            result[key] = value
        return result

    trace = json.loads(payload, object_pairs_hook=unique_object)
    identity = {"drone1": "/example/hello/provider/A",
                "drone2": "/example/hello/provider/B"}.get(provider_id)
    if (identity is None or type(trace) is not dict or
            set(trace) != {"version", "producer", "window_ms", "tail", "changes"} or
            type(trace["version"]) is not int or trace["version"] != 1 or
            trace["producer"] != identity or trace["tail"] != "hold" or
            type(trace["window_ms"]) is not int or
            not 1 <= trace["window_ms"] <= 60000 or
            type(trace["changes"]) is not list or
            not 1 <= len(trace["changes"]) <= 256):
        raise ValueError("busy trace schema")
    changes = []
    for row in trace["changes"]:
        if (type(row) is not dict or set(row) != {"offset_ms", "available"} or
                type(row["offset_ms"]) is not int or
                type(row["available"]) is not bool or
                not 0 <= row["offset_ms"] < trace["window_ms"] or
                (changes and (row["offset_ms"] <= changes[-1][0] or
                              row["available"] == changes[-1][1])) or
                (not changes and row["offset_ms"] != 0)):
            raise ValueError("busy trace row")
        changes.append((row["offset_ms"], row["available"]))
    return changes


class Greeter(helloworld_pb2_grpc.GreeterServicer):
    def __init__(self, delay_ms=0, quiet=False, failure_probability=0.0,
                 epoch_ms=10000, seed=100, provider_id="provider",
                 service_error_code=None, busy_changes=None, origin_path=None,
                 records_path=None, task_busy=False,
                 max_attempt_deadline_ms=2000):
        self.delay_s = max(0, delay_ms) / 1000.0
        self.quiet = quiet
        self.failure_probability = max(0.0, min(1.0, failure_probability))
        self.epoch_s = max(0.001, epoch_ms / 1000.0)
        self.seed = seed
        self.provider_id = provider_id
        self.service_error_code = service_error_code
        self.busy_changes = busy_changes or []
        self.origin_path = origin_path
        self.task_busy = task_busy
        if max_attempt_deadline_ms <= 0:
            raise ValueError("max attempt deadline must be positive")
        self.max_attempt_deadline_ns = max_attempt_deadline_ms * 1000000
        self.started_at = time.monotonic()
        self._counter_lock = threading.Lock()
        self._handler_executions = 0
        self._health_checks = 0
        self._health_success = 0
        self._stats_epoch = 0
        self._stats_reset_monotonic_s = self.started_at
        self._next_service_event_id = 0
        self._next_health_event_id = 0
        self._service_events = {}
        self._health_events = {}
        self._records = Path(records_path).open("w") if records_path else None

    def close(self):
        if self._records is not None:
            self._records.close()
            self._records = None

    def unavailable(self):
        if self.failure_probability <= 0:
            return False
        epoch = int((time.monotonic() - self.started_at) / self.epoch_s)
        rng = random.Random((self.seed << 16) ^ epoch ^ 0x9e3779b9)
        return rng.random() < self.failure_probability

    def busy(self, at_ns=None):
        if not self.busy_changes:
            return False
        try:
            origin = int(Path(self.origin_path).read_text().strip())
        except (OSError, ValueError) as error:
            raise RuntimeError("busy trace origin missing or invalid") from error
        elapsed_ms = ((time.monotonic_ns() if at_ns is None else at_ns) - origin) // 1000000
        if origin <= 0 or elapsed_ms < 0:
            return False
        index = bisect_right(self.busy_changes, (elapsed_ms, True)) - 1
        return index >= 0 and not self.busy_changes[index][1]

    def busy_for_task(self, command, at_ns=None):
        if not self.task_busy:
            return self.busy(at_ns)
        if (len(command) != 64 or command[:5] != "T205|" or
                command[5] not in "WM" or command[6] != "|" or
                command[7] not in "CM"):
            raise ValueError("task-busy request must carry a measured task")
        if command[5] == "W":
            return False
        specialty = "C" if self.provider_id == "drone1" else "M"
        return self.busy(at_ns) and command[7] == specialty

    def counters(self):
        with self._counter_lock:
            return {
                "handler_executions": self._handler_executions,
                "health_checks": self._health_checks,
                "health_success": self._health_success,
                "stats_epoch": self._stats_epoch,
            }

    @staticmethod
    def _invocation_metadata(context):
        return {item.key: item.value for item in context.invocation_metadata()}

    def _attempt_rejection(self, context):
        deadline = self._invocation_metadata(context).get("x-ndnsf-attempt-expires-ns")
        if deadline is not None:
            if not deadline.isdecimal() or len(deadline) > 20:
                return grpc.StatusCode.INVALID_ARGUMENT, "invalid attempt deadline"
            now_ns = time.monotonic_ns()
            if (int(deadline) <= now_ns or
                    int(deadline) - now_ns > self.max_attempt_deadline_ns):
                return grpc.StatusCode.DEADLINE_EXCEEDED, "attempt no longer eligible"
        if not context.is_active():
            return grpc.StatusCode.DEADLINE_EXCEEDED, "cancelled attempt"
        return None

    def _start_handler_execution(self, request, context):
        metadata = self._invocation_metadata(context)
        request_id = metadata.get("x-ndnsf-logical-request-id", request.name)
        attempt_text = metadata.get("x-ndnsf-attempt", "0")
        try:
            attempt = int(attempt_text)
        except ValueError:
            attempt = 0
        with self._counter_lock:
            self._handler_executions += 1
            handler_executions = self._handler_executions
            event_id = self._next_service_event_id
            self._next_service_event_id += 1
            epoch = self._stats_epoch
            self._service_events[event_id] = {
                "event_id": event_id,
                "stats_epoch": epoch,
                "request_id": request_id,
                "request_name": request.name,
                "attempt": attempt,
                "started_monotonic_s": time.monotonic(),
                "completed_monotonic_s": None,
                "status": "IN_PROGRESS",
            }
            return epoch, event_id, handler_executions, request_id

    def _finish_handler_execution(self, epoch, event_id, status):
        with self._counter_lock:
            event = self._service_events.get(event_id)
            if event is None or event["stats_epoch"] != epoch:
                return
            event["completed_monotonic_s"] = time.monotonic()
            event["status"] = status
            if self._records is not None:
                self._records.write(json.dumps({"event": "service", "provider": self.provider_id,
                                                **event}, sort_keys=True) + "\n")
                self._records.flush()

    def _start_health_check(self):
        with self._counter_lock:
            self._health_checks += 1
            event_id = self._next_health_event_id
            self._next_health_event_id += 1
            epoch = self._stats_epoch
            self._health_events[event_id] = {
                "event_id": event_id,
                "stats_epoch": epoch,
                "started_monotonic_s": time.monotonic(),
                "completed_monotonic_s": None,
                "status": "IN_PROGRESS",
            }
            return epoch, event_id

    def _finish_health_check(self, epoch, event_id, success, status):
        with self._counter_lock:
            event = self._health_events.get(event_id)
            if event is None or event["stats_epoch"] != epoch:
                return
            if success:
                self._health_success += 1
            event["completed_monotonic_s"] = time.monotonic()
            event["status"] = status
            if self._records is not None:
                self._records.write(json.dumps({"event": "health", "provider": self.provider_id,
                                                **event}, sort_keys=True) + "\n")
                self._records.flush()

    def stats_snapshot(self):
        with self._counter_lock:
            service_events = [dict(event) for _, event in sorted(
                self._service_events.items())]
            health_events = [dict(event) for _, event in sorted(
                self._health_events.items())]
            request_id_counts = Counter(
                event["request_id"] for event in service_events)
            service_status_counts = Counter(
                event["status"] for event in service_events)
            health_status_counts = Counter(
                event["status"] for event in health_events)
            return {
                "schema": "ndnsf.grpc.baseline.provider-stats.v1",
                "provider_id": self.provider_id,
                "stats_epoch": self._stats_epoch,
                "stats_reset_monotonic_s": self._stats_reset_monotonic_s,
                "snapshot_monotonic_s": time.monotonic(),
                "handler_executions": self._handler_executions,
                "health_checks": self._health_checks,
                "health_success": self._health_success,
                "request_id_counts": dict(sorted(request_id_counts.items())),
                "service_status_counts": dict(sorted(
                    service_status_counts.items())),
                "health_status_counts": dict(sorted(
                    health_status_counts.items())),
                "service_events": service_events,
                "health_events": health_events,
            }

    def reset_stats(self):
        with self._counter_lock:
            self._stats_epoch += 1
            self._stats_reset_monotonic_s = time.monotonic()
            self._handler_executions = 0
            self._health_checks = 0
            self._health_success = 0
            self._next_service_event_id = 0
            self._next_health_event_id = 0
            self._service_events.clear()
            self._health_events.clear()
        return self.stats_snapshot()

    def _trailing_metadata(self, handler_executions, request_id):
        return (
            ("x-ndnsf-provider-id", self.provider_id),
            ("x-ndnsf-handler-executions", str(handler_executions)),
            ("x-ndnsf-logical-request-id", request_id),
        )

    def SayHello(self, request, context):
        epoch, event_id, handler_executions, request_id = (
            self._start_handler_execution(request, context))
        context.set_trailing_metadata(
            self._trailing_metadata(handler_executions, request_id))
        if not self.quiet:
            print(f"Received request: {request.name} provider={self.provider_id}",
                  flush=True)
        status = "UNKNOWN"
        try:
            rejection = self._attempt_rejection(context)
            if rejection is not None:
                status = rejection[0].name
                context.abort(*rejection)
            if self.service_error_code is not None:
                status = self.service_error_code.name
                context.abort(self.service_error_code, "injected service error")
            if self.unavailable():
                status = grpc.StatusCode.UNAVAILABLE.name
                context.abort(
                    grpc.StatusCode.UNAVAILABLE,
                    "intermittent provider unavailable")
            admission_ns = time.monotonic_ns()
            if self.task_busy:
                with self._counter_lock:
                    event = self._service_events.get(event_id)
                    if event is not None and event["stats_epoch"] == epoch:
                        event["admission_monotonic_ns"] = admission_ns
            if self.busy_for_task(request.name, admission_ns):
                status = grpc.StatusCode.RESOURCE_EXHAUSTED.name
                context.abort(grpc.StatusCode.RESOURCE_EXHAUSTED,
                              "provider online but busy")
            if self.delay_s > 0:
                time.sleep(self.delay_s)
            rejection = self._attempt_rejection(context)
            if rejection is not None:
                status = rejection[0].name
                context.abort(*rejection)
            status = "OK"
            return helloworld_pb2.HelloReply(
                message='Hello, {}'.format(request.name))
        finally:
            self._finish_handler_execution(epoch, event_id, status)

    def Health(self, request, context):
        """Application-level readiness probe; intentionally bypasses service delay."""
        epoch, event_id = self._start_health_check()
        success = False
        status = "UNKNOWN"
        try:
            if self.unavailable():
                status = grpc.StatusCode.UNAVAILABLE.name
                context.set_trailing_metadata(
                    (("x-ndnsf-provider-id", self.provider_id),))
                context.abort(
                    grpc.StatusCode.UNAVAILABLE, "provider is not serving")
            if not self.task_busy and self.busy():
                status = grpc.StatusCode.RESOURCE_EXHAUSTED.name
                context.set_trailing_metadata(
                    (("x-ndnsf-provider-id", self.provider_id),))
                context.abort(grpc.StatusCode.RESOURCE_EXHAUSTED,
                              "provider online but busy")
            success = True
            status = "SERVING"
            counters = self.counters()
            return helloworld_pb2.HelloReply(
                message=(f"provider_id={self.provider_id} status=SERVING "
                         f"handler_executions="
                         f"{counters['handler_executions']}"))
        finally:
            self._finish_health_check(
                epoch, event_id, success=success, status=status)

    def Stats(self, request, context):
        return helloworld_pb2.HelloReply(
            message=json.dumps(
                self.stats_snapshot(), sort_keys=True, separators=(",", ":")))

    def ResetStats(self, request, context):
        return helloworld_pb2.HelloReply(
            message=json.dumps(
                self.reset_stats(), sort_keys=True, separators=(",", ":")))


def add_baseline_control_to_server(servicer, server):
    """Register lightweight control RPCs without grpcio-health-checking."""
    rpc_method_handlers = {
        "Health": grpc.unary_unary_rpc_method_handler(
            servicer.Health,
            request_deserializer=helloworld_pb2.HelloRequest.FromString,
            response_serializer=helloworld_pb2.HelloReply.SerializeToString,
        ),
        "Stats": grpc.unary_unary_rpc_method_handler(
            servicer.Stats,
            request_deserializer=helloworld_pb2.HelloRequest.FromString,
            response_serializer=helloworld_pb2.HelloReply.SerializeToString,
        ),
        "ResetStats": grpc.unary_unary_rpc_method_handler(
            servicer.ResetStats,
            request_deserializer=helloworld_pb2.HelloRequest.FromString,
            response_serializer=helloworld_pb2.HelloReply.SerializeToString,
        ),
    }
    generic_handler = grpc.method_handlers_generic_handler(
        "NDNSFBaseline", rpc_method_handlers)
    server.add_generic_rpc_handlers((generic_handler,))


def build_server(bind="0.0.0.0:50051", delay_ms=0, workers=32, quiet=False,
                 failure_probability=0.0, epoch_ms=10000, seed=100,
                 provider_id="provider", service_error_code=None,
                 busy_trace=None, origin_path=None,
                 tls_ca=None, tls_cert=None, tls_key=None, records_path=None,
                 task_busy=False, max_attempt_deadline_ms=2000):
    """Build but do not start a server; exposed for focused local tests."""
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=workers))
    if (busy_trace is None) != (origin_path is None):
        raise ValueError("busy trace requires origin and path")
    if any(value is not None for value in (tls_ca, tls_cert, tls_key)) and not all(
            value is not None for value in (tls_ca, tls_cert, tls_key)):
        raise ValueError("mutual TLS requires CA, certificate and key")
    if busy_trace is not None and failure_probability != 0:
        raise ValueError("busy trace requires reachable provider")
    if task_busy and (busy_trace is None or provider_id not in ("drone1", "drone2")):
        raise ValueError("task-busy requires a matched Provider trace")
    if busy_trace is not None:
        try:
            int(Path(origin_path).read_text().strip())
        except (OSError, ValueError) as error:
            raise ValueError("busy trace origin missing or invalid") from error
    busy_changes = load_busy_trace(busy_trace, provider_id) if busy_trace else None
    greeter = Greeter(
        delay_ms, quiet, failure_probability, epoch_ms, seed, provider_id,
        service_error_code, busy_changes, origin_path, records_path, task_busy,
        max_attempt_deadline_ms)
    helloworld_pb2_grpc.add_GreeterServicer_to_server(greeter, server)
    add_baseline_control_to_server(greeter, server)
    if tls_ca is None:
        bound_port = server.add_insecure_port(bind)
    else:
        credentials = grpc.ssl_server_credentials(
            ((Path(tls_key).read_bytes(), Path(tls_cert).read_bytes()),),
            root_certificates=Path(tls_ca).read_bytes(), require_client_auth=True)
        bound_port = server.add_secure_port(bind, credentials)
    if bound_port == 0:
        raise RuntimeError(f"unable to bind gRPC server to {bind}")
    return server, greeter, bound_port


def serve():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0:50051")
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--workers", type=int, default=32)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--failure-probability", type=float, default=0.0)
    parser.add_argument("--epoch-ms", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=100)
    parser.add_argument("--provider-id", default="provider")
    parser.add_argument("--busy-trace")
    parser.add_argument("--task-busy", action="store_true")
    parser.add_argument("--origin")
    parser.add_argument("--tls-ca")
    parser.add_argument("--tls-cert")
    parser.add_argument("--tls-key")
    parser.add_argument("--records", help="provider-owned JSONL handler records")
    parser.add_argument("--max-attempt-deadline-ms", type=int, default=2000)
    args = parser.parse_args()

    # 读取证书文件
    # with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/server.key', 'rb') as f:
    #     server_key = f.read()
    # with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/server.pem', 'rb') as f:
    #     server_cert = f.read()
    # with open('/home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/ca.pem', 'rb') as f:
    #     trusted_certs = f.read()

    # 创建 SSL 证书对象
    # server_credentials = grpc.ssl_server_credentials(
    #     ((server_key, server_cert),),
    #     root_certificates=trusted_certs,
    #     require_client_auth=True
    # )

    server, greeter, bound_port = build_server(
        bind=args.bind,
        delay_ms=args.delay_ms,
        workers=args.workers,
        quiet=args.quiet,
        failure_probability=args.failure_probability,
        epoch_ms=args.epoch_ms,
        seed=args.seed,
        provider_id=args.provider_id,
        busy_trace=args.busy_trace,
        origin_path=args.origin,
        tls_ca=args.tls_ca,
        tls_cert=args.tls_cert,
        tls_key=args.tls_key,
        records_path=args.records,
        task_busy=args.task_busy,
        max_attempt_deadline_ms=args.max_attempt_deadline_ms,
    )
    server.start()
    print(f"GRPC_SERVER_READY bind={args.bind} delay_ms={args.delay_ms} "
          f"failure_probability={args.failure_probability} "
          f"provider_id={args.provider_id} bound_port={bound_port}", flush=True)

    try:
        while True:
            time.sleep(86400)  # 让服务器持续运行
    except KeyboardInterrupt:
        server.stop(0)

    counters = greeter.counters()
    print(f"GRPC_SERVER_FINAL provider_id={args.provider_id} "
          f"handler_executions={counters['handler_executions']} "
          f"health_checks={counters['health_checks']} "
          f"health_success={counters['health_success']}", flush=True)
    greeter.close()

if __name__ == '__main__':
    serve()
