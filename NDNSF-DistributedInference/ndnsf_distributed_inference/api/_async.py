"""Small asyncio adapters for the native prepared-model handles.

The native operation, timer, ownership, and cancellation state remain in C++.
These helpers only create a Future, marshal completion onto the running loop,
and unsubscribe the native waiter when the Python wait is cancelled.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any


class NativeCallbackError(RuntimeError):
    """Structured native callback failure exposed to an asyncio caller."""

    def __init__(self, details: Any):
        self.details = details
        if isinstance(details, dict):
            self.code = str(details.get("code", ""))
            self.domain = str(details.get("domain", ""))
            self.boundary = str(details.get("boundary", ""))
            self.request_id = str(details.get("request_id", ""))
            self.attempt = int(details.get("attempt", 0))
        else:
            self.code = self.domain = self.boundary = self.request_id = ""
            self.attempt = 0
        if isinstance(details, dict):
            message = str(details.get("message", "native operation failed"))
        else:
            message = str(details)
        super().__init__(message)


def _deliver(loop: asyncio.AbstractEventLoop, future: asyncio.Future,
             error: Any, value: Any) -> None:
    def finish() -> None:
        if future.done():
            return
        if error is None:
            future.set_result(value)
        else:
            future.set_exception(NativeCallbackError(error))

    try:
        loop.call_soon_threadsafe(finish)
    except RuntimeError:
        # The loop is closed.  The caller's cancellation/finalizer owns the
        # native subscription; do not invoke Python after loop shutdown.
        return


async def _await_subscription(register, *, timeout_s: float | None) -> Any:
    loop = asyncio.get_running_loop()
    future = loop.create_future()
    holder: list[Any] = [None]

    def callback(error: Any, value: Any) -> None:
        _deliver(loop, future, error, value)
        holder[0] = None

    holder[0] = register(timeout_s, callback)
    try:
        return await future
    except asyncio.CancelledError:
        subscription = holder[0]
        holder[0] = None
        if subscription is not None:
            subscription.unsubscribe()
        raise
    finally:
        holder[0] = None


async def preparation_result(handle: Any, timeout_s: float | None = None) -> Any:
    """Await one native preparation waiter without cancelling the job."""

    if timeout_s is None:
        return await _await_subscription(
            lambda _timeout, callback: handle.on_completion(callback),
            timeout_s=None)
    return await _await_subscription(
        lambda timeout, callback: handle.result_async(timeout, callback),
        timeout_s=timeout_s)


async def request_result(handle: Any, timeout_s: float | None = None) -> Any:
    """Await one native request result; cancellation only cancels this wait."""

    if timeout_s is None:
        return await _await_subscription(
            lambda _timeout, callback: handle.on_completion(callback),
            timeout_s=None)
    return await _await_subscription(
        lambda timeout, callback: handle.result_async(timeout, callback),
        timeout_s=timeout_s)


async def drain_result(owner: Any, timeout_s: float | None = 5.0) -> bool:
    """Await a native owner drain notification."""

    return bool(await _await_subscription(
        lambda timeout, callback: owner.drain_async(timeout, callback),
        timeout_s=timeout_s))


async def runtime_enter(runtime: Any) -> Any:
    """Async-context entry; Runtime.open itself remains synchronous."""

    return runtime


async def runtime_exit(runtime: Any, exc_type: Any = None,
                       exc: Any = None, traceback: Any = None,
                       timeout_s: float | None = 5.0) -> bool:
    """Close and await the native Runtime drain barrier."""

    del exc, traceback
    runtime.close()
    drained = await drain_result(runtime, timeout_s)
    if not drained:
        details = {
            "code": "SHUTDOWN_TIMEOUT",
            "domain": "local",
            "boundary": "runtime",
            "message": "native Runtime did not drain before async context exit",
        }
        if exc_type is not None:
            logging.getLogger(__name__).error("async Runtime shutdown failed: %s", details)
            return False
        raise NativeCallbackError(details)
    return False


async def next_event(reader: Any, timeout_s: float | None = None) -> Any:
    """Await one native EventReader read."""

    return await _await_subscription(
        lambda timeout, callback: reader.next_async(timeout, callback),
        timeout_s=timeout_s)


async def events_async(handle: Any, timeout_s: float | None = None):
    """Yield reliable native events and close only the reader on exit."""

    reader = handle.events()
    try:
        while True:
            event = await next_event(reader, timeout_s)
            if event is None:
                return
            yield event
    finally:
        reader.close()


async def prepare(user: Any, model_key: str = "default", *,
                  options: Any = None, timeout_s: float | None = None) -> Any:
    """Start a native preparation and await only its Python waiter."""

    if options is None:
        from ndnsf import _ndnsf
        options = _ndnsf.PrepareOptions()
    handle = user.start_prepare(model_key, options=options, timeout_s=timeout_s)
    # timeout_s is the native preparation-job deadline, so await the terminal
    # completion without installing a second Python-side deadline.
    return await preparation_result(handle)


async def _prepare_positional(user: Any, model_key: str,
                              options: Any, timeout_s: float | None) -> Any:
    """Private positional bridge for the pybind method adaptor."""

    return await prepare(user, model_key, options=options, timeout_s=timeout_s)


__all__ = [
    "NativeCallbackError",
    "drain_result",
    "events_async",
    "next_event",
    "preparation_result",
    "prepare",
    "request_result",
    "runtime_enter",
    "runtime_exit",
]
