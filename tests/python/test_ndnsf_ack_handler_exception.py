#!/usr/bin/env python3
"""Python ACK exceptions must be visible negative decisions, never silence."""

from __future__ import annotations

from contextlib import redirect_stdout
import io
import unittest

from ndnsf import ServiceProvider


class _NativeProvider:
    def __init__(self) -> None:
        self.ack_handler = None

    def add_collaboration_service(
        self, _service, _roles, _request_handler, ack_handler, _include_context,
    ) -> None:
        self.ack_handler = ack_handler


class AckHandlerExceptionTest(unittest.TestCase):
    def test_collaboration_ack_exception_becomes_visible_internal_error(self) -> None:
        native = _NativeProvider()
        provider = ServiceProvider.__new__(ServiceProvider)
        provider._native = native
        provider._collaboration_services = set()

        def broken_ack(_payload):
            raise ValueError("malformed\nrequest")

        output = io.StringIO()
        with redirect_stdout(output):
            provider.add_collaboration_handler(
                "/Inference/Test",
                ["/Stage/0"],
                lambda _ctx, _payload: None,
                broken_ack,
            )
            decision = native.ack_handler(b"opaque-request")

        self.assertFalse(decision.status)
        self.assertFalse(decision.suppress)
        self.assertEqual("INTERNAL_ERROR", decision.message)
        marker = output.getvalue()
        self.assertIn("NDNSF_ACK_HANDLER_EXCEPTION", marker)
        self.assertIn("service=/Inference/Test", marker)
        self.assertIn("errorType=ValueError", marker)
        self.assertIn("detail=malformed request", marker)
        self.assertNotIn("opaque-request", marker)


if __name__ == "__main__":
    unittest.main()
