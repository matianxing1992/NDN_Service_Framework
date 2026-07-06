#!/usr/bin/env python3
"""Generic NDNSF coordination envelope tests."""

from __future__ import annotations

import unittest
from dataclasses import replace

from ndnsf.coordination import (
    COORDINATION_ADVISORY_SERVICE,
    CoordinationIntent,
    CoordinationRequest,
    CoordinationResponse,
    CoordinationServiceClient,
    CoordinationServiceProvider,
    CoordinationSuggestion,
    CoordinationWindow,
    decode_coordination_request,
    decode_coordination_response,
    encode_coordination_request,
    encode_coordination_response,
    coordination_suggestion_proof,
    verify_coordination_suggestion,
)


class CoreCoordinationTest(unittest.TestCase):
    def test_intent_validity_and_digest_are_service_neutral(self) -> None:
        intent = CoordinationIntent(
            intent_id="intent-1",
            request_id="req-1",
            requester_name="/user/A",
            service_name="/Any/Service",
            payload_schema="example-v1",
            payload={"applicationSpecific": True},
            expires_at_ms=2000,
        )

        self.assertTrue(intent.is_valid(now_ms_value=1000))
        self.assertFalse(intent.is_valid(now_ms_value=2000))
        self.assertEqual(len(intent.digest()), 24)

    def test_window_groups_intent_digests_without_service_semantics(self) -> None:
        intents = [
            CoordinationIntent(intent_id="i1", request_id="r1"),
            CoordinationIntent(intent_id="i2", request_id="r2"),
        ]

        window = CoordinationWindow.open(intents, now_ms_value=1000, window_ms=50)

        self.assertEqual(window.started_at_ms, 1000)
        self.assertEqual(window.closes_at_ms, 1050)
        self.assertEqual(len(window.window_id), 20)
        self.assertEqual(len(window.intents), 2)

    def test_dict_round_trip_keeps_payload_opaque(self) -> None:
        intent = CoordinationIntent.from_dict({
            "intentId": "intent-1",
            "requestId": "req-1",
            "requesterName": "/user/A",
            "serviceName": "/Any/Service",
            "payloadSchema": "custom-service-v1",
            "payload": {"serviceOwned": {"nested": True}},
            "unknownField": "ignored",
        })
        suggestion = CoordinationSuggestion.from_dict({
            "suggestionId": "suggestion-1",
            "intentId": "intent-1",
            "requestId": "req-1",
            "serviceName": "/Any/Service",
            "payloadSchema": "custom-suggestion-v1",
            "payload": {"providerHint": "/provider/A"},
            "unknownField": "ignored",
        })

        self.assertEqual(intent.payload_schema, "custom-service-v1")
        self.assertEqual(intent.payload, {"serviceOwned": {"nested": True}})
        self.assertFalse(hasattr(intent, "unknownField"))
        self.assertEqual(suggestion.payload_schema, "custom-suggestion-v1")
        self.assertEqual(suggestion.payload, {"providerHint": "/provider/A"})
        self.assertFalse(hasattr(suggestion, "unknownField"))

    def test_suggestion_proof_and_freshness(self) -> None:
        suggestion = CoordinationSuggestion(
            suggestion_id="suggestion-1",
            intent_id="intent-1",
            request_id="req-1",
            service_name="/Any/Service",
            coordinator_name="/coord",
            window_id="window-1",
            expires_at_ms=2000,
            payload_schema="example-suggestion-v1",
            payload={"hint": "provider-a"},
        )
        suggestion = replace(
            suggestion,
            proof=coordination_suggestion_proof(suggestion, secret="test-secret"),
        )

        self.assertEqual(
            verify_coordination_suggestion(
                suggestion,
                secret="test-secret",
                now_ms_value=1000,
            ),
            (True, "OK"),
        )
        self.assertEqual(
            verify_coordination_suggestion(
                replace(suggestion, proof="tampered"),
                secret="test-secret",
                now_ms_value=1000,
            ),
            (False, "COORDINATION_PROOF_INVALID"),
        )
        self.assertEqual(
            verify_coordination_suggestion(
                suggestion,
                secret="test-secret",
                now_ms_value=2000,
            ),
            (False, "STALE_SUGGESTION"),
        )

    def test_request_response_json_round_trip(self) -> None:
        request = CoordinationRequest((
            CoordinationIntent(
                intent_id="i1",
                request_id="r1",
                service_name="/Any/Service",
                payload_schema="custom-intent-v1",
                payload={"opaque": True},
            ),
        ))
        response = CoordinationResponse((
            CoordinationSuggestion(
                suggestion_id="s1",
                intent_id="i1",
                request_id="r1",
                service_name="/Any/Service",
                payload_schema="custom-suggestion-v1",
                payload={"hint": "/provider/A"},
            ),
        ))

        parsed_request = decode_coordination_request(encode_coordination_request(request))
        parsed_response = decode_coordination_response(encode_coordination_response(response))

        self.assertEqual(parsed_request.intents[0].payload, {"opaque": True})
        self.assertEqual(parsed_response.suggestions[0].payload, {"hint": "/provider/A"})

    def test_service_provider_and_client_use_ndnsf_service_shape(self) -> None:
        class FakeResponse:
            def __init__(self, status: bool, payload: bytes = b"", error: str = "") -> None:
                self.status = status
                self.payload = payload
                self.error = error

        class FakeProvider:
            def __init__(self) -> None:
                self.handlers = {}

            def add_handler(self, service: str, handler) -> None:
                self.handlers[service] = handler

        class FakeUser:
            def __init__(self, provider: FakeProvider) -> None:
                self.provider = provider
                self.calls = []

            def request_service(self, service: str, payload: bytes, *,
                                ack_timeout_ms: int, timeout_ms: int):
                self.calls.append({
                    "service": service,
                    "ackTimeoutMs": ack_timeout_ms,
                    "timeoutMs": timeout_ms,
                })
                return FakeResponse(True, self.provider.handlers[service](payload))

        provider = FakeProvider()

        def handle(request: CoordinationRequest) -> CoordinationResponse:
            intent = request.intents[0]
            return CoordinationResponse((
                CoordinationSuggestion(
                    suggestion_id="s-" + intent.intent_id,
                    intent_id=intent.intent_id,
                    request_id=intent.request_id,
                    service_name=intent.service_name,
                    payload_schema="custom-suggestion-v1",
                    payload={"hint": "/provider/A"},
                ),
            ))

        CoordinationServiceProvider(provider, handle).register()
        client = CoordinationServiceClient(
            FakeUser(provider),
            ack_timeout_ms=123,
            timeout_ms=456,
        )
        result = client.request([
            CoordinationIntent(
                intent_id="i1",
                request_id="r1",
                service_name="/Any/Service",
            )
        ])

        self.assertIn(COORDINATION_ADVISORY_SERVICE, provider.handlers)
        self.assertEqual(result.suggestions[0].payload["hint"], "/provider/A")


if __name__ == "__main__":
    unittest.main()
