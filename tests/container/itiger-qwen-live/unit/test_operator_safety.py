from __future__ import annotations

import unittest

from _support import load_tool


operator = load_tool("spec110_operator")


class OperatorSafetyTests(unittest.TestCase):
    def test_credential_fields_are_rejected(self):
        for field in ("password", "mfaCode", "privateKey", "accessToken", "registry_token"):
            with self.subTest(field=field):
                with self.assertRaisesRegex(operator.OperatorSafetyError, "OPERATOR_CREDENTIAL_FIELD_FORBIDDEN"):
                    operator.validate_safe_document({field: "do-not-store"})

    def test_command_profile_injection_is_rejected(self):
        for value in ("bigTiger;id", "normal\n#SBATCH --mail-user=x", "gpu:3$(id)"):
            with self.subTest(value=value):
                with self.assertRaisesRegex(operator.OperatorSafetyError, "OPERATOR_(COMMAND_INJECTION|MULTILINE_VALUE_FORBIDDEN)"):
                    operator.validate_safe_document({"partition": value})

    def test_evidence_token_verification_fields_are_not_secrets(self):
        operator.validate_safe_document({"userTokenVerified": True, "providerTokenVerified": True, "tokenizerDigest": "sha256:" + "a" * 64})


if __name__ == "__main__":
    unittest.main()
