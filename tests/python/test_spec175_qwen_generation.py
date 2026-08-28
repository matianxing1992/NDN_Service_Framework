from __future__ import annotations

import sys
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.adapters.qwen.generation import (  # noqa: E402
    GenerationContractError,
    GenerationTokenEventV1,
    IncrementalDetokenizer,
    SamplingConfig,
    accepted_prefix_digest,
    sample_token,
)
from ndnsf_distributed_inference.adapters.qwen.tokenizer import (  # noqa: E402
    StandaloneQwenTokenizer,
)
from ndnsf_distributed_inference.core.contracts import (  # noqa: E402
    DIRequestEnvelopeV2,
    GenerationRecoveryV1,
)


class Spec175QwenGenerationTests(unittest.TestCase):
    def test_greedy_is_exact_and_repetition_penalty_is_sealed(self) -> None:
        config = SamplingConfig(repetition_penalty=2.0)
        self.assertEqual(sample_token([0.1, 0.2, 0.9], config), 2)
        self.assertEqual(sample_token([0.1, 0.9, 0.8], config,
                                      generated=[1]), 2)
        with self.assertRaises(GenerationContractError):
            SamplingConfig(mode="Greedy", temperature=0.5).validate(3)

    def test_seeded_top_k_top_p_is_repeatable(self) -> None:
        config = SamplingConfig(mode="SeededTopKTopP", temperature=0.8,
                                top_k=3, top_p=0.9, seed=1750001)
        first = [sample_token([4.0, 3.0, 2.0, 1.0], config, step=i)
                 for i in range(8)]
        second = [sample_token([4.0, 3.0, 2.0, 1.0], config, step=i)
                  for i in range(8)]
        self.assertEqual(first, second)
        self.assertTrue(all(0 <= token < 3 for token in first))

    def test_incremental_detokenization_handles_unicode_empty_delta_and_stop(self) -> None:
        pieces = {1: "你", 2: "好", 3: "", 4: "🙂", 5: "<eos>"}

        def decode(ids):
            return "".join(pieces[int(item)] for item in ids)

        decoder = IncrementalDetokenizer(decode, stop_strings=("好🙂",))
        self.assertEqual(decoder.push(1), "你")
        self.assertEqual(decoder.push(2), "好")
        self.assertEqual(decoder.push(3), "")
        self.assertEqual(decoder.push(4), "🙂")
        self.assertEqual(decoder.matched_stop, "好🙂")
        with self.assertRaises(GenerationContractError):
            decoder.push(5)

    def test_event_and_prefix_digest_are_bound(self) -> None:
        digest = accepted_prefix_digest((4, 5, 6))
        event = GenerationTokenEventV1("/request/1", 1, 6, "x", digest)
        self.assertEqual(event.accepted_prefix_digest, digest)
        with self.assertRaises(GenerationContractError):
            GenerationTokenEventV1("/request/1", 0, 6, "x", digest)

    def test_attempt_two_recovery_seals_exact_committed_prefix(self) -> None:
        prefix = (4, 5, 6)
        manifest_digest = "sha256:" + "1" * 64
        recovery = GenerationRecoveryV1(
            logical_generation_id="ab" * 16,
            original_request_id="/request/one",
            recovery_request_id="/request/two",
            attempt=2,
            original_input_manifest_digest=manifest_digest,
            prior_plan_digest="sha256:" + "2" * 64,
            failed_provider="/provider/failed",
            committed_token_ids=prefix,
            committed_token_count=len(prefix),
            committed_prefix_digest=accepted_prefix_digest(prefix),
        )
        request = DIRequestEnvelopeV2(
            invocation_id="invocation:logical",
            request_id="/request/two",
            attempt=2,
            service="/LLM/Test",
            model_name="Qwen",
            model_identity_hash="sha256:" + "3" * 64,
            task_kind="generation",
            input_manifest_digest=manifest_digest,
            input_payload_b64="",
            options_payload_b64="",
            plan_deadline_ms=9999999999999,
            security_domain="tenant",
            task={
                "name": "generation",
                "generation_recovery": recovery.to_dict(),
            },
        )
        decoded = DIRequestEnvelopeV2.from_bytes(request.to_bytes())
        self.assertEqual(decoded.attempt, 2)
        self.assertEqual(
            decoded.task["generation_recovery"]["committed_token_ids"],
            [4, 5, 6],
        )
        with self.assertRaisesRegex(ValueError, "GenerationRecoveryV1"):
            GenerationRecoveryV1(
                **{**recovery.__dict__, "committed_token_ids": (4, 5, 7)}
            )

    def test_standalone_tokenizers_boundary_is_digest_bound(self) -> None:
        tokenizers = __import__("tokenizers")
        tokenizer = tokenizers.Tokenizer(
            tokenizers.models.WordLevel(
                {"[UNK]": 0, "hello": 1, "world": 2}, unk_token="[UNK]"))
        tokenizer.pre_tokenizer = tokenizers.pre_tokenizers.Whitespace()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tokenizer.json"
            tokenizer.save(str(path))
            value = StandaloneQwenTokenizer.from_file(path)
            self.assertEqual(value.encode("hello world"), (1, 2))
            self.assertEqual(value.decode((1, 2)), "hello world")
            with self.assertRaises(ValueError):
                StandaloneQwenTokenizer.from_file(path, expected_digest="sha256:" + "0" * 64)

    def test_deployed_generation_modules_do_not_import_transformers(self) -> None:
        source = (ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen").glob("*.py")
        combined = "\n".join(path.read_text(encoding="utf-8") for path in source)
        self.assertNotIn("import transformers", combined)
        self.assertNotIn("import torch", combined)


if __name__ == "__main__":
    unittest.main()
