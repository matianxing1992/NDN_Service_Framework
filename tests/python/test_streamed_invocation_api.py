"""Host-side contract checks for the native streamed invocation bindings.

These tests deliberately stop at the Python/C++ API boundary.  Transport and
authorization delivery are covered by the native integration gate; this file
ensures the public Python facade does not silently fall back to a Python-only
queue or reintroduce a Provider list for Normal requests.
"""

from __future__ import annotations

import asyncio
import inspect
from types import SimpleNamespace

import pytest


ndnsf_native = pytest.importorskip("ndnsf._ndnsf")
from ndnsf.service import (  # noqa: E402
    ServiceProvider, ServiceUser, StreamedInvocation,
    StreamedInvocationError, StreamedInvocationMetrics,
    StreamedInvocationOptions,
)
from ndnsf_distributed_inference.provider import ProviderRuntimeContext  # noqa: E402


def test_native_streaming_surface_is_bound() -> None:
    assert hasattr(ndnsf_native, "StreamWriter")
    assert hasattr(ndnsf_native, "NativeStreamedInvocationHandle")
    assert hasattr(ndnsf_native.StreamWriter, "publish_event")
    assert hasattr(ndnsf_native.StreamWriter, "finish_stream")
    assert hasattr(ndnsf_native.StreamWriter, "fail")
    assert hasattr(ndnsf_native.NativeServiceProvider, "add_streaming_service")
    assert hasattr(ndnsf_native.NativeServiceProvider, "start")
    assert hasattr(ndnsf_native.NativeServiceProvider,
                   "add_streaming_context_service")
    assert hasattr(ndnsf_native.NativeServiceUser, "request_service_streaming")
    assert hasattr(ndnsf_native.NativeServiceUser,
                   "request_service_streaming_handle")


class _FakeNativeStreamHandle:
    def __init__(self, request_id: str = "/request/native-stream") -> None:
        self.request_id = request_id
        self.status = "Completed"
        self.metrics = {
            "published_events": 2,
            "delivered_events": 2,
            "retry_count": 1,
            "duplicate_count": 0,
        }
        self.cancel_count = 0

    def cancel(self) -> None:
        self.cancel_count += 1


class _FakeNativeStreamingUser:
    def __init__(self) -> None:
        self.calls = []
        self.handle = _FakeNativeStreamHandle()
        self.on_event = None
        self.on_complete = None
        self.on_error = None

    def request_service_streaming_handle(
        self, service, payload, provider, options, strategy,
        on_event, on_complete, on_error,
    ):
        self.calls.append((service, payload, provider, options, strategy))
        self.on_event = on_event
        self.on_complete = on_complete
        self.on_error = on_error
        return self.handle


def test_contract_level_stream_handle_forwards_frozen_options() -> None:
    native = _FakeNativeStreamingUser()
    user = object.__new__(ServiceUser)
    user._native = native
    observed = []
    completed = []
    failed = []
    options = StreamedInvocationOptions(max_events=7, callback_queue_capacity=3)

    invocation = user.request_service_streaming(
        "/LLM/Test", b"prompt", options=options,
        on_event=observed.append,
        on_complete=completed.append,
        on_error=failed.append,
    )

    assert isinstance(invocation, StreamedInvocation)
    assert invocation.request_id == "/request/native-stream"
    assert native.calls == [(
        "/LLM/Test", b"prompt", "", options.as_dict(), "first-responding",
    )]
    native.on_event(b"token-1")
    native.on_complete(b"done")
    assert observed == [b"token-1"]
    assert completed == [b"done"]
    assert failed == []
    with pytest.raises(RuntimeError, match="already has an event consumer"):
        invocation.__aiter__()


def test_contract_level_stream_async_iterator_and_result_decode() -> None:
    native = _FakeNativeStreamingUser()
    user = object.__new__(ServiceUser)
    user._native = native
    invocation = user.request_service_streaming(
        "/LLM/Test", b"prompt",
        event_decoder=lambda value: value.decode("utf-8"),
        response_decoder=lambda value: {"value": value.decode("utf-8")},
    )
    native.on_event(b"token-1")
    native.on_event(b"token-2")
    native.on_complete(b"done")

    async def consume():
        events = []
        async for event in invocation:
            events.append(event)
        return events, await invocation.result()

    events, result = asyncio.run(consume())
    assert events == ["token-1", "token-2"]
    assert result == {"value": "done"}
    assert invocation.status == "Completed"
    assert invocation.metrics == StreamedInvocationMetrics(
        published_events=2, delivered_events=2,
        retry_count=1, duplicate_count=0,
    )


