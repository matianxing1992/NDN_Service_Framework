import unittest
from types import SimpleNamespace

from ndnsf.service import CollaborationDeadlineExceeded, ServiceUser


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def snapshot(sequence, progress, state="RUNNING"):
    member = SimpleNamespace(
        role="stage-0",
        operation_id="prepare-1",
        attempt=1,
        epoch=1,
        sequence=sequence,
        state=SimpleNamespace(value=state),
        progress_known=True,
        progress=progress,
    )
    return (SimpleNamespace(provider_name="/provider/1", member_statuses=(member,)),)


class FakeUser:
    def __init__(self, values):
        self.values = iter(values)

    def collaboration_status(self, request_id, timeout_ms):
        try:
            return next(self.values)
        except StopIteration:
            return ()


class CollaborationWatchTests(unittest.TestCase):
    def collect(self, fake, clock, **timeouts):
        return list(
            ServiceUser.watch_collaboration_request(
                fake,
                "request-1",
                query_interval_ms=1000,
                _clock=clock,
                _sleep=clock.sleep,
                **timeouts,
            )
        )

    def test_advancing_progress_outlives_original_idle_window(self):
        clock = FakeClock()
        fake = FakeUser(
            [
                snapshot(1, 0.1),
                snapshot(2, 0.2),
                snapshot(3, 0.3, "COMPLETED"),
                (),
            ]
        )
        with self.assertRaises(CollaborationDeadlineExceeded) as raised:
            self.collect(
                fake,
                clock,
                timeout_ms=5000,
                idle_timeout_ms=1500,
                hard_timeout_ms=5000,
            )
        self.assertEqual(raised.exception.reason, "STALLED")
        self.assertGreater(clock.now, 1.5)

    def test_duplicate_status_does_not_renew_idle(self):
        clock = FakeClock()
        first = snapshot(1, 0.1)
        fake = FakeUser([first, first, first])
        with self.assertRaises(CollaborationDeadlineExceeded) as raised:
            self.collect(
                fake,
                clock,
                timeout_ms=5000,
                idle_timeout_ms=1500,
                hard_timeout_ms=5000,
            )
        self.assertEqual(raised.exception.reason, "STALLED")
        self.assertEqual(clock.now, 1.5)

    def test_continuous_progress_cannot_extend_hard_deadline(self):
        clock = FakeClock()
        fake = FakeUser(
            [snapshot(index, index / 10.0) for index in range(1, 10)]
        )
        with self.assertRaises(CollaborationDeadlineExceeded) as raised:
            self.collect(
                fake,
                clock,
                timeout_ms=3000,
                idle_timeout_ms=1500,
                hard_timeout_ms=3000,
            )
        self.assertEqual(raised.exception.reason, "HARD_TIMEOUT")
        self.assertEqual(clock.now, 3.0)

    def test_same_version_changed_content_is_rejected(self):
        clock = FakeClock()
        fake = FakeUser([snapshot(1, 0.1), snapshot(1, 0.2)])
        with self.assertRaisesRegex(ValueError, "equivocation"):
            self.collect(
                fake,
                clock,
                timeout_ms=5000,
                idle_timeout_ms=2000,
                hard_timeout_ms=5000,
            )


if __name__ == "__main__":
    unittest.main()
