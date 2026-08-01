import unittest

from Experiments.ndnsf_validation.deadlines import (
    DeadlineMonitor,
    DeadlineTerminal,
    ProgressObservation,
)


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


def progress(clock, **updates):
    values = dict(
        request_id="request-1",
        operation_id="prepare-1",
        provider="/provider/1",
        role="stage-0",
        attempt=1,
        epoch=1,
        sequence=1,
        phase="FETCHING",
        completed_work=1,
        total_work=10,
        authenticated=True,
        observed_at=clock.now,
    )
    values.update(updates)
    return ProgressObservation(**values)


class DeadlineTests(unittest.TestCase):
    def monitor(self, clock):
        return DeadlineMonitor(
            request_id="request-1",
            operation_id="prepare-1",
            provider="/provider/1",
            role="stage-0",
            attempt=1,
            idle_budget=10,
            hard_budget=30,
            clock=clock,
            phase_order=("FETCHING", "LOADING", "READY"),
        )

    def test_advancing_progress_renews_idle_but_not_hard(self):
        clock = FakeClock()
        monitor = self.monitor(clock)
        hard = monitor.hard_deadline
        clock.advance(9)
        decision = monitor.admit(progress(clock))
        self.assertTrue(decision.renewed)
        self.assertEqual(monitor.idle_deadline, 19)
        self.assertEqual(monitor.hard_deadline, hard)

    def test_duplicate_forgery_and_wrong_binding_do_not_renew(self):
        clock = FakeClock()
        monitor = self.monitor(clock)
        monitor.admit(progress(clock))
        deadline = monitor.idle_deadline
        rejected = (
            progress(clock),
            progress(clock, sequence=2, authenticated=False, completed_work=2),
            progress(clock, sequence=3, request_id="other", completed_work=3),
        )
        for item in rejected:
            self.assertFalse(monitor.admit(item).renewed)
            self.assertEqual(monitor.idle_deadline, deadline)

    def test_reorder_nonadvance_and_bad_total_do_not_renew(self):
        clock = FakeClock()
        monitor = self.monitor(clock)
        self.assertTrue(monitor.admit(progress(clock, sequence=2)).admitted)
        deadline = monitor.idle_deadline
        rejected = (
            progress(clock, sequence=1, completed_work=2),
            progress(clock, sequence=3, completed_work=1),
            progress(clock, sequence=4, completed_work=11, total_work=10),
        )
        for item in rejected:
            self.assertFalse(monitor.admit(item).admitted)
            self.assertEqual(monitor.idle_deadline, deadline)

    def test_stall_and_hard_timeout_are_distinct(self):
        stalled_clock = FakeClock()
        stalled = self.monitor(stalled_clock)
        stalled_clock.advance(10)
        self.assertEqual(stalled.poll(), DeadlineTerminal.STALLED)

        hard_clock = FakeClock()
        hard = self.monitor(hard_clock)
        for sequence, now in enumerate((9, 18, 27), start=1):
            hard_clock.now = now
            hard.admit(
                progress(
                    hard_clock,
                    sequence=sequence,
                    completed_work=sequence,
                )
            )
        hard_clock.now = 30
        self.assertEqual(hard.poll(), DeadlineTerminal.HARD_TIMEOUT)

    def test_first_terminal_wins_and_post_terminal_is_rejected(self):
        clock = FakeClock()
        monitor = self.monitor(clock)
        self.assertTrue(monitor.finish(DeadlineTerminal.CANCELLED))
        self.assertFalse(monitor.finish(DeadlineTerminal.COMPLETED))
        self.assertEqual(monitor.terminal, DeadlineTerminal.CANCELLED)
        self.assertEqual(monitor.admit(progress(clock)).reason, "post-terminal")

    def test_hard_deadline_wins_at_equal_boundary(self):
        clock = FakeClock()
        monitor = DeadlineMonitor(
            request_id="request-1",
            operation_id="prepare-1",
            provider="/provider/1",
            role="stage-0",
            attempt=1,
            idle_budget=10,
            hard_budget=10,
            clock=clock,
        )
        clock.advance(10)
        self.assertEqual(monitor.poll(), DeadlineTerminal.HARD_TIMEOUT)


if __name__ == "__main__":
    unittest.main()
