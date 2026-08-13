"""Sequential and parallel multi-provider gRPC baselines with optional health probes.

The client deliberately avoids gRPC retry, hedging, and load-balancing policy.
Every logical HELLO request has one global deadline and at most one explicit
attempt per endpoint. Only UNAVAILABLE and DEADLINE_EXCEEDED are retryable.
The strict sequential mode disables proactive health routing; the health-
assisted mode is retained as a separately labelled diagnostic. The optional
parallel mode is a separately labelled first-success fan-out diagnostic: it
issues one RPC to every configured endpoint for each logical request and keeps
the first successful response. It is not used by the strict baseline.
"""

from __future__ import annotations

import argparse
import asyncio
from collections import Counter
from dataclasses import dataclass, field
import json
import math
import time
from typing import Any, Iterable, Sequence

import grpc

import helloworld_pb2
import helloworld_pb2_grpc


RETRYABLE_STATUS_CODES = frozenset({
    grpc.StatusCode.UNAVAILABLE,
    grpc.StatusCode.DEADLINE_EXCEEDED,
})

CHANNEL_OPTIONS = (
    ("grpc.enable_retries", 0),
    ("grpc.max_concurrent_streams", 1024),
    ("grpc.keepalive_time_ms", 10000),
    ("grpc.keepalive_timeout_ms", 5000),
    ("grpc.http2.max_pings_without_data", 0),
)


@dataclass(frozen=True)
class TargetSpec:
    provider_id: str
    address: str


@dataclass
class Endpoint:
    provider_id: str
    address: str
    channel: Any
    say_hello: Any
    health: Any
    stats: Any
    channel_ready: bool = False
    healthy: bool = False
    last_checked_at: float = 0.0
    last_success_at: float = 0.0
    last_status: str = "UNKNOWN"

    def is_fresh_healthy(self, now: float, stale_s: float) -> bool:
        return self.healthy and now - self.last_success_at <= stale_s


@dataclass
class RequestOutcome:
    request_id: int
    success: bool
    provider_id: str | None
    status: str
    latency_ms: float
    response_message: str = ""


@dataclass
class Metrics:
    sent: int = 0
    success: int = 0
    failures: int = 0
    attempts: int = 0
    failovers: int = 0
    health_directed_selections: int = 0
    handler_executions: int = 0
    parallel_issued: int = 0
    parallel_winners: int = 0
    parallel_cancellations: int = 0
    latencies_ms: list[float] = field(default_factory=list)
    status_counts: Counter[str] = field(default_factory=Counter)


def parse_target_specs(
    values: Sequence[str], *, allow_single_provider: bool = False,
) -> list[TargetSpec]:
    if len(values) < 3 and not (allow_single_provider and len(values) == 1):
        # Keep the historical wording for the three-provider contract tests,
        # while allowing the four-provider mobility profile.
        raise ValueError(
            "the mobility baseline requires exactly three or more --target values")
    result: list[TargetSpec] = []
    provider_ids: set[str] = set()
    addresses: set[str] = set()
    for value in values:
        provider_id, separator, address = value.partition("=")
        provider_id = provider_id.strip()
        address = address.strip()
        if separator == "" or not provider_id or not address:
            raise ValueError(
                f"invalid target {value!r}; expected PROVIDER_ID=HOST:PORT")
        if provider_id in provider_ids:
            raise ValueError(f"duplicate provider id: {provider_id}")
        if address in addresses:
            raise ValueError(f"duplicate endpoint address: {address}")
        provider_ids.add(provider_id)
        addresses.add(address)
        result.append(TargetSpec(provider_id, address))
    return result


def rotated(items: Sequence[Any], logical_request_id: int) -> list[Any]:
    if not items:
        return []
    offset = logical_request_id % len(items)
    return list(items[offset:]) + list(items[:offset])


def is_retryable(code: grpc.StatusCode) -> bool:
    return code in RETRYABLE_STATUS_CODES


