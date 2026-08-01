from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
JOBS = ROOT / "specs/162-itiger-qwen36-generation/jobs"
PREPARE = JOBS / "prepare-qwen36.py"
PREPARE_REFERENCE = JOBS / "prepare-reference.sbatch"
PREPARE_STAGES = JOBS / "prepare-stages.sh"
CAPACITY = JOBS / "capacity-preflight.py"
GENERATION_SMOKE = JOBS / "generation-smoke.sbatch"
GENERATION_RANK = JOBS / "generation-rank.sh"
GENERATION_RANK_INNER = JOBS / "generation-rank-inner.sh"
SMOKE_ANALYZER = JOBS / "analyze-generation-smoke.py"
FORMAL_ANALYZER = JOBS / "analyze-generation-formal.py"
QWEN_USER = (
    ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py"
)
QWEN_PIPELINE = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py"
)


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class Qwen36PreparationContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.prepare = load_module("spec162_prepare_qwen36", PREPARE)
        cls.capacity = load_module("spec162_capacity_preflight", CAPACITY)

    def test_frozen_model_and_stage_ranges(self) -> None:
        self.assertEqual(self.prepare.MODEL_REPOSITORY, "Qwen/Qwen3.6-27B")
        self.assertEqual(
            self.prepare.MODEL_REVISION,
            "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        )
        self.assertEqual(self.prepare.DTYPE, "bfloat16")
        self.assertEqual(self.prepare.STAGE_RANGES, ((0, 21), (21, 42), (42, 64)))
        self.assertEqual(self.prepare.MAX_NEW_TOKENS, 64)

        source = PREPARE.read_text(encoding="utf-8")
        self.assertIn('"warmupPerPrompt": 0', source)
        self.assertIn('"measuredPerPrompt": 1', source)
        self.assertIn('"prompts": [reference_cases[0]]', source)
        self.assertIn('"enableThinking": False', source)
        self.assertIn('"requireEos": False', source)
        self.assertNotIn('"requireEos": True', source)
        self.assertIn(
            '"EOS" if reached_eos else "MAX_NEW_TOKENS"',
            source,
        )
        self.assertNotIn("did not reach EOS within", source)
        self.assertIn("gc.collect()", source)
        self.assertIn("SPEC162_REFERENCE_CUDA_RELEASED", source)
        self.assertIn("reference model CUDA release incomplete", source)
        self.assertIn('sort_keys=True).encode("utf-8")', source)
        self.assertIn('role=str(row["role"])', source)
        self.assertIn("stages=STAGE_COUNT", source)
        self.assertIn('"inputIds": [validation_ids]', source)
        self.assertIn('key=lambda item: int(item["inputTokenCount"])', source)
        self.assertIn('"inputTokenCount": validation_input_token_count', source)
        user_source = QWEN_USER.read_text(encoding="utf-8")
        pipeline_source = QWEN_PIPELINE.read_text(encoding="utf-8")
        self.assertIn(
            'require_eos = generation.get("requireEos", True)',
            user_source,
        )
        self.assertIn("require_eos=require_eos", user_source)
        self.assertIn('stop_reason = "MAX_NEW_TOKENS"', pipeline_source)

    def test_reference_requires_three_visible_rtx5000_gpus(self) -> None:
        accepted = self.prepare.validate_reference_gpus([
            {"index": 0, "name": "NVIDIA RTX 5000 Ada Generation",
             "totalBytes": 32_760 * 1024 * 1024},
            {"index": 1, "name": "NVIDIA RTX 5000 Ada Generation",
             "totalBytes": 32_760 * 1024 * 1024},
            {"index": 2, "name": "NVIDIA RTX 5000 Ada Generation",
             "totalBytes": 32_760 * 1024 * 1024},
        ])
        self.assertEqual([row["index"] for row in accepted], [0, 1, 2])

        with self.assertRaisesRegex(RuntimeError, "exactly three"):
            self.prepare.validate_reference_gpus(accepted[:2])
        with self.assertRaisesRegex(RuntimeError, "RTX 5000"):
            self.prepare.validate_reference_gpus([
                accepted[0],
                accepted[1],
                {**accepted[2], "name": "NVIDIA H100 80GB HBM3"},
            ])

    def test_reference_device_validation_uses_actual_cuda_placement_when_metadata_absent(
            self) -> None:
        class Device:
            def __init__(self, device_type: str, index: int | None):
                self.type = device_type
                self.index = index

            def __str__(self) -> str:
                return (
                    self.type if self.index is None
                    else f"{self.type}:{self.index}"
                )

        class Tensor:
            def __init__(self, device_type: str, index: int | None):
                self.device = Device(device_type, index)

        class Model:
            def __init__(self, devices):
                self._parameters = [
                    Tensor(device_type, index)
                    for device_type, index in devices
                ]

            def parameters(self):
                return iter(self._parameters)

            def buffers(self):
                return iter(())

        observed = self.prepare._assert_reference_device_map(
            Model([("cuda", 0), ("cuda", 0)]),
            expected_devices={0},
        )
        self.assertEqual(set(observed.values()), {0})

        with self.assertRaisesRegex(RuntimeError, "non-CUDA"):
            self.prepare._assert_reference_device_map(
                Model([("cuda", 0), ("cpu", None)]),
                expected_devices={0},
            )

    def test_stage_acceptance_requires_forward_and_one_gib_headroom(self) -> None:
        total = 32_760 * 1024 * 1024
        rows = [
            {
                "stageIndex": index,
                "layerRange": {"start": start, "endExclusive": end},
                "cudaValidated": True,
                "forwardValidated": True,
                "cpuFallback": False,
                "peakAllocatedBytes": total - 2 * 1024**3,
                "peakReservedBytes": total - 1024**3,
                "gpuTotalBytes": total,
            }
            for index, (start, end) in enumerate(self.prepare.STAGE_RANGES)
        ]
        for row in rows:
            row["validationInputTokenCount"] = 12
        self.prepare.validate_stage_acceptance(
            rows, validation_input_token_count=12)

        rows[1]["peakReservedBytes"] = total - 1024**3 + 1
        with self.assertRaisesRegex(RuntimeError, "headroom"):
            self.prepare.validate_stage_acceptance(
                rows, validation_input_token_count=12)

    def test_stage_acceptance_rejects_short_validation(self) -> None:
        total = 32_760 * 1024 * 1024
        rows = [
            {
                "stageIndex": index,
                "layerRange": {"start": start, "endExclusive": end},
                "cudaValidated": True,
                "forwardValidated": True,
                "cpuFallback": False,
                "validationInputTokenCount": 8,
                "peakAllocatedBytes": total - 2 * 1024**3,
                "peakReservedBytes": total - 1024**3,
                "gpuTotalBytes": total,
            }
            for index, (start, end) in enumerate(self.prepare.STAGE_RANGES)
        ]
        with self.assertRaisesRegex(RuntimeError, "validated with 8 tokens"):
            self.prepare.validate_stage_acceptance(
                rows, validation_input_token_count=37)

    def test_capacity_is_fail_closed_without_authorities(self) -> None:
        decision = self.capacity.evaluate_capacity({
            "currentUsageBytes": 67 * 1024**3,
            "verifiedQuotaBytes": 0,
            "scratchFreeBytes": 800 * 1024**3,
            "scratchTemporaryPeakBytes": 140 * 1024**3,
            "durablePromotionBytes": 70 * 1024**3,
            "runtimeBytes": 5 * 1024**3,
            "evidenceBytes": 2 * 1024**3,
            "reserveBytes": 20 * 1024**3,
            "scratchPath": "/tmp/tma1",
            "scratchWritable": True,
        })

        self.assertFalse(decision["allowed"])
        self.assertIn("DURABLE_QUOTA_UNVERIFIED", decision["blockReasons"])
        self.assertIn("SCRATCH_AUTHORITY_UNVERIFIED", decision["blockReasons"])
        self.assertFalse(decision["cleanupAuthorized"])

    def test_capacity_accepts_explicitly_authorized_global_nfs_free_space(
            self) -> None:
        decision = self.capacity.evaluate_capacity({
            "currentUsageBytes": 76 * 1024**3,
            "verifiedQuotaBytes": 0,
            "quotaAuthority": "",
            "durableCapacityMode": "global-filesystem-free",
            "durableCapacityBytes": 800 * 1024**4,
            "durableCapacityAuthority":
                "user-authorized:df-B1:nfs4:/project:2026-07-28",
            "userAuthorizedGlobalCapacity": True,
            "scratchFreeBytes": 14 * 1024**4,
            "scratchTemporaryPeakBytes": 160 * 1024**3,
            "durablePromotionBytes": 70 * 1024**3,
            "runtimeBytes": 0,
            "evidenceBytes": 2 * 1024**3,
            "reserveBytes": 20 * 1024**3,
            "scratchPath": "/tmp/tma1",
            "scratchCapacityAuthority": "slurm-allocation:job-174610",
            "scratchWritable": True,
        })

        self.assertTrue(decision["allowed"])
        self.assertEqual(decision["blockReasons"], [])
        self.assertEqual(
            decision["additionalDurableWithReserveBytes"], 92 * 1024**3)
        self.assertEqual(decision["verifiedQuotaBytes"], 0)
        self.assertFalse(decision["cleanupAuthorized"])

    def test_global_nfs_free_space_requires_explicit_user_authorization(
            self) -> None:
        decision = self.capacity.evaluate_capacity({
            "durableCapacityMode": "global-filesystem-free",
            "durableCapacityBytes": 800 * 1024**4,
            "durableCapacityAuthority": "df-B1:nfs4:/project",
            "userAuthorizedGlobalCapacity": False,
            "scratchFreeBytes": 14 * 1024**4,
            "scratchTemporaryPeakBytes": 160 * 1024**3,
            "durablePromotionBytes": 70 * 1024**3,
            "evidenceBytes": 2 * 1024**3,
            "reserveBytes": 20 * 1024**3,
            "scratchPath": "/tmp/tma1",
            "scratchCapacityAuthority": "slurm-allocation:job-174610",
            "scratchWritable": True,
        })

        self.assertFalse(decision["allowed"])
        self.assertIn(
            "GLOBAL_CAPACITY_NOT_USER_AUTHORIZED", decision["blockReasons"])

    def test_linked_smoke_is_three_node_rtx_only_and_single_generation(
            self) -> None:
        smoke = GENERATION_SMOKE.read_text(encoding="utf-8")
        rank = GENERATION_RANK.read_text(encoding="utf-8")
        inner = GENERATION_RANK_INNER.read_text(encoding="utf-8")
        analyzer = SMOKE_ANALYZER.read_text(encoding="utf-8")

        self.assertIn("#SBATCH --nodes=3", smoke)
        self.assertIn("#SBATCH --ntasks=3", smoke)
        self.assertIn("#SBATCH --gres=gpu:rtx_5000:1", smoke)
        self.assertNotIn("h100", smoke.lower())
        self.assertIn("SPEC162_SMOKE_SUBMISSION_ID", rank)
        self.assertIn('--request-id "$SPEC162_SMOKE_SUBMISSION_ID"', inner)
        self.assertIn('ack_timeout_ms="${SPEC162_ACK_TIMEOUT_MS:-120000}"', inner)
        self.assertIn('--ack-timeout-ms "$ack_timeout_ms"', inner)
        self.assertIn('--env "SPEC162_ACK_TIMEOUT_MS=${ack_timeout_ms}"', rank)
        self.assertIn('selection_offer_lease_ms="${SPEC162_SELECTION_OFFER_LEASE_MS:-600000}"', inner)
        self.assertIn('--selection-offer-lease-ms "$selection_offer_lease_ms"', inner)
        self.assertIn('--env "SPEC162_SELECTION_OFFER_LEASE_MS=${selection_offer_lease_ms}"', rank)
        self.assertIn('provider_settle_seconds="${SPEC162_PROVIDER_SETTLE_SECONDS:-120}"', rank)
        self.assertIn('--env "SPEC162_PROVIDER_SETTLE_SECONDS=${provider_settle_seconds}"', rank)
        self.assertIn('user_startup_settle_ms="${SPEC162_USER_STARTUP_SETTLE_MS:-5000}"', rank)
        self.assertIn('--env "SPEC162_USER_STARTUP_SETTLE_MS=${user_startup_settle_ms}"', rank)
        self.assertIn('face_scheme="${SPEC162_FACE_SCHEME:-tcp4}"', rank)
        self.assertIn('--env "SPEC162_FACE_SCHEME=${face_scheme}"', rank)
        self.assertIn('provider_settle_seconds="${SPEC162_PROVIDER_SETTLE_SECONDS:-120}"', inner)
        self.assertIn('sleep "$provider_settle_seconds"', inner)
        self.assertIn('face_scheme="${SPEC162_FACE_SCHEME:-tcp4}"', inner)
        self.assertIn('uri="${face_scheme}://${peer_ip}:${SPEC162_PORT}"', inner)
        self.assertIn('NDNSF_DI_STARTUP_SETTLE_MS="${SPEC162_USER_STARTUP_SETTLE_MS:-5000}"', inner)
        self.assertIn("expected one smoke row", analyzer)
        self.assertIn("invalid three-stage layer cover", analyzer)
        self.assertIn('stage_manifest["layerCount"]', analyzer)

    def test_frozen_small_qwen_profile_is_exact_and_isolated(self) -> None:
        profile = self.prepare.MODEL_PROFILES["qwen3-0.6b"]

        self.assertEqual(profile["repository"], "Qwen/Qwen3-0.6B")
        self.assertEqual(
            profile["revision"],
            "e6de91484c29aa9480d55605af694f39b081c455",
        )
        self.assertEqual(profile["modelType"], "qwen3")
        self.assertEqual(profile["layerCount"], 28)
        self.assertEqual(profile["referenceDeviceMap"], "cuda:0")
        job = PREPARE_REFERENCE.read_text(encoding="utf-8")
        wrapper = PREPARE_STAGES.read_text(encoding="utf-8")
        self.assertIn("qwen3-0.6b", job)
        self.assertIn('${model_tag}-prep', job)
        self.assertIn('--model-profile "$model_profile"', wrapper)

    def test_generation_job_selects_sealed_smoke_or_formal_campaign(
            self) -> None:
        job = GENERATION_SMOKE.read_text(encoding="utf-8")
        formal_analyzer = FORMAL_ANALYZER.read_text(encoding="utf-8")

        self.assertIn(
            'readonly campaign_mode="${SPEC162_CAMPAIGN_MODE:-smoke}"',
            job,
        )
        self.assertIn("smoke|formal", job)
        self.assertIn(
            'readonly campaign_name="${campaign_mode}-campaign.json"',
            job,
        )
        self.assertIn('"$partial/source/jobs/analyze-generation-formal.py"', job)
        self.assertIn('--campaign "$SPEC162_SMOKE_CAMPAIGN"', job)
        self.assertIn('generation["requireEos"] = False', job)
        self.assertIn(
            'export SPEC162_SMOKE_CAMPAIGN="$partial/$campaign_name"',
            job,
        )
        self.assertIn("warmupPerPrompt", formal_analyzer)
        self.assertIn("measuredPerPrompt", formal_analyzer)

    def test_preparation_container_exposes_installed_sdk_and_pipeline(
            self) -> None:
        script = PREPARE_REFERENCE.read_text(encoding="utf-8")
        self.assertIn(
            'PYTHONPATH=/source/llm_pipeline:/opt/ndnsf-app/python',
            script,
        )
        self.assertIn('--env "HOME=/home/${USER}"', script)


if __name__ == "__main__":
    unittest.main()
