"""Retry policy with exponential backoff for NDNSF service requests.

Reuses the existing ``FailureAction`` enum and ``ReplanRecord`` from
NDNSF-DI.  Applies retry logic at the user-driver level.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any, Callable


@dataclass
class RetryPolicy:
    max_attempts: int = 3
    base_backoff_ms: float = 100.0
    max_backoff_ms: float = 5000.0
    multiplier: float = 2.0
    jitter: float = 0.1  # 0.0 = no jitter, 0.1 = ±10%

    retry_on_timeout: bool = True
    retry_on_lease_rejected: bool = True
    retry_on_provider_busy: bool = True

    attempts: int = field(default=0, init=False)
    total_wait_ms: float = field(default=0.0, init=False)

    def should_retry(self, error: str, elapsed_ms: float) -> bool:
        if self.attempts >= self.max_attempts:
            return False
        if not error:
            return False
        lower = error.lower()
        if self.retry_on_timeout and "timeout" in lower:
            return True
        if self.retry_on_lease_rejected and any(
            kw in lower for kw in ("lease", "rejected", "expired", "not_found")
        ):
            return True
        if self.retry_on_provider_busy and any(
            kw in lower for kw in ("busy", "queue", "overload")
        ):
            return True
        return False

    def next_backoff_ms(self) -> float:
        base = min(self.max_backoff_ms,
                   self.base_backoff_ms * (self.multiplier ** self.attempts))
        if self.jitter > 0:
            import random
            j = base * self.jitter
            base = base + random.uniform(-j, j)
        return max(0.0, base)

    def wait_and_record(self) -> None:
        wait = self.next_backoff_ms()
        if wait > 0:
            time.sleep(wait / 1000.0)
        self.total_wait_ms += wait
        self.attempts += 1

    def reset(self) -> None:
        self.attempts = 0
        self.total_wait_ms = 0.0


def retry_call(fn: Callable[[], dict[str, Any]],
               policy: RetryPolicy | None = None,
               *,
               skip_retry_on: tuple[str, ...] = ()) -> dict[str, Any]:
    """Call ``fn`` with retry, returning the first successful result or the last failure.

    ``fn`` must return a dict with at least ``status`` and ``error`` keys.
    """
    p = policy or RetryPolicy()
    last: dict[str, Any] = {}
    while True:
        result = fn()
        status = str(result.get("status", ""))
        error = str(result.get("error", ""))
        elapsed = float(result.get("elapsedMs", 0.0))
        if status == "executed":
            result["retryAttempts"] = p.attempts
            result["retryTotalWaitMs"] = p.total_wait_ms
            return result
        skip = any(kw in error.lower() for kw in skip_retry_on)
        if skip or not p.should_retry(error, elapsed):
            result["retryAttempts"] = p.attempts
            result["retryTotalWaitMs"] = p.total_wait_ms
            return result
        p.wait_and_record()
        last = result
    return last
