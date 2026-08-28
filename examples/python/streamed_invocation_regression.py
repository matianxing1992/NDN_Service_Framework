#!/usr/bin/env python3
"""Real Python Provider/User regression for the generic streamed invocation."""

from __future__ import annotations

import argparse
import asyncio
import threading
import time

from ndnsf import (
    ServiceProvider,
    ServiceUser,
    StreamedInvocationError,
    StreamedInvocationOptions,
)


SERVICE = "/HELLO"
EVENTS = (b"event-1", b"event-2", b"event-3", b"event-4", b"event-5")
RESULT = b"complete-result"
_lifetime_probe_started = False


def run_provider() -> int:
    provider = ServiceProvider(
        provider_id="",
        group="/example/hello/group",
        controller="/example/hello/controller",
        provider_prefix="/example/hello/provider",
        trust_schema="examples/trust-schema.conf",
        serve_certificates=True,
    )

    @provider.streaming_context_handler(SERVICE)
    def generate(context, request, writer) -> None:
        global _lifetime_probe_started
        if request == b"stream-fail":
            raise RuntimeError("intentional Python streamed handler failure")
        if request not in (b"stream-request", b"stream-callback"):
            writer.fail(20, "unexpected request payload")
            return
        if context["service"] != SERVICE or not context["request_id"]:
            writer.fail(20, "invalid authenticated stream context")
            return
        if threading.current_thread() is threading.main_thread():
            writer.fail(20, "Python streamed handler ran on the Face/main thread")
            return
        for expected_cursor, payload in enumerate(EVENTS, start=1):
            cursor = writer.publish_event(payload)
            if cursor != expected_cursor:
                writer.fail(17, "Core rejected ordered Python event")
                return
        if not writer.finish_stream(RESULT, 1):
            raise RuntimeError("Core rejected Python stream terminal")
        if not _lifetime_probe_started:
            _lifetime_probe_started = True

            def probe_invalidated_writer() -> None:
                # The handler must return before this probe runs. Core then
                # invalidates the capability; no late publish or terminal may
                # be accepted even though Python still owns the wrapper.
                time.sleep(0.2)
                passed = (
                    writer.publish_event(b"late-event") == 0
                    and not writer.finish_stream(b"late-result", 1)
                    and not writer.fail(20, "late-failure")
                )
                print(
                    "PYTHON_STREAM_WRITER_FENCED=" + ("PASS" if passed else "FAIL"),
                    flush=True,
                )

            threading.Thread(target=probe_invalidated_writer, daemon=True).start()
        print("PYTHON_STREAM_PROVIDER_FINISHED", flush=True)

    print("PYTHON_STREAM_PROVIDER_START", flush=True)
    return provider.run()


async def consume_stream() -> None:
    user = ServiceUser(
        group="/example/hello/group",
        controller="/example/hello/controller",
        user="/example/hello/user",
        trust_schema="examples/trust-schema.conf",
        serve_certificates=True,
    )
    try:
        options = StreamedInvocationOptions(
            # max_events includes the authenticated End event, so five
            # application events require six cursor slots.
            max_events=6,
            interest_window=3,
            callback_queue_capacity=8,
        )
        invocation = user.request_service_streaming(
            SERVICE,
            b"stream-request",
            options=options,
            event_decoder=lambda value: value.decode("utf-8"),
            response_decoder=lambda value: value.decode("utf-8"),
        )
        observed = []

        async def collect() -> None:
            async for event in invocation:
                observed.append(event)

        await asyncio.wait_for(collect(), timeout=20.0)
        result = await asyncio.wait_for(invocation.result(), timeout=2.0)
        expected = [value.decode("utf-8") for value in EVENTS]
        if observed != expected:
            raise RuntimeError(f"event mismatch: {observed!r} != {expected!r}")
        if result != RESULT.decode("utf-8"):
            raise RuntimeError(f"result mismatch: {result!r}")
        if invocation.status != "Completed":
            raise RuntimeError(f"unexpected terminal status: {invocation.status}")
        if invocation.metrics.delivered_events != len(EVENTS):
            raise RuntimeError(
                "Core delivered-event counter does not match Python iterator")

        callback_events = []
        callback_results = []
        callback_errors = []
        callback_invocation = user.request_service_streaming(
            SERVICE,
            b"stream-callback",
            options=options,
            event_decoder=lambda value: value.decode("utf-8"),
            response_decoder=lambda value: value.decode("utf-8"),
            on_event=callback_events.append,
            on_complete=callback_results.append,
            on_error=callback_errors.append,
        )
        try:
            callback_invocation.__aiter__()
        except RuntimeError:
            pass
        else:
            raise RuntimeError("callback invocation incorrectly allowed async iteration")
        callback_result = await asyncio.wait_for(
            callback_invocation.result(), timeout=20.0)
        if callback_events != expected or callback_results != [callback_result]:
            raise RuntimeError(
                "real callback delivery disagrees with iterator delivery")
        if callback_errors or callback_result != RESULT.decode("utf-8"):
            raise RuntimeError("real callback invocation did not complete cleanly")

        failed_callbacks = []
        failed_invocation = user.request_service_streaming(
            SERVICE,
            b"stream-fail",
            options=options,
            on_error=lambda error: (
                failed_callbacks.append(error),
                print(
                    f"PYTHON_STREAM_ERROR_CALLBACK code={error.code} "
                    f"message={error}",
                    flush=True,
                ),
            ),
        )
        try:
            await asyncio.wait_for(failed_invocation.result(), timeout=20.0)
        except StreamedInvocationError as error:
            if error.code != 17 or "intentional Python streamed handler failure" not in str(error):
                raise RuntimeError(f"unexpected contained handler failure: {error}") from error
            if failed_callbacks != [error]:
                raise RuntimeError("stream failure callback/result did not share one error")
        else:
            raise RuntimeError("Python handler exception escaped terminal failure path")
        print(
            "PYTHON_STREAM_USER_PASS "
            f"request_id={invocation.request_id} events={len(observed)} "
            f"callback_events={len(callback_events)} contained_failure=1 "
            f"result={result}",
            flush=True,
        )
    finally:
        user.stop()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("role", choices=("provider", "user"))
    args = parser.parse_args()
    if args.role == "provider":
        return run_provider()
    asyncio.run(consume_stream())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
