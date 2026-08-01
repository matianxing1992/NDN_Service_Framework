"""Generic NDNSF progress admission with idle and absolute deadlines."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Callable, Optional


class DeadlineTerminal(str, Enum):
    COMPLETED = "COMPLETED"
    FAILED = "FAILED"
    CANCELLED = "CANCELLED"
    STALLED = "STALLED"
    HARD_TIMEOUT = "HARD_TIMEOUT"


@dataclass(frozen=True)
class ProgressObservation:
    request_id: str
    operation_id: str
    provider: str
    role: str
    attempt: int
    epoch: int
    sequence: int
    phase: str
    completed_work: int
    total_work: Optional[int]
    authenticated: bool
    observed_at: Optional[float] = None


@dataclass(frozen=True)
class ProgressDecision:
    admitted: bool
    renewed: bool
    reason: str
    idle_deadline: float
    hard_deadline: float


class DeadlineMonitor:
    """State machine whose clock and identity are fixed at construction."""

    def __init__(
        self,
        *,
        request_id: str,
        operation_id: str,
        provider: str,
        role: str,
        attempt: int,
        idle_budget: float,
        hard_budget: float,
        clock: Callable[[], float],
        phase_order: tuple[str, ...] = (),
    ) -> None:
        if idle_budget <= 0 or hard_budget <= 0:
            raise ValueError("deadline budgets must be positive")
        if idle_budget > hard_budget:
            raise ValueError("idle budget cannot exceed hard budget")
        self.request_id = request_id
        self.operation_id = operation_id
        self.provider = provider
        self.role = role
        self.attempt = attempt
        self.idle_budget = float(idle_budget)
        self._clock = clock
        self._phase_rank = {phase: index for index, phase in enumerate(phase_order)}
        self.started_at = float(clock())
        self.last_admitted_progress_at = self.started_at
        self.idle_deadline = self.started_at + self.idle_budget
        self.hard_deadline = self.started_at + float(hard_budget)
        self.terminal: Optional[DeadlineTerminal] = None
        self.terminal_at: Optional[float] = None
        self._last_epoch_sequence: Optional[tuple[int, int]] = None
        self._last_phase: Optional[str] = None
        self._last_completed = -1
        self._last_total: Optional[int] = None

    def _now(self, observation: Optional[ProgressObservation] = None) -> float:
        if observation is not None and observation.observed_at is not None:
            return float(observation.observed_at)
        return float(self._clock())

    def _expire(self, now: float) -> Optional[DeadlineTerminal]:
        if self.terminal is not None:
            return self.terminal
        if now >= self.hard_deadline:
            self.finish(DeadlineTerminal.HARD_TIMEOUT, now=now)
        elif now >= self.idle_deadline:
            self.finish(DeadlineTerminal.STALLED, now=now)
        return self.terminal

    def poll(self) -> Optional[DeadlineTerminal]:
        return self._expire(self._now())

    def finish(
        self, terminal: DeadlineTerminal, *, now: Optional[float] = None
    ) -> bool:
        if self.terminal is not None:
            return False
        current = self._now() if now is None else float(now)
        if current >= self.hard_deadline:
            terminal = DeadlineTerminal.HARD_TIMEOUT
        elif current >= self.idle_deadline and terminal not in {
            DeadlineTerminal.COMPLETED,
            DeadlineTerminal.FAILED,
            DeadlineTerminal.CANCELLED,
        }:
            terminal = DeadlineTerminal.STALLED
        self.terminal = terminal
        self.terminal_at = current
        return True

    def _reject(self, reason: str) -> ProgressDecision:
        return ProgressDecision(
            admitted=False,
            renewed=False,
            reason=reason,
            idle_deadline=self.idle_deadline,
            hard_deadline=self.hard_deadline,
        )

    def admit(self, observation: ProgressObservation) -> ProgressDecision:
        now = self._now(observation)
        if self._expire(now) is not None:
            return self._reject("post-terminal")
        if not observation.authenticated:
            return self._reject("unauthenticated")
        expected = (
            self.request_id,
            self.operation_id,
            self.provider,
            self.role,
            self.attempt,
        )
        actual = (
            observation.request_id,
            observation.operation_id,
            observation.provider,
            observation.role,
            observation.attempt,
        )
        if actual != expected:
            return self._reject("wrong-binding")
        if observation.epoch < 0 or observation.sequence < 0:
            return self._reject("invalid-position")
        position = (observation.epoch, observation.sequence)
        if (
            self._last_epoch_sequence is not None
            and position <= self._last_epoch_sequence
        ):
            return self._reject("duplicate-or-reordered")
        if observation.completed_work < 0:
            return self._reject("invalid-work")
        if observation.total_work is not None:
            if observation.total_work < observation.completed_work:
                return self._reject("invalid-total")
            if (
                self._last_total is not None
                and observation.total_work < self._last_total
            ):
                return self._reject("regressed-total")

        work_advanced = observation.completed_work > self._last_completed
        phase_advanced = False
        if self._last_phase is None:
            phase_advanced = bool(observation.phase)
        elif observation.phase != self._last_phase:
            if self._phase_rank:
                old_rank = self._phase_rank.get(self._last_phase)
                new_rank = self._phase_rank.get(observation.phase)
                phase_advanced = (
                    old_rank is not None
                    and new_rank is not None
                    and new_rank > old_rank
                )
            else:
                phase_advanced = bool(observation.phase)
        if not (work_advanced or phase_advanced):
            return self._reject("non-advancing")

        self._last_epoch_sequence = position
        self._last_phase = observation.phase
        self._last_completed = observation.completed_work
        self._last_total = observation.total_work
        self.last_admitted_progress_at = now
        self.idle_deadline = min(now + self.idle_budget, self.hard_deadline)
        return ProgressDecision(
            admitted=True,
            renewed=True,
            reason="advanced",
            idle_deadline=self.idle_deadline,
            hard_deadline=self.hard_deadline,
        )
