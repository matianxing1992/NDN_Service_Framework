import copy
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

from tokenizers import Tokenizer
from tokenizers.models import WordLevel
from tokenizers.pre_tokenizers import Whitespace


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "build_spec175_workload.py"
WORKLOAD = (
    ROOT / "packaging" / "ndnsf-di-container" / "jobs" / "spec175"
    / "workload.json"
)


def _load_builder():
    spec = importlib.util.spec_from_file_location("spec175_workload", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load Spec175 workload builder")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec175WorkloadTest(unittest.TestCase):
    def _tokenizer_dir(self, root: Path) -> Path:
        directory = root / "tokenizer"
        directory.mkdir()
        template = (
            "<|im_start|>user\n{{ messages[0].content }}<|im_end|>\n"
            "<|im_start|>assistant\n"
            "{% if enable_thinking is defined and enable_thinking is false %}"
            "<think>\n\n</think>\n\n{% else %}<think>\n{% endif %}"
        )
        (directory / "chat_template.jinja").write_text(template)
        (directory / "tokenizer_config.json").write_text(json.dumps({
            "chat_template": template,
            "eos_token": "<|im_end|>",
            "pad_token": "<|endoftext|>",
            "tokenizer_class": "Qwen2Tokenizer",
        }))
        tokenizer = Tokenizer(WordLevel(
            vocab={
                "<unk>": 0,
                "<|im_start|>": 1,
                "<|im_end|>": 2,
                "<|endoftext|>": 3,
                "user": 4,
                "assistant": 5,
                "think": 6,
            },
            unk_token="<unk>",
        ))
        tokenizer.pre_tokenizer = Whitespace()
        tokenizer.save(str(directory / "tokenizer.json"))
        return directory

    def test_builder_is_deterministic_and_validator_rejects_mutations(self):
        module = _load_builder()
        with tempfile.TemporaryDirectory() as temporary:
            tokenizer_dir = self._tokenizer_dir(Path(temporary))
            first = module.build_workload(tokenizer_dir)
            second = module.build_workload(tokenizer_dir)
            self.assertEqual(first, second)
            module.validate_workload(first, tokenizer_dir)

            bad = copy.deepcopy(first)
            bad["prompts"][0]["inputTokenIds"][0] += 1
            with self.assertRaisesRegex(ValueError, "workload does not match"):
                module.validate_workload(bad, tokenizer_dir)

            bad = copy.deepcopy(first)
            bad["generation"]["thinkingMode"] = "enabled"
            with self.assertRaisesRegex(ValueError, "workload does not match"):
                module.validate_workload(bad, tokenizer_dir)

    def test_checked_in_workload_freezes_the_registered_subject(self):
        value = json.loads(WORKLOAD.read_text())
        self.assertEqual(value["schema"], "ndnsf-di-spec175-workload-v1")
        self.assertEqual(value["model"]["repository"], "Qwen/Qwen3.6-27B")
        self.assertEqual(
            value["model"]["revision"],
            "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        )
        self.assertEqual([item["id"] for item in value["prompts"]], ["P1", "P2"])
        self.assertEqual(value["generation"], {
            "doSample": False,
            "eosTokenIds": [248046, 248044],
            "maxEvents": 65,
            "maxGeneratedTokens": 64,
            "requestDeadlineMs": 120000,
            "samplingMode": "greedy",
            "thinkingMode": "disabled",
        })
        for item in value["prompts"]:
            self.assertEqual(item["inputTokenCount"], len(item["inputTokenIds"]))
            self.assertEqual(
                bytes.fromhex(item["canonicalMessageUtf8Hex"]).decode("utf-8"),
                item["message"]["content"],
            )


if __name__ == "__main__":
    unittest.main()
