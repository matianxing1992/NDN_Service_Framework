import grpc
from concurrent import futures
import argparse
from collections import Counter
import json
import time
import random
import threading
import helloworld_pb2
import helloworld_pb2_grpc


class Greeter(helloworld_pb2_grpc.GreeterServicer):
    def __init__(self, delay_ms=0, quiet=False, failure_probability=0.0,
                 epoch_ms=10000, seed=100, provider_id="provider",
                 service_error_code=None):
        self.delay_s = max(0, delay_ms) / 1000.0
        self.quiet = quiet
        self.failure_probability = max(0.0, min(1.0, failure_probability))
        self.epoch_s = max(0.001, epoch_ms / 1000.0)
        self.seed = seed
        self.provider_id = provider_id
        self.service_error_code = service_error_code
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

    def unavailable(self):
        if self.failure_probability <= 0:
            return False
        epoch = int((time.monotonic() - self.started_at) / self.epoch_s)
        rng = random.Random((self.seed << 16) ^ epoch ^ 0x9e3779b9)
        return rng.random() < self.failure_probability

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
            if self.service_error_code is not None:
                status = self.service_error_code.name
                context.abort(self.service_error_code, "injected service error")
            if self.unavailable():
                status = grpc.StatusCode.UNAVAILABLE.name
                context.abort(
                    grpc.StatusCode.UNAVAILABLE,
                    "intermittent provider unavailable")
            if self.delay_s > 0:
                time.sleep(self.delay_s)
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
                 provider_id="provider", service_error_code=None):
    """Build but do not start a server; exposed for focused local tests."""
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=workers))
    greeter = Greeter(
        delay_ms, quiet, failure_probability, epoch_ms, seed, provider_id,
        service_error_code)
    helloworld_pb2_grpc.add_GreeterServicer_to_server(greeter, server)
    add_baseline_control_to_server(greeter, server)
    bound_port = server.add_insecure_port(bind)
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

if __name__ == '__main__':
    serve()
