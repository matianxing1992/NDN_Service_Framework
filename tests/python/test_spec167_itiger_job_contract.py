#!/usr/bin/env python3

from pathlib import Path
import stat
import unittest
import importlib.util

ROOT = Path(__file__).resolve().parents[2]
JOBS = ROOT / "specs/167-itiger-repo-throughput/jobs"
SOURCE_BUNDLE = ROOT / "Experiments/validate_spec167_source_bundle.py"


def load_source_bundle_validator():
    spec = importlib.util.spec_from_file_location(
        "spec167_source_bundle", SOURCE_BUNDLE
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class Spec167ItigerJobContractTest(unittest.TestCase):
    def test_required_sources_exist_and_are_executable(self):
        for name in (
            "validate-local-candidate.sh",
            "run-local-two-container-preflight.sh",
            "run-local-two-container-formal-smoke.sh",
            "rank.sh",
            "rank-inner.sh",
            "campaign-rank-inner.sh",
        ):
            path = JOBS / name
            self.assertTrue(path.is_file(), name)
            self.assertTrue(path.stat().st_mode & stat.S_IXUSR, name)
        self.assertTrue((JOBS / "repo-throughput-preflight.sbatch").is_file())
        self.assertTrue((JOBS / "repo-throughput-formal-smoke.sbatch").is_file())
        self.assertTrue((JOBS / "repo-throughput.sbatch").is_file())
        self.assertTrue((JOBS / "campaign-rank-inner.sh").is_file())
        self.assertTrue((JOBS / "nfd.conf.in").is_file())
        self.assertNotIn("\n      status\n", (JOBS / "nfd.conf.in").read_text())

    def test_preflight_is_cpu_only_exact_once_and_two_node(self):
        source = (JOBS / "repo-throughput-preflight.sbatch").read_text()
        self.assertIn("#SBATCH --nodes=2", source)
        self.assertIn("#SBATCH --nodelist=itiger07,itiger08", source)
        self.assertNotIn("#SBATCH --gres", source)
        self.assertIn("test ! -e \"$PARTIAL\" && test ! -e \"$FINAL\"", source)
        self.assertIn("source-checksums.sha256", source)
        self.assertIn("sif-checksum.log", source)

    def test_rank_wrapper_uses_local_scratch_and_exact_container_paths(self):
        source = (JOBS / "rank.sh").read_text()
        self.assertIn("${SLURM_TMPDIR:-/tmp/${USER}}", source)
        self.assertIn("--bind \"$scratch:/scratch:rw\"", source)
        self.assertIn("--bind \"$SPEC167_SHARED:/shared:rw\"", source)
        self.assertIn("--bind \"$SPEC167_SOURCE:/source:ro\"", source)
        self.assertNotIn("--nv", source)

    def test_inner_rank_exercises_cross_node_raw_and_cold_paths(self):
        source = (JOBS / "rank-inner.sh").read_text()
        self.assertIn("route add /spec164/raw", source)
        self.assertIn("route add /spec164/repo-cold", source)
        self.assertIn("--data-dir \"$data\"", source)
        self.assertIn("SPEC167_CROSS_NODE_RAW_PASS", source)
        self.assertLess(
            source.index('wait_file "$coord/preflight.producer-ready.json"'),
            source.index('--role cold-consumer'),
        )

    def test_local_candidate_starts_exact_nfd_config(self):
        source = (JOBS / "validate-local-candidate.sh").read_text()
        self.assertIn("timeout 2s nfd --config /input/nfd.conf", source)
        self.assertIn("test \"$rc\" -eq 124", source)
        self.assertIn("run-local-two-container-preflight.sh", source)
        self.assertIn("run-local-two-container-formal-smoke.sh", source)
        self.assertIn("raw-segmented-ndn legacy-exact-packet digest-only signed-manifest", source)
        local_source = (JOBS / "run-local-two-container-preflight.sh").read_text()
        self.assertIn('"$WORK/source:/source:ro"', local_source)
        self.assertIn("Experiments/spec164_artifact_campaign.py", local_source)
        self.assertNotIn('"$ROOT:/source:ro"', local_source)

    def test_exact_source_bundle_declares_runtime_helper(self):
        source = (JOBS / "validate-local-candidate.sh").read_text()
        self.assertIn('SOURCE_ROOT="${SPEC167_SOURCE_ROOT:-$ROOT}"', source)
        self.assertIn("validate_spec167_source_bundle.py", source)
        self.assertIn("! -perm -o+x", source)
        self.assertIn("! -perm -o+r", source)
        validator = load_source_bundle_validator()
        self.assertIn(
            "Experiments/spec164_artifact_campaign.py",
            validator.REQUIRED_RUNTIME_FILES,
        )

    def test_source_bundle_validator_rejects_missing_runtime_helper(self):
        import hashlib
        import json
        import tempfile

        validator = load_source_bundle_validator()
        with tempfile.TemporaryDirectory() as temporary:
            root = __import__("pathlib").Path(temporary)
            experiments = root / "Experiments"
            experiments.mkdir()
            entries = []
            for name in (
                "NDNSF_DistributedRepo_Artifact_Itiger.py",
                "NDNSF_DistributedRepo_Artifact_Minindn.py",
                "analyze_spec167_itiger_repo.py",
            ):
                path = experiments / name
                path.write_text("# fixture\n")
                entries.append({
                    "bytes": path.stat().st_size,
                    "path": f"Experiments/{name}",
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                })
            manifest = root / "source-manifest.json"
            manifest.write_text(json.dumps({"files": entries}))
            errors = validator.validate_source_bundle(root, manifest)
            self.assertTrue(
                any("spec164_artifact_campaign.py" in error for error in errors)
            )

    def test_formal_campaign_freezes_matrix_and_retains_failures(self):
        source = (JOBS / "repo-throughput.sbatch").read_text()
        self.assertIn("#SBATCH --time=02:30:00", source)
        self.assertIn("SPEC167_DURATION_SECONDS=60", source)
        self.assertIn('>= 60000', source)
        self.assertIn("run_index * 32", source)
        self.assertIn('rank.sh" < /dev/null', source)
        self.assertIn("run_status=FAIL", source)
        self.assertIn("run-records.jsonl", source)
        self.assertIn("analyze_spec167_itiger_repo.py", source)
        self.assertIn('mkdir -p "$PARTIAL/runs/${sub_name}"', source)
        self.assertNotIn("#SBATCH --gres", source)


if __name__ == "__main__":
    unittest.main()
