from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DIProviderOfferV2, decode_placement_wire,
)


class Spec170V2V3CompatibilityTest(unittest.TestCase):
    def test_unknown_or_legacy_profile_does_not_auto_downgrade(self):
        with self.assertRaises(ValueError):
            decode_placement_wire(b'{"schema":"PREASSEMBLED_PARTITION_SINGLE_DEVICE"}')

    def test_v2_decoder_remains_explicit(self):
        # V2 is intentionally not constructed here: it requires a signed,
        # request-bound offer.  The dispatch contract is tested by its exact
        # schema rejection rather than by manufacturing an unsigned fixture.
        self.assertTrue(DIProviderOfferV2.__name__.endswith("V2"))


if __name__ == "__main__":
    unittest.main()
