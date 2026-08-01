import unittest

from Experiments.ndnsf_validation.evidence import (
    EvidenceError,
    validate_generation_evidence,
)
from Experiments.ndnsf_validation.workload import canonical_workload


def workload():
    return canonical_workload(include_snapshot_manifest=False)


def rows(value):
    output = []
    model_digest = value["modelIdentity"]["contentDigest"]
    for prompt in value["prompts"]:
        for phase, count in (
            ("warmup", value["warmupPerPrompt"]),
            ("measured", value["measuredPerPrompt"]),
        ):
            for repetition in range(count):
                generation = f"campaign-{prompt['promptId']}-{phase}-{repetition}"
                steps = [
                    {
                        "requestId": f"{generation}-token-{index}",
                        "durationMs": 1.0,
                        "transport": {
                            "wireRequestId": f"{generation}-token-{index}",
                            "attempt": 1,
                            "planId": "plan-1",
                            "modelIdentityDigest": model_digest,
                        },
                    }
                    for index in range(8)
                ]
                output.append(
                    {
                        "schemaVersion": "ndnsf-di-qwen-generation-sample-v1",
                        "generationId": generation,
                        "promptId": prompt["promptId"],
                        "phase": phase,
                        "repetition": repetition,
                        "status": "OK",
                        "generatedTokenIds": list(range(8)),
                        "decodedText": "complete answer",
                        "ttftMs": 1.0,
                        "interTokenMs": [1.0] * 7,
                        "totalMs": 8.0,
                        "tokensPerSecond": 1000.0,
                        "tokenSteps": steps,
                        "modelIdentityDigest": model_digest,
                        "workloadDigest": value["workloadDigest"],
                        "planId": "plan-1",
                    }
                )
    return output


class GenerationEvidenceTests(unittest.TestCase):
    def test_complete_evidence_passes(self):
        value = workload()
        summary = validate_generation_evidence(rows(value), workload=value)
        self.assertEqual(summary["measuredCount"], 6)
        self.assertEqual(summary["minimumObservedTokens"], 8)

    def test_one_token_missing_metric_and_wrong_lineage_fail(self):
        value = workload()
        candidates = []
        one_token = rows(value)
        one_token[0]["generatedTokenIds"] = [1]
        one_token[0]["tokenSteps"] = one_token[0]["tokenSteps"][:1]
        one_token[0]["interTokenMs"] = []
        candidates.append(one_token)
        missing_answer = rows(value)
        missing_answer[0]["decodedText"] = ""
        candidates.append(missing_answer)
        wrong_request = rows(value)
        wrong_request[0]["tokenSteps"][0]["transport"]["wireRequestId"] = "other"
        candidates.append(wrong_request)
        for candidate in candidates:
            with self.subTest():
                with self.assertRaises(EvidenceError):
                    validate_generation_evidence(candidate, workload=value)


if __name__ == "__main__":
    unittest.main()