def percentile_nearest_rank(values: Sequence[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = max(1, math.ceil(percentile * len(ordered)))
    return ordered[min(rank - 1, len(ordered) - 1)]


def count_health_events(
    events: Sequence[tuple[float, bool]], start_s: float, end_s: float,
) -> tuple[int, int]:
    selected = [event for event in events if start_s <= event[0] < end_s]
    return len(selected), sum(1 for _, success in selected if success)


def metadata_value(metadata: Iterable[Any] | None, key: str) -> str | None:
    if metadata is None:
        return None
    for item in metadata:
        item_key = getattr(item, "key", None)
        item_value = getattr(item, "value", None)
        if item_key is None and isinstance(item, tuple) and len(item) == 2:
            item_key, item_value = item
        if item_key == key:
            if isinstance(item_value, bytes):
                return item_value.decode("utf-8", errors="replace")
            return str(item_value)
    return None


class ThreeProviderFailoverClient:
    def __init__(
        self,
        targets: Sequence[TargetSpec],
        *,
        global_deadline_s: float = 5.0,
        attempt_timeout_s: float = 0.2,
        health_interval_s: float = 0.2,
        health_timeout_s: float = 0.2,
        health_stale_s: float = 0.4,
        prewarm_timeout_s: float = 2.0,
        parallel: bool = False,
        quiet: bool = False,
        allow_single_provider: bool = False,
    ) -> None:
        if len(targets) < 3 and not (allow_single_provider and len(targets) == 1):
            raise ValueError(
                "exactly three or more independently addressed providers are required")
        if allow_single_provider and len(targets) != 1:
            raise ValueError("single-provider mode requires exactly one target")
        for name, value in (
            ("global_deadline_s", global_deadline_s),
            ("attempt_timeout_s", attempt_timeout_s),
            ("health_interval_s", health_interval_s),
            ("health_timeout_s", health_timeout_s),
            ("health_stale_s", health_stale_s),
            ("prewarm_timeout_s", prewarm_timeout_s),
        ):
            if value <= 0:
                raise ValueError(f"{name} must be positive")
        self.global_deadline_s = global_deadline_s
        self.attempt_timeout_s = attempt_timeout_s
        self.health_interval_s = health_interval_s
        self.health_timeout_s = health_timeout_s
        self.health_stale_s = health_stale_s
        self.prewarm_timeout_s = prewarm_timeout_s
        self.parallel = bool(parallel)
        self.allow_single_provider = bool(allow_single_provider)
        self.quiet = quiet
        self.endpoints = [self._make_endpoint(target) for target in targets]
        self.health_checks = 0
        self.health_success = 0
        self.health_events: list[tuple[float, bool]] = []
        self._health_tasks: list[asyncio.Task[Any]] = []
        self._closing = False
        self._closed = False

    @staticmethod
    def _make_endpoint(target: TargetSpec) -> Endpoint:
        channel = grpc.aio.insecure_channel(target.address, options=CHANNEL_OPTIONS)
        stub = helloworld_pb2_grpc.GreeterStub(channel)
        health = channel.unary_unary(
            "/NDNSFBaseline/Health",
            request_serializer=helloworld_pb2.HelloRequest.SerializeToString,
            response_deserializer=helloworld_pb2.HelloReply.FromString,
        )
        stats = channel.unary_unary(
            "/NDNSFBaseline/Stats",
            request_serializer=helloworld_pb2.HelloRequest.SerializeToString,
            response_deserializer=helloworld_pb2.HelloReply.FromString,
        )
        return Endpoint(
            provider_id=target.provider_id,
            address=target.address,
            channel=channel,
            say_hello=stub.SayHello,
            health=health,
            stats=stats,
        )

    async def prewarm_all(self) -> None:
        async def prewarm(endpoint: Endpoint) -> None:
            try:
                await asyncio.wait_for(
                    endpoint.channel.channel_ready(), self.prewarm_timeout_s)
                endpoint.channel_ready = True
            except (asyncio.TimeoutError, grpc.RpcError):
                endpoint.channel_ready = False
            print(
                f"GRPC_PREWARM provider={endpoint.provider_id} "
                f"target={endpoint.address} ready={int(endpoint.channel_ready)}",
                flush=True,
            )

        await asyncio.gather(*(prewarm(endpoint) for endpoint in self.endpoints))

    async def probe(self, endpoint: Endpoint) -> bool:
        before = endpoint.last_status
        checked_at = time.monotonic()
        try:
            response = await endpoint.health(
                helloworld_pb2.HelloRequest(name="active-health-probe"),
                timeout=self.health_timeout_s,
            )
            success = "status=SERVING" in response.message
            status = "SERVING" if success else "INVALID_RESPONSE"
        except grpc.aio.AioRpcError as error:
            success = False
            status = error.code().name
        except asyncio.TimeoutError:
            success = False
            status = "DEADLINE_EXCEEDED"
        # Count completed probe request/status pairs together. This makes the
        # measurement snapshot deterministic even when a probe straddles the
        # warm-up boundary.
        self.health_checks += 1
        self.health_events.append((checked_at, success))
        endpoint.last_checked_at = checked_at
        endpoint.healthy = success
        endpoint.last_status = status
        if success:
            endpoint.last_success_at = checked_at
            self.health_success += 1
        if before != status or not self.quiet:
            print(
                f"GRPC_HEALTH_STATUS provider={endpoint.provider_id} "
                f"status={status} healthy={int(success)} "
                f"observed_at_ms={checked_at * 1000.0:.3f}",
                flush=True,
            )
        return success

    async def probe_all(self) -> list[bool]:
        return list(await asyncio.gather(
            *(self.probe(endpoint) for endpoint in self.endpoints)))

    async def _health_loop(self, endpoint: Endpoint) -> None:
        while not self._closing:
            started = time.monotonic()
            await self.probe(endpoint)
            sleep_s = self.health_interval_s - (time.monotonic() - started)
            if sleep_s > 0:
                await asyncio.sleep(sleep_s)

    def start_health_monitoring(self) -> None:
        if self._closing or self._closed:
            raise RuntimeError("cannot start health monitoring after client close")
        if self._health_tasks:
            return
        self._health_tasks = [
            asyncio.create_task(
                self._health_loop(endpoint),
                name=f"grpc-health-{endpoint.provider_id}",
            )
            for endpoint in self.endpoints
        ]

    async def close(self) -> None:
        if self._closed:
            return
        self._closing = True
        for task in self._health_tasks:
            task.cancel()
        if self._health_tasks:
            await asyncio.gather(*self._health_tasks, return_exceptions=True)
        self._health_tasks.clear()
        await asyncio.gather(
            *(endpoint.channel.close() for endpoint in self.endpoints))
        self._closed = True

    async def stats_snapshot_all(self, timeout_s: float = 2.0) -> dict[str, Any]:
        """Fetch an exact server-owned counter/event snapshot from all Providers."""
        if timeout_s <= 0:
            raise ValueError("stats timeout must be positive")

        async def fetch(endpoint: Endpoint) -> tuple[str, dict[str, Any]]:
            response = await endpoint.stats(
                helloworld_pb2.HelloRequest(name="snapshot"), timeout=timeout_s)
            try:
                snapshot = json.loads(response.message)
            except json.JSONDecodeError as error:
                raise ValueError(
                    f"invalid Stats JSON from {endpoint.provider_id}: {error}") from error
            if snapshot.get("provider_id") != endpoint.provider_id:
                raise ValueError(
                    f"Stats provider mismatch for {endpoint.provider_id}: "
                    f"{snapshot.get('provider_id')!r}")
            return endpoint.provider_id, snapshot

        results = await asyncio.gather(*(fetch(endpoint)
                                         for endpoint in self.endpoints))
        return {
            "schema": "ndnsf.grpc.baseline.aggregate-stats.v1",
            "snapshot_monotonic_s": time.monotonic(),
            "providers": {provider_id: snapshot
                          for provider_id, snapshot in results},
        }

    async def reset_stats_all(self, timeout_s: float = 2.0) -> dict[str, Any]:
        """Reset all server counters and return their post-reset snapshots."""
        if timeout_s <= 0:
            raise ValueError("stats timeout must be positive")

        async def reset(endpoint: Endpoint) -> tuple[str, dict[str, Any]]:
            reset_rpc = endpoint.channel.unary_unary(
                "/NDNSFBaseline/ResetStats",
                request_serializer=helloworld_pb2.HelloRequest.SerializeToString,
                response_deserializer=helloworld_pb2.HelloReply.FromString,
            )
            response = await reset_rpc(
                helloworld_pb2.HelloRequest(name="reset"), timeout=timeout_s)
            snapshot = json.loads(response.message)
            if snapshot.get("provider_id") != endpoint.provider_id:
                raise ValueError(
                    f"ResetStats provider mismatch for {endpoint.provider_id}")
            return endpoint.provider_id, snapshot

        results = await asyncio.gather(*(reset(endpoint)
                                         for endpoint in self.endpoints))
        return {
            "schema": "ndnsf.grpc.baseline.aggregate-stats.v1",
            "snapshot_monotonic_s": time.monotonic(),
            "providers": {provider_id: snapshot
                          for provider_id, snapshot in results},
        }

    async def _trailing_metadata(self, call: Any) -> Any:
        try:
            return await call.trailing_metadata()
        except (grpc.RpcError, asyncio.CancelledError):
            return None

    async def execute_request(
        self,
        logical_request_id: int,
        metrics: Metrics,
        *,
        measured: bool = True,
    ) -> RequestOutcome:
        if getattr(self, "parallel", False):
            return await self.execute_request_parallel(
                logical_request_id, metrics, measured=measured)
        request_started = time.monotonic()
        request_deadline = request_started + self.global_deadline_s
        base_order = rotated(self.endpoints, logical_request_id)
        now = time.monotonic()
        fresh = [
            endpoint for endpoint in base_order
            if endpoint.is_fresh_healthy(now, self.health_stale_s)
        ]
        stale = [endpoint for endpoint in base_order if endpoint not in fresh]
        attempt_order = fresh + stale if fresh else base_order
        preferred = base_order[0]
        if measured:
            metrics.sent += 1
        if attempt_order[0] is not preferred and measured:
            metrics.health_directed_selections += 1
            metrics.status_counts["HEALTH_DIRECTED_SELECTION"] += 1
            print(
                f"GRPC_FAILOVER_STATUS request_id={logical_request_id} "
                f"from={preferred.provider_id} to={attempt_order[0].provider_id} "
                "status=HEALTH_DIRECTED_SELECTION",
                flush=True,
            )

        request = helloworld_pb2.HelloRequest(name=f"Test-{logical_request_id}")
        last_status = "NO_ATTEMPT"
        for attempt_number, endpoint in enumerate(attempt_order, start=1):
            remaining_s = request_deadline - time.monotonic()
            if remaining_s <= 0:
                last_status = "GLOBAL_DEADLINE_EXCEEDED"
                break
            # A failover is an issued service RPC after an earlier service RPC;
            # proactive health-based initial selection is counted separately.
            if attempt_number > 1 and measured:
                metrics.failovers += 1
            call_timeout_s = min(self.attempt_timeout_s, remaining_s)
            attempt_started = time.monotonic()
            if measured:
                metrics.attempts += 1
            call = endpoint.say_hello(
                request,
                timeout=call_timeout_s,
                metadata=(
                    ("x-ndnsf-logical-request-id", str(logical_request_id)),
                    ("x-ndnsf-attempt", str(attempt_number)),
                ),
            )
            try:
                response = await call
                attempt_latency_ms = (time.monotonic() - attempt_started) * 1000.0
                trailing = await self._trailing_metadata(call)
                # A unary gRPC call should normally either return a reply or
                # raise AioRpcError.  Some transports/server shutdown races
                # can nevertheless complete the await with ``None``.  Treat
                # that as a failed endpoint attempt so the explicit
                # sequential failover loop can continue; never dereference a
                # missing response and abort the whole measurement task.
                if response is None:
                    last_status = "NO_RESPONSE"
                    handler_observed = metadata_value(
                        trailing, "x-ndnsf-handler-executions") is not None
                    if measured:
                        metrics.status_counts[last_status] += 1
                        if handler_observed:
                            metrics.handler_executions += 1
                    print(
                        f"GRPC_FAILOVER_ATTEMPT request_id={logical_request_id} "
                        f"attempt={attempt_number} provider={endpoint.provider_id} "
                        f"status={last_status} latency_ms={attempt_latency_ms:.3f} "
                        f"handler_observed={int(handler_observed)}",
                        flush=True,
                    )
                    endpoint.healthy = False
                    endpoint.last_status = last_status
                    endpoint.last_checked_at = time.monotonic()
                    continue
                if measured:
                    metrics.handler_executions += 1
                    metrics.status_counts["OK"] += 1
                total_latency_ms = (time.monotonic() - request_started) * 1000.0
                if measured:
                    metrics.success += 1
                    metrics.latencies_ms.append(total_latency_ms)
                print(
                    f"GRPC_FAILOVER_ATTEMPT request_id={logical_request_id} "
                    f"attempt={attempt_number} provider={endpoint.provider_id} "
                    f"status=OK latency_ms={attempt_latency_ms:.3f} "
                    f"server_provider={metadata_value(trailing, 'x-ndnsf-provider-id') or endpoint.provider_id}",
                    flush=True,
                )
                return RequestOutcome(
                    logical_request_id,
                    True,
                    endpoint.provider_id,
                    "OK",
                    total_latency_ms,
                    response.message,
                )
            except grpc.aio.AioRpcError as error:
                attempt_latency_ms = (time.monotonic() - attempt_started) * 1000.0
                code = error.code()
                last_status = code.name
                trailing = await self._trailing_metadata(call)
                handler_observed = metadata_value(
                    trailing, "x-ndnsf-handler-executions") is not None
                if measured:
                    metrics.status_counts[last_status] += 1
                    if handler_observed:
                        metrics.handler_executions += 1
                print(
                    f"GRPC_FAILOVER_ATTEMPT request_id={logical_request_id} "
                    f"attempt={attempt_number} provider={endpoint.provider_id} "
                    f"status={last_status} latency_ms={attempt_latency_ms:.3f} "
                    f"handler_observed={int(handler_observed)}",
                    flush=True,
                )
                if not is_retryable(code):
                    break
                endpoint.healthy = False
                endpoint.last_status = last_status
                endpoint.last_checked_at = time.monotonic()
            except asyncio.TimeoutError:
                attempt_latency_ms = (time.monotonic() - attempt_started) * 1000.0
                last_status = "DEADLINE_EXCEEDED"
                if measured:
                    metrics.status_counts[last_status] += 1
                print(
                    f"GRPC_FAILOVER_ATTEMPT request_id={logical_request_id} "
                    f"attempt={attempt_number} provider={endpoint.provider_id} "
                    f"status={last_status} latency_ms={attempt_latency_ms:.3f} "
                    "handler_observed=0",
                    flush=True,
                )
                endpoint.healthy = False
                endpoint.last_status = last_status
                endpoint.last_checked_at = time.monotonic()
        total_latency_ms = (time.monotonic() - request_started) * 1000.0
        if measured:
            metrics.failures += 1
            metrics.status_counts["LOGICAL_FAILURE"] += 1
        return RequestOutcome(
            logical_request_id, False, None, last_status, total_latency_ms)


    async def execute_request_parallel(
        self,
        logical_request_id: int,
        metrics: Metrics,
        *,
        measured: bool = True,
    ) -> RequestOutcome:
        """Issue one RPC to every endpoint and keep the first successful reply.

        This is a diagnostic mode, not a retry implementation. All endpoints
        receive the same logical request concurrently, the same per-attempt
        timeout, and the same global deadline. Every issued RPC is counted as
        an attempt; fan-out and cancellation cost are recorded separately.
        """
        request_started = time.monotonic()
        request_deadline = request_started + self.global_deadline_s
        endpoints = rotated(self.endpoints, logical_request_id)
        if measured:
            metrics.sent += 1
            metrics.attempts += len(endpoints)
            metrics.parallel_issued += len(endpoints)
        if not endpoints:
            if measured:
                metrics.failures += 1
                metrics.status_counts["NO_ENDPOINTS"] += 1
                metrics.status_counts["LOGICAL_FAILURE"] += 1
            return RequestOutcome(
                logical_request_id, False, None, "NO_ENDPOINTS", 0.0)

        request = helloworld_pb2.HelloRequest(name=f"Test-{logical_request_id}")

        async def issue(endpoint: Endpoint, attempt_number: int) -> dict[str, Any]:
            remaining_s = request_deadline - time.monotonic()
            if remaining_s <= 0:
                return {
                    "endpoint": endpoint,
                    "status": "GLOBAL_DEADLINE_EXCEEDED",
                    "response": None,
                    "latency_ms": 0.0,
                    "handler_observed": False,
                }
            call_timeout_s = min(self.attempt_timeout_s, remaining_s)
            attempt_started = time.monotonic()
            call = endpoint.say_hello(
                request,
                timeout=call_timeout_s,
                metadata=(
                    ("x-ndnsf-logical-request-id", str(logical_request_id)),
                    ("x-ndnsf-attempt", str(attempt_number)),
                    ("x-ndnsf-execution-mode", "parallel-first-success"),
                ),
            )
            try:
                response = await call
                trailing = await self._trailing_metadata(call)
                status = "OK" if response is not None else "NO_RESPONSE"
                return {
                    "endpoint": endpoint,
                    "status": status,
                    "response": response,
                    "trailing": trailing,
                    "latency_ms": (time.monotonic() - attempt_started) * 1000.0,
                    "handler_observed": metadata_value(
                        trailing, "x-ndnsf-handler-executions") is not None,
                }
            except grpc.aio.AioRpcError as error:
                trailing = await self._trailing_metadata(call)
                return {
                    "endpoint": endpoint,
                    "status": error.code().name,
                    "response": None,
                    "trailing": trailing,
                    "latency_ms": (time.monotonic() - attempt_started) * 1000.0,
                    "handler_observed": metadata_value(
                        trailing, "x-ndnsf-handler-executions") is not None,
                }
            except asyncio.TimeoutError:
                return {
                    "endpoint": endpoint,
                    "status": "DEADLINE_EXCEEDED",
                    "response": None,
                    "latency_ms": (time.monotonic() - attempt_started) * 1000.0,
                    "handler_observed": False,
                }

        tasks = {
            asyncio.create_task(issue(endpoint, attempt_number),
                                 name=f"grpc-parallel-{logical_request_id}-"
                                      f"{attempt_number}")
            for attempt_number, endpoint in enumerate(endpoints, start=1)
        }
        pending = set(tasks)
        winner: dict[str, Any] | None = None
        last_status = "NO_ATTEMPT"
        try:
            while pending:
                done, pending = await asyncio.wait(
                    pending, return_when=asyncio.FIRST_COMPLETED)
                for task in done:
                    result = task.result()
                    endpoint = result["endpoint"]
                    status = result["status"]
                    last_status = status
                    if measured:
                        metrics.status_counts[status] += 1
                        if result.get("handler_observed"):
                            metrics.handler_executions += 1
                    print(
                        f"GRPC_PARALLEL_ATTEMPT request_id={logical_request_id} "
                        f"provider={endpoint.provider_id} status={status} "
                        f"latency_ms={result['latency_ms']:.3f} "
                        f"handler_observed={int(result.get('handler_observed', False))}",
                        flush=True,
                    )
                    if status == "OK" and winner is None:
                        winner = result
                if winner is not None:
                    for task in pending:
                        task.cancel()
                    if pending:
                        if measured:
                            metrics.parallel_cancellations += len(pending)
                        await asyncio.gather(*pending, return_exceptions=True)
                    pending.clear()
                    break
        finally:
            for task in pending:
                task.cancel()
            if pending:
                await asyncio.gather(*pending, return_exceptions=True)

        total_latency_ms = (time.monotonic() - request_started) * 1000.0
        if winner is not None:
            endpoint = winner["endpoint"]
            if measured:
                metrics.success += 1
                metrics.parallel_winners += 1
                metrics.latencies_ms.append(total_latency_ms)
            print(
                f"GRPC_PARALLEL_WINNER request_id={logical_request_id} "
                f"provider={endpoint.provider_id} latency_ms={total_latency_ms:.3f}",
                flush=True,
            )
            response = winner["response"]
            return RequestOutcome(
                logical_request_id,
                True,
                endpoint.provider_id,
                "OK",
                total_latency_ms,
                response.message if response is not None else "",
            )
        if measured:
            metrics.failures += 1
            metrics.status_counts["LOGICAL_FAILURE"] += 1
        return RequestOutcome(
            logical_request_id, False, None, last_status, total_latency_ms)


async def schedule_requests(
    client: ThreeProviderFailoverClient,
    *,
    count: int,
    interval_s: float,
    first_request_id: int,
    metrics: Metrics,
    measured: bool,
) -> list[RequestOutcome]:
    started = time.monotonic()
    tasks: list[asyncio.Task[RequestOutcome]] = []
    try:
        for index in range(count):
            scheduled_at = started + index * interval_s
            delay_s = scheduled_at - time.monotonic()
            if delay_s > 0:
                await asyncio.sleep(delay_s)
            request_id = first_request_id + index
            tasks.append(asyncio.create_task(
                client.execute_request(request_id, metrics, measured=measured),
                name=f"grpc-request-{request_id}",
            ))
        if not tasks:
            return []
        return list(await asyncio.gather(*tasks))
    except BaseException:
        for task in tasks:
            if not task.done():
                task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        raise


async def wait_for_measurement_start(
    *,
    process_started_at: float,
    delay_s: float,
    absolute_monotonic_s: float,
    tolerance_s: float,
) -> tuple[float, float, float]:
    """Wait for a shared CLOCK_MONOTONIC target and report offset/lateness."""
    if absolute_monotonic_s > 0 and delay_s > 0:
        raise ValueError(
            "--measurement-start-delay-s and --measurement-start-monotonic-s "
            "are mutually exclusive")
    target = (absolute_monotonic_s if absolute_monotonic_s > 0
              else process_started_at + delay_s)
    remaining = target - time.monotonic()
    if remaining > 0:
        await asyncio.sleep(remaining)
    actual = time.monotonic()
    offset_s = actual - process_started_at
    lateness_s = max(0.0, actual - target)
    if tolerance_s > 0 and lateness_s > tolerance_s:
        raise ValueError(
            f"measurement start missed by {lateness_s * 1000.0:.3f} ms; "
            f"tolerance is {tolerance_s * 1000.0:.3f} ms")
    return actual, offset_s, lateness_s


def format_summary(
    metrics: Metrics,
    *,
    health_checks: int,
    health_success: int,
    duration_s: float,
    offered_rps: float,
    measurement_start_offset_s: float = 0.0,
    measurement_start_lateness_ms: float = 0.0,
    pre_measurement_health_checks: int = 0,
    pre_measurement_health_success: int = 0,
    health_routing_enabled: bool = True,
    parallel_enabled: bool = False,
) -> str:
    p50 = percentile_nearest_rank(metrics.latencies_ms, 0.50)
    p95 = percentile_nearest_rank(metrics.latencies_ms, 0.95)
    p99 = percentile_nearest_rank(metrics.latencies_ms, 0.99)
    mean_ms = (sum(metrics.latencies_ms) / len(metrics.latencies_ms)
               if metrics.latencies_ms else 0.0)
    actual_success_rps = metrics.success / duration_s if duration_s > 0 else 0.0
    application_rpc_calls = metrics.attempts + health_checks
    # Application-level accounting only: one request event and one terminal
    # response/status event per issued unary RPC. This is not a wire-packet count.
    application_messages = 2 * application_rpc_calls
    return (
        f"GRPC_FAILOVER_RATE sent={metrics.sent} success={metrics.success} "
        f"failures={metrics.failures} attempts={metrics.attempts} "
        f"failovers={metrics.failovers} health_checks={health_checks} "
        f"health_success={health_success} "
        f"health_directed_selections={metrics.health_directed_selections} "
        f"execution_mode={'parallel-first-success' if parallel_enabled else 'sequential-failover'} "
        f"parallel_issued={metrics.parallel_issued} "
        f"parallel_winners={metrics.parallel_winners} "
        f"parallel_cancellations={metrics.parallel_cancellations} "
        f"health_routing={'enabled' if health_routing_enabled else 'disabled'} "
        f"handler_executions_observed={metrics.handler_executions} "
        f"application_rpc_calls={application_rpc_calls} "
        f"application_messages={application_messages} "
        f"mean_ms={mean_ms:.3f} p50_ms={p50:.3f} p95_ms={p95:.3f} p99_ms={p99:.3f} "
        f"actual_success_rps={actual_success_rps:.3f} "
        f"duration_s={duration_s:.3f} offered_rps={offered_rps:.3f} "
        f"measurement_start_offset_s={measurement_start_offset_s:.6f} "
        f"measurement_start_lateness_ms={measurement_start_lateness_ms:.3f} "
        f"pre_measurement_health_checks={pre_measurement_health_checks} "
        f"pre_measurement_health_success={pre_measurement_health_success} "
        f"status_ok={metrics.status_counts['OK']} "
        f"status_unavailable={metrics.status_counts['UNAVAILABLE']} "
        f"status_deadline_exceeded={metrics.status_counts['DEADLINE_EXCEEDED']} "
        f"status_health_directed_selection="
        f"{metrics.status_counts['HEALTH_DIRECTED_SELECTION']} "
        "message_definition=rpc_request_plus_terminal_event"
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Explicit multi-provider gRPC health/failover baseline")
    parser.add_argument(
        "--target",
        action="append",
        required=True,
        metavar="PROVIDER_ID=HOST:PORT",
        help="repeat at least three times; single-provider mode permits one",
    )
    parser.add_argument(
        "--single-provider", action="store_true",
        help="allow exactly one target and never fail over to another provider",
    )
    parser.add_argument("--count", type=int, default=5)
    parser.add_argument("--interval-ms", type=float, default=0.0)
    parser.add_argument("--rate-rps", type=float, default=0.0)
    parser.add_argument("--duration-s", type=float, default=0.0)
    parser.add_argument("--warmup-s", type=float, default=0.0)
    parser.add_argument("--global-deadline-s", type=float, default=5.0)
    parser.add_argument("--attempt-timeout-s", type=float, default=0.2)
    parser.add_argument("--health-interval-s", type=float, default=0.2)
    parser.add_argument("--health-timeout-s", type=float, default=0.2)
    parser.add_argument("--health-stale-s", type=float, default=0.4)
    parser.add_argument("--prewarm-timeout-s", type=float, default=2.0)
    parser.add_argument("--measurement-start-delay-s", type=float, default=0.0)
    parser.add_argument("--measurement-start-monotonic-s", type=float, default=0.0)
    parser.add_argument("--measurement-start-tolerance-s", type=float, default=0.0)
    parser.add_argument("--require-all-prewarmed", action="store_true")
    parser.add_argument(
        "--disable-health-routing", action="store_true",
        help="strict sequential mode: do not probe or reorder by health state",
    )
    parser.add_argument(
        "--parallel", action="store_true",
        help="diagnostic first-success fan-out: issue one RPC per provider concurrently",
    )
    parser.add_argument(
        "--stats-only",
        action="store_true",
        help="query exact server-owned JSON snapshots and do not run workload",
    )
    parser.add_argument(
        "--reset-stats",
        action="store_true",
        help="with --stats-only, reset counters before returning snapshots",
    )
    parser.add_argument("--stats-timeout-s", type=float, default=2.0)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    return parser


async def run_stats_from_args(args: argparse.Namespace) -> dict[str, Any]:
    targets = parse_target_specs(
        args.target, allow_single_provider=args.single_provider)
    client = ThreeProviderFailoverClient(
        targets,
        prewarm_timeout_s=args.prewarm_timeout_s,
        parallel=False,
        quiet=True,
        allow_single_provider=args.single_provider,
    )
    try:
        if args.reset_stats:
            snapshot = await client.reset_stats_all(args.stats_timeout_s)
        else:
            snapshot = await client.stats_snapshot_all(args.stats_timeout_s)
        for provider_id, provider_snapshot in snapshot["providers"].items():
            print(
                "GRPC_PROVIDER_STATS_JSON " +
                json.dumps(
                    {"provider_id": provider_id, **provider_snapshot},
                    sort_keys=True,
                    separators=(",", ":"),
                ),
                flush=True,
            )
        print(
            "GRPC_STATS_SNAPSHOT_JSON " +
            json.dumps(snapshot, sort_keys=True, separators=(",", ":")),
            flush=True,
        )
        return snapshot
    finally:
        await client.close()


async def run_from_args(args: argparse.Namespace) -> tuple[Metrics, str]:
    process_started_at = getattr(args, "_process_started_at", time.monotonic())
    targets = parse_target_specs(
        args.target, allow_single_provider=args.single_provider)
    if args.count <= 0:
        raise ValueError("--count must be positive")
    if (args.interval_ms < 0 or args.rate_rps < 0 or args.duration_s < 0 or
            args.measurement_start_delay_s < 0 or
            args.measurement_start_monotonic_s < 0 or
            args.measurement_start_tolerance_s < 0 or
            args.stats_timeout_s <= 0):
        raise ValueError("interval, rate, and duration cannot be negative")
    if (args.rate_rps > 0) != (args.duration_s > 0):
        raise ValueError("--rate-rps and --duration-s must be supplied together")
    client = ThreeProviderFailoverClient(
        targets,
        global_deadline_s=args.global_deadline_s,
        attempt_timeout_s=args.attempt_timeout_s,
        health_interval_s=args.health_interval_s,
        health_timeout_s=args.health_timeout_s,
        health_stale_s=args.health_stale_s,
        prewarm_timeout_s=args.prewarm_timeout_s,
        parallel=args.parallel,
        quiet=args.quiet,
        allow_single_provider=args.single_provider,
    )
    try:
        await client.prewarm_all()
        if args.require_all_prewarmed:
            unavailable = [
                endpoint.provider_id for endpoint in client.endpoints
                if not endpoint.channel_ready]
            if unavailable:
                raise ValueError(
                    "all configured channels must prewarm; unavailable: " +
                    ",".join(unavailable))
        if not args.disable_health_routing:
            await client.probe_all()
            client.start_health_monitoring()
        if args.warmup_s > 0:
            warmup_rate = args.rate_rps if args.rate_rps > 0 else max(1.0, 1000.0 / max(1.0, args.interval_ms))
            warmup_count = max(1, int(round(warmup_rate * args.warmup_s)))
            await schedule_requests(
                client,
                count=warmup_count,
                interval_s=1.0 / warmup_rate,
                first_request_id=-warmup_count,
                metrics=Metrics(),
                measured=False,
            )

        measurement_started, measurement_start_offset_s, measurement_lateness_s = (
            await wait_for_measurement_start(
                process_started_at=process_started_at,
                delay_s=args.measurement_start_delay_s,
                absolute_monotonic_s=args.measurement_start_monotonic_s,
                tolerance_s=args.measurement_start_tolerance_s,
            ))
        print(
            f"GRPC_MEASUREMENT_START monotonic_s={measurement_started:.6f} "
            f"offset_s={measurement_start_offset_s:.6f} "
            f"lateness_ms={measurement_lateness_s * 1000.0:.3f}",
            flush=True,
        )
        pre_measurement_health_checks, pre_measurement_health_success = (
            count_health_events(
                client.health_events, float("-inf"), measurement_started))
        metrics = Metrics()
        if args.rate_rps > 0:
            count = max(1, int(round(args.rate_rps * args.duration_s)))
            interval_s = 1.0 / args.rate_rps
            report_duration_s = args.duration_s
            offered_rps = args.rate_rps
        else:
            count = min(args.count, 3) if args.smoke else args.count
            interval_s = args.interval_ms / 1000.0
            report_duration_s = 0.0
            offered_rps = 1.0 / interval_s if interval_s > 0 else 0.0
        await schedule_requests(
            client,
            count=count,
            interval_s=interval_s,
            first_request_id=0,
            metrics=metrics,
            measured=True,
        )
        elapsed_s = time.monotonic() - measurement_started
        if report_duration_s <= 0:
            report_duration_s = max(elapsed_s, 1e-9)
        measurement_ended = measurement_started + report_duration_s
        health_checks, health_success = count_health_events(
            client.health_events, measurement_started, measurement_ended)
        summary = format_summary(
            metrics,
            health_checks=health_checks,
            health_success=health_success,
            duration_s=report_duration_s,
            offered_rps=offered_rps,
            measurement_start_offset_s=measurement_start_offset_s,
            measurement_start_lateness_ms=measurement_lateness_s * 1000.0,
            pre_measurement_health_checks=pre_measurement_health_checks,
            pre_measurement_health_success=pre_measurement_health_success,
            health_routing_enabled=not args.disable_health_routing,
            parallel_enabled=args.parallel,
        )
        print(summary, flush=True)
        if args.smoke and metrics.sent > 0 and metrics.success == metrics.sent:
            print("SMOKE_OK", flush=True)
        return metrics, summary
    finally:
        await client.close()


def main() -> int:
    process_started_at = time.monotonic()
    parser = build_parser()
    args = parser.parse_args()
    args._process_started_at = process_started_at
    if args.reset_stats and not args.stats_only:
        parser.error("--reset-stats requires --stats-only")
    try:
        if args.stats_only:
            asyncio.run(run_stats_from_args(args))
            return 0
        metrics, _ = asyncio.run(run_from_args(args))
    except ValueError as error:
        parser.error(str(error))
    if args.smoke and (metrics.sent == 0 or metrics.success != metrics.sent):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
