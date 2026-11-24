from __future__ import annotations

import sys
from pathlib import Path
import types
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.provider import (  # noqa: E402
    ProviderRuntimeContext,
    _assignment_deadline_ms,
)


class ProviderDependencyDeadlineTest(unittest.TestCase):
    def _context(self, *, deadline_ms: int = 0) -> ProviderRuntimeContext:
        execution = types.SimpleNamespace(
            spec=types.SimpleNamespace(metadata={}),
        )
        return ProviderRuntimeContext(
            ndnsf=types.SimpleNamespace(),
            execution=execution,
            request=b"request",
            role="/LLM/Pipeline/Stage/1",
            deadline_ms=deadline_ms,
        )

    def test_v2_deadline_controls_remaining_dependency_timeout(self) -> None:
        context = self._context(deadline_ms=11000)
        self.assertEqual(
            context.remaining_deadline_ms(now_ms=1000, safety_margin_ms=1000),
            9000,
        )

    def test_expired_v2_deadline_fails_closed(self) -> None:
        context = self._context(deadline_ms=1000)
        with self.assertRaisesRegex(TimeoutError, "deadline expired"):
            context.dependency_timeout_ms()

    def test_legacy_assignment_metadata_remains_bounded(self) -> None:
        self.assertEqual(
            _assignment_deadline_ms(
                b"role=/LLM/Pipeline/Stage/1;executionDeadlineMs=9876;"),
            9876,
        )
        self.assertEqual(_assignment_deadline_ms(b"not-an-assignment"), 0)


if __name__ == "__main__":
    unittest.main()
