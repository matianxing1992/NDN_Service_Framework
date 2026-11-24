from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest

from ndnsf_distributed_inference.policy import generate_trust_schema


ROOT = Path(__file__).resolve().parents[2]


class Spec168MiniNdnSecurityPolicyTest(unittest.TestCase):

    def test_controller_issued_participant_certificates_are_trusted(self) -> None:
        schema = generate_trust_schema({
            "controller": "/example/llm-pipeline/controller",
            "trust": {"app_roots": ["/example/llm-pipeline"]},
        }, ())

        block_start = schema.index('id "NDN certificates"')
        block_end = schema.index("\n}\n", block_start)
        block = schema[block_start:block_end]
        controller_key = (
            'regex "^<example><llm-pipeline><controller><KEY><>{1,3}$"'
        )
        self.assertIn(controller_key, block)
        self.assertIn("type customized", block)
        self.assertIn("sig-type rsa-sha256", block)
        self.assertIn("sig-type ecdsa-sha256", block)
        self.assertLess(block.index("type customized"), block.index("type hierarchical"))

    def test_group_data_accepts_application_member_rsa_or_ecdsa_key(self) -> None:
        schema = generate_trust_schema({
            "trust": {"app_roots": ["/example/llm-pipeline"]},
        }, ())

        exact = "regex ^<example><llm-pipeline><group><>*$"
        self.assertIn(exact, schema)
        block_start = schema.index('id "NDN-SVS group data /example/llm-pipeline"')
        block_end = schema.index("\n}\n", block_start)
        block = schema[block_start:block_end]
        self.assertIn("sig-type rsa-sha256", block)
        self.assertIn("sig-type ecdsa-sha256", block)
        self.assertLess(
            block_start,
            schema.index('id "Application data /example/llm-pipeline"'),
        )


if __name__ == "__main__":
    unittest.main()
