"""Optional off-path observer delivery with idempotency and isolation."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, TimeoutError
from threading import Lock
from typing import Any


class ObserverRegistry:
    def __init__(self) -> None:
        self._observers: dict[str, Any] = {}
        self._delivered: set[tuple[str, str]] = set()
        self._lock = Lock()

    def register(self, observer: Any) -> None:
        name = str(getattr(observer, "name", ""))
        version = str(getattr(observer, "version", ""))
        if not name or not version or not callable(getattr(observer, "observe", None)):
            raise ValueError("invalid optimization observer")
        if name in self._observers:
            raise ValueError("duplicate optimization observer")
        self._observers[name] = observer

    def deliver(self, outcome: Any, idempotency_key: str,
                timeout_ms: int = 100) -> dict[str, str]:
        statuses: dict[str, str] = {}
        for name, observer in self._observers.items():
            key = (name, idempotency_key)
            with self._lock:
                if key in self._delivered:
                    statuses[name] = "duplicate"
                    continue
                self._delivered.add(key)
            pool = ThreadPoolExecutor(max_workers=1)
            future = pool.submit(observer.observe, outcome, idempotency_key)
            try:
                future.result(timeout=max(1, timeout_ms) / 1000.0)
                statuses[name] = "delivered"
            except TimeoutError:
                statuses[name] = "timeout"
            except Exception:
                statuses[name] = "failed"
            finally:
                pool.shutdown(wait=False)
        return statuses


__all__ = ["ObserverRegistry"]