def test_contract_level_stream_result_raises_native_error() -> None:
    native = _FakeNativeStreamingUser()
    user = object.__new__(ServiceUser)
    user._native = native
    invocation = user.request_service_streaming("/LLM/Test", b"prompt")
    native.on_error({
        "code": 6,
        "message": "event gap expired",
        "requestId": "/request/native-stream",
        "expectedCursor": 3,
        "providerName": "/provider/a",
    })

    with pytest.raises(StreamedInvocationError) as caught:
        asyncio.run(invocation.result())
    assert caught.value.code == 6
    assert caught.value.expected_cursor == 3
    assert caught.value.provider_name == "/provider/a"


def test_contract_level_stream_cancel_delegates_idempotently() -> None:
    native = _FakeNativeStreamingUser()
    user = object.__new__(ServiceUser)
    user._native = native
    invocation = user.request_service_streaming("/LLM/Test", b"prompt")

    asyncio.run(invocation.cancel())
    asyncio.run(invocation.cancel())
    assert native.handle.cancel_count == 1


def test_contract_level_stream_rejects_mode_target_mismatch() -> None:
    user = object.__new__(ServiceUser)
    user._native = _FakeNativeStreamingUser()
    with pytest.raises(ValueError, match="Normal.*target_provider"):
        user.request_service_streaming(
            "/LLM/Test", b"prompt", target_provider="/provider/a")
    with pytest.raises(ValueError, match="Targeted.*target_provider"):
        user.request_service_streaming(
            "/LLM/Test", b"prompt",
            options=StreamedInvocationOptions(mode="Targeted"))


def test_provider_facade_delegates_core_owned_writer() -> None:
    calls = []

    class FakeNative:
        def add_streaming_service(self, service, handler):
            calls.append((service, handler))

    provider = object.__new__(ServiceProvider)
    provider._native = FakeNative()
    provider._streaming_services = set()
    handler = lambda request, writer: writer.finish_stream(request)

    provider.add_streaming_handler("/LLM/Test", handler)

    assert calls == [("/LLM/Test", handler)]


def test_provider_context_and_decorator_use_native_core_writer() -> None:
    calls = []

    class FakeNative:
        def add_streaming_service(self, service, handler):
            calls.append(("plain", service, handler))

        def add_streaming_context_service(self, service, handler):
            calls.append(("context", service, handler))

    provider = object.__new__(ServiceProvider)
    provider._native = FakeNative()
    provider._streaming_services = set()

    @provider.streaming_handler("/LLM/Plain")
    def plain(request, writer):
        writer.finish_stream(request)

    @provider.streaming_context_handler("/LLM/Context")
    def contextual(context, request, writer):
        writer.finish_stream(context["request_id"].encode() + request)

    assert calls == [
        ("plain", "/LLM/Plain", plain),
        ("context", "/LLM/Context", contextual),
    ]
    assert provider._streaming_services == {"/LLM/Plain", "/LLM/Context"}


def test_provider_run_accepts_streaming_only_registration() -> None:
    called = []

    class FakeNative:
        def run(self):
            called.append("run")

    provider = object.__new__(ServiceProvider)
    provider._native = FakeNative()
    provider._handlers = {}
    provider._collaboration_services = set()
    provider._streaming_services = {"/LLM/Test"}
    assert provider.run() == 0
    assert called == ["run"]


def test_provider_start_registers_handlers_before_native_start() -> None:
    events = []

    class FakeNative:
        def add_service(self, *args):
            events.append(("add_service", args[0]))

        def start(self):
            events.append(("start",))

        def wait_until_ready(self, timeout_ms):
            assert timeout_ms == 15000
            events.append(("ready",))
            return True

    provider = object.__new__(ServiceProvider)
    provider._native = FakeNative()
    provider._handlers = {"/LLM/Test": lambda payload: payload}
    provider._context_handlers = set()
    provider._ack_handlers = {}
    provider._ack_context_handlers = set()
    provider._collaboration_services = set()
    provider._streaming_services = set()
    provider._registered_services = set()
    provider.start()
    assert events == [("add_service", "/LLM/Test"), ("start",), ("ready",)]
    provider.start()
    assert events == [("add_service", "/LLM/Test"), ("start",), ("ready",),
                      ("start",), ("ready",)]


def test_user_facade_has_single_normal_request_shape() -> None:
    calls = []

    class FakeNative:
        def request_service_streaming(self, *args):
            calls.append(args)
            return "/request-1"

    user = object.__new__(ServiceUser)
    user._native = FakeNative()
    on_event = lambda payload: None
    on_complete = lambda payload: None
    on_error = lambda error: None

    request_id = user.request_streaming(
        "/LLM/Test",
        b"prompt",
        on_event=on_event,
        on_complete=on_complete,
        on_error=on_error,
    )

    assert request_id == "/request-1"
    assert calls[0][:3] == ("/LLM/Test", b"prompt", "")

    signature = inspect.signature(ServiceUser.request_streaming)
    assert "providers" not in signature.parameters
    assert signature.parameters["provider"].default == ""


