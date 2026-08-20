from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import hashlib
from pathlib import Path
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.artifact_deployment import (  # noqa: E402
    AssembledFragmentIdentity,
    CanonicalResidencyIdentity,
    LoadedRuntimeIdentity,
    ProviderResidencyLedger,
)
from ndnsf_distributed_inference.core.deployment_control import (  # noqa: E402
    BoundedSingleFlight,
    CanonicalFetchResult,
    ExactArtifactPreparationPipeline,
    ExactPreparationCallbacks,
    FragmentBuildResult,
    RuntimeLoadResult,
)


def digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


CANONICAL = b"canonical-onnx-model"
ASSEMBLED = b"assembled-onnx-role"
D = {name: digest(name.encode()) for name in (
    "model", "profile", "graph", "recipe", "adapter", "assembler",
    "kernel", "topology", "state",
)}


def identities(*, topology: str | None = None, protection: str = "plain-v1"):
    canonical = CanonicalResidencyIdentity(
        D["model"], D["profile"], D["graph"], digest(CANONICAL), protection)
    fragment = AssembledFragmentIdentity(
        D["model"], D["profile"], D["graph"], D["recipe"],
        digest(ASSEMBLED), D["adapter"], D["assembler"],
        "onnxruntime-1", "fp32", "none", "native", "none", protection)
    loaded = LoadedRuntimeIdentity(
        fragment.digest, fragment.artifact_digest, "onnxruntime-cpu", "1.20",
        "cpu-abi", D["kernel"], (), topology or D["topology"],
        "boot-0001", "process-0001", 1, "fence-0001", protection, D["state"])
    return canonical, fragment, loaded


