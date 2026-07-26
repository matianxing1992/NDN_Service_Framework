from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
USER_SOURCE = ROOT / "ndn-service-framework/ServiceUser.cpp"


class HybridEpochRecoveryContractTest(unittest.TestCase):
    def test_group_control_messages_reuse_and_attach_cached_wrapped_key(self):
        text = USER_SOURCE.read_text(encoding="utf-8")
        start = text.index("void ServiceUser::publishHybridEncodedMessage(")
        end = text.index("bool ServiceUser::decryptHybridMessage(", start)
        body = text[start:end]
        self.assertIn("getWrappedSendKey", body)
        self.assertIn("cacheWrappedSendKey", body)
        self.assertGreaterEqual(body.count("envelope.setWrappedMessageKey"), 2)
        self.assertNotIn("markSendKeyWrapped", body)


if __name__ == "__main__":
    unittest.main()
