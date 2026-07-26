from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ndn-service-framework/ServiceProvider.cpp"


def function_body(signature: str) -> str:
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class CollaborationScopeKeyFetchContractTest(unittest.TestCase):
    def test_fallback_uses_hybrid_large_data_contract_off_event_loop(self):
        body = function_body(
            "bool ServiceProvider::maybeFetchCollaborationScopeKey")

        self.assertIn("fetchAndDecryptLargeData", body)
        self.assertIn("std::thread", body)
        self.assertIn("boost::asio::post(m_face.getIoContext()", body)
        self.assertNotIn("nacConsumer.consume", body)

    def test_assignment_records_service_for_fallback_decryption(self):
        body = function_body(
            "void ServiceProvider::prepareCollaborationAssignmentAsync")

        self.assertIn("m_collaborationServiceNamesByRequest", body)
        self.assertIn("state->assignment.service", body)


if __name__ == "__main__":
    unittest.main()
