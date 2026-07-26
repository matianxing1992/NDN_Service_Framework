"""Bounded policy execution; late results never mutate the current request."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, TimeoutError
from typing import Any, Callable


class PolicyExecutionTimeout(TimeoutError):
    pass


class BoundedPolicyExecutor:
    def execute(self, function: Callable[[Any], Any], request: Any,
                timeout_ms: int) -> Any:
        pool = ThreadPoolExecutor(max_workers=1)
        future = pool.submit(function, request)
        try:
            return future.result(timeout=max(1, timeout_ms) / 1000.0)
        except TimeoutError as exc:
            future.cancel()
            raise PolicyExecutionTimeout("policy execution budget exceeded") from exc
        finally:
            pool.shutdown(wait=False)


__all__ = ["BoundedPolicyExecutor", "PolicyExecutionTimeout"]
