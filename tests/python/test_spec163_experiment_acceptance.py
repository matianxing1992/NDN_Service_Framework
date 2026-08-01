from __future__ import annotations

import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from Experiments.NDNSF_DI_PlacementPreparation_Minindn import validate_answer


class Spec163ExperimentAcceptanceTest(unittest.TestCase):
    def test_semantically_complete_answers_pass(self):
        answers = {
            "ndn-vs-ip": "NDN 按内容名称转发，IP 按主机地址转发。",
            "identity-binding": "内容哈希与语义哈希必须同时绑定。",
            "pipeline-vs-tensor": (
                "Pipeline uses layer stages; tensor parallelism splits tensor "
                "operations within a layer."),
            "stage-timeout": (
                "标记本次尝试失败，取消并隔离旧消息，在截止时间内重试或重规划。"),
            "evidence-summary": (
                "This run confirms the observations, but does not confirm "
                "universal correctness or optimality."),
        }
        for prompt_id, answer in answers.items():
            with self.subTest(prompt_id=prompt_id):
                self.assertTrue(validate_answer(prompt_id, answer)[0])

    def test_consistent_but_wrong_or_unsafe_answers_fail(self):
        rejected = {
            "pipeline-vs-tensor": (
                "Pipeline is sequential and tensor parallelism uses nodes."),
            "stage-timeout": "等待恢复，然后假设请求成功。",
            "evidence-summary": (
                "It does not prove that hidden states were present."),
        }
        for prompt_id, answer in rejected.items():
            with self.subTest(prompt_id=prompt_id):
                self.assertFalse(validate_answer(prompt_id, answer)[0])


if __name__ == "__main__":
    unittest.main()
