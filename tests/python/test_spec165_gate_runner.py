"""Quick-check declarations cannot authorize deployment."""

import unittest

from Experiments.NDNSF_Run_Minindn_Quick_Checks import checks as quick_checks, fidelity_declaration


class QuickCheckFidelityTests(unittest.TestCase):
    def test_legacy_quick_checks_are_explicitly_non_authorizing(self):
        declarations = [
            fidelity_declaration(item) for item in quick_checks().values()
        ]
        self.assertTrue(declarations)
        self.assertTrue(
            all(not item["deploymentAuthorizing"] for item in declarations)
        )
        self.assertNotIn(
            "REAL_MININDN_MODEL",
            {item["fidelityTier"] for item in declarations},
        )