def test_deferred_streaming_reuses_one_collaboration_request() -> None:
    calls = []

    class FakeNative:
        def begin_collaboration(self, *args, **kwargs):
            calls.append((args, kwargs))
            return "/request-stream-1"

    user = object.__new__(ServiceUser)
    user._native = FakeNative()
    events = []
    complete = []
    errors = []
    invocation = user.begin_collaboration(
        "/LLM/Test", b"prompt",
        stream_options=StreamedInvocationOptions(),
        on_stream_event=events.append,
        on_stream_complete=complete.append,
        on_stream_error=errors.append,
    )

    assert invocation.request_id == "/request-stream-1"
    assert len(calls) == 1
    assert calls[0][1]["stream_options"]["mode"] == "Normal"
    assert callable(calls[0][1]["on_stream_event"])
    assert callable(calls[0][1]["on_stream_complete"])
    assert callable(calls[0][1]["on_stream_error"])


def test_deferred_streaming_forwards_frozen_attempt_identity() -> None:
    calls = []

    class FakeNative:
        def begin_collaboration(self, *args, **kwargs):
            calls.append((args, kwargs))
            return "/request-stream-2"

    user = object.__new__(ServiceUser)
    user._native = FakeNative()
    options = StreamedInvocationOptions(
        attempt_epoch=2,
        generation_id="01" * 16,
        stream_epoch=2,
    )
    user.begin_collaboration(
        "/LLM/Test", b"prompt",
        stream_options=options,
        on_stream_event=lambda _value: None,
        on_stream_complete=lambda _value: None,
        on_stream_error=lambda _value: None,
    )

    forwarded = calls[0][1]["stream_options"]
    assert forwarded["attempt_epoch"] == 2
    assert forwarded["generation_id"] == "01" * 16
    assert forwarded["stream_epoch"] == 2


def test_di_context_delegates_events_and_claims_stream_terminal() -> None:
    calls = []

    class FakeWriter:
        def publish_event(self, payload):
            calls.append(("event", bytes(payload)))
            return 1

        def finish_stream(self, payload, reason):
            calls.append(("finish", bytes(payload), int(reason)))
            return True

    context = ProviderRuntimeContext(
        ndnsf=SimpleNamespace(session_id="/request/1"),
        execution=object(),
        request=b"prompt",
        role="stage-0",
        stream_writer=FakeWriter(),
    )
    assert context.publish_event(b"token-1") == 1
    context.finish_stream(b"done", finish_reason="eos")
    assert calls == [("event", b"token-1"), ("finish", b"done", 1)]

    with pytest.raises(RuntimeError, match="already published"):
        context.publish_final_response(b"late-unary")


def test_di_context_exposes_cancellation_and_rejects_non_application_events() -> None:
    class FakeWriter:
        cancelled = True

        def publish_event(self, _payload, **_kwargs):
            return 1

    context = ProviderRuntimeContext(
        ndnsf=SimpleNamespace(session_id="/request/cancelled"),
        execution=object(), request=b"prompt", role="stage-0",
        stream_writer=FakeWriter(),
    )
    assert context.stream_cancelled() is True
    with pytest.raises(ValueError, match="event_type=application"):
        context.publish_event(b"token", event_type="end")


def test_di_context_terminal_claim_competes_in_both_directions() -> None:
    class FakeWriter:
        def finish_stream(self, _payload, _reason):
            return True

    first = ProviderRuntimeContext(
        ndnsf=SimpleNamespace(session_id="/request/first"),
        execution=object(), request=b"prompt", role="stage-0",
        stream_writer=FakeWriter(),
    )
    first.finish_stream(b"done", finish_reason="application_complete")
    with pytest.raises(RuntimeError, match="already published"):
        first.publish_final_response(b"late")

    class UnaryContext:
        session_id = "/request/second"

        def publish_final_response(self, _payload):
            return None

    second = ProviderRuntimeContext(
        ndnsf=UnaryContext(), execution=object(), request=b"prompt", role="stage-0",
        stream_writer=FakeWriter(),
    )
    second.publish_final_response(b"done")
    with pytest.raises(RuntimeError, match="already published"):
        second.finish_stream(b"late", finish_reason="application_complete")


def test_di_context_unary_fallback_remains_unchanged() -> None:
    published = []
    context = ProviderRuntimeContext(
        ndnsf=SimpleNamespace(
            session_id="/request/unary",
            publish_final_response=lambda payload: published.append(bytes(payload)),
        ),
        execution=object(),
        request=b"prompt",
        role="stage-0",
    )
    context.finish_stream(b"unary-result")
    assert published == [b"unary-result"]