class Spec170ExactResidencyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.ledger = ProviderResidencyLedger(
            self.root, provider_boot_epoch="boot-0001",
            max_canonical_entries=4, max_assembled_entries=4,
            max_loaded_entries=4)
        self.calls = {"fetch": 0, "build": 0, "load": 0}

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def pipeline(self, *, pause: bool = False) -> ExactArtifactPreparationPipeline:
        def fetch(identity):
            self.calls["fetch"] += 1
            if pause:
                time.sleep(0.03)
            path = self.root / "canonical" / "model.onnx"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(CANONICAL)
            return CanonicalFetchResult(
                path, len(CANONICAL), len(CANONICAL), len(CANONICAL) + 7)

        def build(_canonical_hit, _identity):
            self.calls["build"] += 1
            if pause:
                time.sleep(0.03)
            path = self.root / "assembled" / "role.bundle"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(ASSEMBLED)
            return FragmentBuildResult(path, len(ASSEMBLED), len(ASSEMBLED))

        def load(_fragment_hit, _identity):
            self.calls["load"] += 1
            if pause:
                time.sleep(0.03)
            return RuntimeLoadResult(object(), len(ASSEMBLED))

        return ExactArtifactPreparationPipeline(
            ledger=self.ledger,
            callbacks=ExactPreparationCallbacks(fetch, build, load),
            flights=BoundedSingleFlight(max_inflight=3))

    def test_cold_then_exact_warm_has_zero_model_work(self):
        canonical, fragment, loaded = identities()
        pipeline = self.pipeline()
        cold = pipeline.ensure_loaded(
            canonical_identity=canonical, fragment_identity=fragment,
            loaded_identity=loaded, owner="request-1")
        self.assertFalse(cold.exact_warm_hit)
        self.assertEqual(cold.transferred_bytes, len(CANONICAL) + 7)
        self.assertEqual(cold.built_bytes, len(ASSEMBLED))
        self.assertEqual(cold.loaded_bytes, len(ASSEMBLED))
        self.ledger.release_exact(loaded, owner="request-1")

        warm = pipeline.ensure_loaded(
            canonical_identity=canonical, fragment_identity=fragment,
            loaded_identity=loaded, owner="request-2")
        self.assertTrue(warm.exact_warm_hit)
        self.assertEqual(
            (warm.transferred_bytes, warm.built_bytes, warm.loaded_bytes),
            (0, 0, 0))
        self.assertEqual(self.calls, {"fetch": 1, "build": 1, "load": 1})
        counters = self.ledger.snapshot()["counters"]
        self.assertEqual(counters["canonicalAdmitCount"], 1)
        self.assertEqual(counters["assembledBuildCount"], 1)
        self.assertEqual(counters["runtimeLoadCount"], 1)
        proof = self.ledger.make_residency_proof_v3(
            loaded, role="stage0", rank=0,
            process_epoch="process-0001", topology_digest=D["topology"],
            captured_at_ms=1, expires_at_ms=100)
        self.assertTrue(proof.is_exact_reuse_proof())
        self.assertEqual(proof.identity_digest, loaded.digest)
        self.assertEqual(proof.assembly_spec_digest, D["recipe"])

    def test_equal_concurrent_work_is_single_flighted(self):
        canonical, fragment, loaded = identities()
        pipeline = self.pipeline(pause=True)

        def prepare(index: int):
            return pipeline.ensure_loaded(
                canonical_identity=canonical, fragment_identity=fragment,
                loaded_identity=loaded, owner=f"request-{index}")

        with ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = tuple(pool.map(prepare, (1, 2)))
        self.assertEqual(self.calls, {"fetch": 1, "build": 1, "load": 1})
        self.assertEqual(sum(item.transferred_bytes > 0 for item in outcomes), 1)
        self.assertEqual(sum(item.built_bytes > 0 for item in outcomes), 1)
        self.assertEqual(sum(item.loaded_bytes > 0 for item in outcomes), 1)

    def test_runtime_context_and_protection_invalidate_exact_levels(self):
        canonical, fragment, loaded = identities()
        pipeline = self.pipeline()
        pipeline.ensure_loaded(
            canonical_identity=canonical, fragment_identity=fragment,
            loaded_identity=loaded, owner="request-1")
        self.ledger.release_exact(loaded, owner="request-1")
        removed = self.ledger.invalidate_exact_context(
            provider_boot_epoch="boot-0001", process_epoch="process-0001",
            topology_digest=digest(b"new-topology"), fencing_token="fence-0001",
            protection_epoch="plain-v1", runtime_generation=1)
        self.assertEqual(removed, (loaded.digest,))
        self.assertIsNotNone(self.ledger.lookup_assembled(fragment))
        self.assertIsNotNone(self.ledger.lookup_canonical(canonical))

        removed = self.ledger.invalidate_exact_context(
            provider_boot_epoch="boot-0001", process_epoch="process-0001",
            topology_digest=digest(b"new-topology"), fencing_token="fence-0001",
            protection_epoch="plain-v2", runtime_generation=1)
        self.assertEqual(set(removed), {canonical.digest, fragment.digest})

    def test_bound_never_evicts_an_owned_entry(self):
        ledger = ProviderResidencyLedger(
            self.root / "bounded", provider_boot_epoch="boot-0001",
            max_canonical_entries=1)
        first, _, _ = identities()
        second = CanonicalResidencyIdentity(
            D["model"], D["profile"], D["graph"], digest(b"second"),
            "plain-v1")
        first_path = ledger.root / "first.bin"
        first_path.write_bytes(CANONICAL)
        ledger.admit_canonical(first, first_path, size=len(CANONICAL))
        ledger.acquire_exact(first, owner="active")
        second_path = ledger.root / "second.bin"
        second_path.write_bytes(b"second")
        with self.assertRaisesRegex(RuntimeError, "active entries"):
            ledger.admit_canonical(second, second_path, size=6)
        ledger.release_exact(first, owner="active")
        ledger.admit_canonical(second, second_path, size=6)
        self.assertIsNone(ledger.lookup_canonical(first))
        self.assertIsNotNone(ledger.lookup_canonical(second))


if __name__ == "__main__":
    unittest.main()
