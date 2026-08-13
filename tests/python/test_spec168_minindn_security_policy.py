from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest

from ndnsf_distributed_inference.policy import generate_trust_schema


ROOT = Path(__file__).resolve().parents[2]
EXPERIMENT = ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"


def load_experiment():
    spec = importlib.util.spec_from_file_location(
        "spec168_minindn_experiment", EXPERIMENT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec168MiniNdnSecurityPolicyTest(unittest.TestCase):
    def test_minindn_hosts_receive_only_local_private_keys(self) -> None:
        source = EXPERIMENT.read_text(encoding="utf-8")
        self.assertIn("local_identities_by_node", source)
        self.assertIn("if owner_host == host_name", source)
        self.assertIn("for _, _, cert, _, _ in exported_keys", source)
        self.assertNotIn("for key, _ in exported_keys:", source)
        self.assertIn("SPEC168_LOCAL_PIB_CERTIFICATE_LOOKUP_MISS", source)
        self.assertIn("SPEC168_ACK_COVERAGE_CLOSURE_TOO_SLOW", source)
        self.assertIn("SPEC168_SECURITY_FIDELITY_PASS", source)

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

    def test_three_column_controller_bootstrap_token_is_accepted(self) -> None:
        experiment = load_experiment()
        with tempfile.TemporaryDirectory(prefix="spec168-token-") as temporary:
            root = Path(temporary)
            authority = root / "tokens.txt"
            authority.write_text(
                "# identity token role\n"
                "/example/llm-pipeline/provider secret-token provider\n",
                encoding="utf-8",
            )
            target = experiment.write_bootstrap_token(
                authority,
                "/example/llm-pipeline/provider",
                root / "provider.token",
            )

            self.assertEqual(target.read_text(encoding="utf-8"), "secret-token\n")
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)


if __name__ == "__main__":
    unittest.main()
