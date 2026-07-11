"""DI-owned bounded retry helpers for idempotent user-driver operations."""

from __future__ import annotations

import random
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable


class RetryReason(str, Enum):
    TIMEOUT = "TIMEOUT"
    LEASE_REJECTED = "LEASE_REJECTED"
    LEASE_EXPIRED = "LEASE_EXPIRED"
    PROVIDER_BUSY = "PROVIDER_BUSY"
    OVERLOADED = "OVERLOADED"
    NON_RETRYABLE = "NON_RETRYABLE"
    UNKNOWN = "UNKNOWN"

    @classmethod
    def parse(cls, value: object) -> "RetryReason":
        try:
            return cls(str(value or "").strip().upper())
        except ValueError:
            return cls.UNKNOWN


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

    def should_retry(self, reason: RetryReason | str, *, idempotent: bool) -> bool:
        parsed = reason if isinstance(reason, RetryReason) else RetryReason.parse(reason)
        if not idempotent or self.attempts >= self.max_attempts:
            return False
        return bool(
            (self.retry_on_timeout and parsed == RetryReason.TIMEOUT)
            or (self.retry_on_lease_rejected and parsed in {
                RetryReason.LEASE_REJECTED,
                RetryReason.LEASE_EXPIRED,
            })
            or (self.retry_on_provider_busy and parsed in {
                RetryReason.PROVIDER_BUSY,
                RetryReason.OVERLOADED,
            })
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
    idempotent: bool = False,
    reason_getter: Callable[[dict[str, Any]], RetryReason | str] | None = None,
    skip_retry_on: tuple[str, ...] = (),
) -> dict[str, Any]:
    """Retry an idempotent DI operation and retain deterministic diagnostics."""

    current = policy or RetryPolicy()
    while True:
        result = fn()
        status = str(result.get("status", ""))
        error = str(result.get("error", ""))
        if status == "executed" or any(word in error.lower() for word in skip_retry_on):
            result["retryAttempts"] = current.attempts
            result["retryTotalWaitMs"] = current.total_wait_ms
            return result
        reason = (
            reason_getter(result)
            if reason_getter is not None
            else RetryReason.parse(result.get("retryReason", result.get("reasonCode", "")))
        )
        if not current.should_retry(reason, idempotent=idempotent):
            result["retryAttempts"] = current.attempts
            result["retryTotalWaitMs"] = current.total_wait_ms
            result["retryReason"] = RetryReason.parse(reason).value
            return result
        current.wait_and_record()
