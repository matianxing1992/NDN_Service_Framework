"""DI-owned bounded retry helpers for idempotent user-driver operations."""

from __future__ import annotations

import random
import time
from dataclasses import dataclass, field
from typing import Any, Callable


@dataclass
class RetryPolicy:
    max_attempts: int = 3
    base_backoff_ms: float = 100.0
    max_backoff_ms: float = 5000.0
    multiplier: float = 2.0
    jitter: float = 0.1
    retry_on_timeout: bool = True
    retry_on_lease_rejected: bool = True
    retry_on_provider_busy: bool = True
    attempts: int = field(default=0, init=False)
    total_wait_ms: float = field(default=0.0, init=False)

    def should_retry(self, error: str, elapsed_ms: float) -> bool:
        del elapsed_ms
        if self.attempts >= self.max_attempts or not error:
            return False
        lower = error.lower()
        return bool(
            (self.retry_on_timeout and "timeout" in lower)
            or (
                self.retry_on_lease_rejected
                and any(word in lower for word in ("lease", "rejected", "expired", "not_found"))
            )
            or (
                self.retry_on_provider_busy
                and any(word in lower for word in ("busy", "queue", "overload"))
            )
        )

    def next_backoff_ms(self) -> float:
        base = min(
            self.max_backoff_ms,
            self.base_backoff_ms * (self.multiplier ** self.attempts),
        )
        if self.jitter > 0:
            spread = base * self.jitter
            base += random.uniform(-spread, spread)
        return max(0.0, base)

    def wait_and_record(self) -> None:
        wait_ms = self.next_backoff_ms()
        if wait_ms > 0:
            time.sleep(wait_ms / 1000.0)
        self.total_wait_ms += wait_ms
        self.attempts += 1

    def reset(self) -> None:
        self.attempts = 0
        self.total_wait_ms = 0.0


def retry_call(
    fn: Callable[[], dict[str, Any]],
    policy: RetryPolicy | None = None,
    *,
    skip_retry_on: tuple[str, ...] = (),
) -> dict[str, Any]:
    """Retry an idempotent DI operation and retain deterministic diagnostics."""

    current = policy or RetryPolicy()
    while True:
        result = fn()
        status = str(result.get("status", ""))
        error = str(result.get("error", ""))
        elapsed_ms = float(result.get("elapsedMs", 0.0))
        if status == "executed" or any(word in error.lower() for word in skip_retry_on):
            result["retryAttempts"] = current.attempts
            result["retryTotalWaitMs"] = current.total_wait_ms
            return result
        if not current.should_retry(error, elapsed_ms):
            result["retryAttempts"] = current.attempts
            result["retryTotalWaitMs"] = current.total_wait_ms
            return result
        current.wait_and_record()
