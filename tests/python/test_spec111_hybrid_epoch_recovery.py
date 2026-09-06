from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
USER_SOURCE = ROOT / "ndn-service-framework/ServiceUser.cpp"


class HybridEpochRecoveryContractTest(unittest.TestCase):
    def test_group_control_messages_publish_epoch_key_once_and_attach_first_packet(self):
        text = USER_SOURCE.read_text(encoding="utf-8")
        start = text.index("void ServiceUser::publishHybridEncodedMessage(")
        end = text.index("bool ServiceUser::decryptHybridMessage(", start)
        body = text[start:end]
        self.assertIn("getWrappedSendKey", body)
        self.assertIn("cacheWrappedSendKey", body)
        # A new epoch carries the wrapped key on its first packet so a cold
        # receiver does not depend on a named-key fetch racing SVS delivery.
        # Later packets remain compact and recover the epoch key by name.
        self.assertEqual(body.count("envelope.setWrappedMessageKey"), 1)
        self.assertIn("wrappedKeyKnown", body)
        self.assertNotIn("markSendKeyWrapped", body)


if __name__ == "__main__":
    unittest.main()
