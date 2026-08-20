from __future__ import annotations

import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = (
    ROOT / "specs/162-itiger-qwen36-generation/jobs"
    / "build-t009-live-source-bundle.py"
)
RANK_SCRIPT = (
    ROOT / "specs/162-itiger-qwen36-generation/jobs" / "generation-rank.sh"
)
SBATCH_SCRIPT = (
    ROOT / "specs/162-itiger-qwen36-generation/jobs"
    / "generation-smoke.sbatch"
)
RANK_INNER_SCRIPT = (
    ROOT / "specs/162-itiger-qwen36-generation/jobs"
    / "generation-rank-inner.sh"
)
REPO_NODE_SCRIPT = (
    ROOT / "specs/162-itiger-qwen36-generation/jobs" / "run-repo-node.py"
)
REPO_REGISTER_SCRIPT = (
    ROOT / "specs/162-itiger-qwen36-generation/jobs"
    / "register-qwen36-repo.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location(
        "build_t009_live_source_bundle", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class T009LiveSourceBundleTests(unittest.TestCase):
    def test_cold_repo_publication_uses_whole_artifact_collaboration(self):
        inner = RANK_INNER_SCRIPT.read_text(encoding="utf-8")
        user = (ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py").read_text(
            encoding="utf-8")
        registration = REPO_REGISTER_SCRIPT.read_text(encoding="utf-8")
        # Request-scoped publication stays on the inference User's existing
        # ServiceUser/SVS session after ACK_CLOSED.  A second same-identity
        # ServiceUser would supersede the session and make final Selection
        # stale at Providers.
        self.assertIn("service_user=client.service_user", user)
        self.assertIn("publish_qwen_stage_manifest", user)
        self.assertIn("CollaborationArtifactApiBackend(", user)
        self.assertNotIn("subprocess.run(", user)
        self.assertIn('choices=("normal", "targeted")', registration)
        self.assertIn("CollaborationArtifactApiBackend.from_config", registration)
        self.assertIn("repo.publish_file(", registration)
        self.assertNotIn("repo.put_file(", registration)
        self.assertIn('"controlMode": args.control_mode', registration)
        self.assertNotIn("repo-registration-ready", inner)
        self.assertIn("LLM_PIPELINE_QWEN_DEFERRED_REPO_PUBLISH_START", user)
        sbatch = SBATCH_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("#SBATCH --time=16:00:00", sbatch)

    def test_tiger_manual_fib_explicitly_carries_svs_group_prefix(self):
        inner = RANK_INNER_SCRIPT.read_text(encoding="utf-8")
        self.assertIn(
            'route add /NDNSF-DistributeInference/example/group "$uri"',
            inner,
        )
        self.assertIn(
            'strategy set /NDNSF-DistributeInference/example/group',
            inner,
        )
        self.assertIn(
            'uri="${face_scheme}://${peer_ip}:${SPEC162_PORT}"',
            inner,
        )

    def test_request_gate_has_no_fixed_provider_settle_sleep(self):
        inner = RANK_INNER_SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn("\nsleep 30\n", inner)
        self.assertIn(
            "the first planning trigger; certificates are fetched on demand",
            inner,
        )

    def test_deferred_planner_records_graph_after_ack_closed(self):
        placement = (
            ROOT / "NDNSF-DistributedInference"
            / "ndnsf_distributed_inference/app_sdk/placement.py"
        ).read_text(encoding="utf-8")
        self.assertIn(
            '"NDNSF_DI_AUTOPLANNING_GRAPH_READY"',
            placement,
        )
        self.assertIn('"after=ACK_CLOSED"', placement)
        self.assertLess(
            placement.index("collaboration = self.service_user.begin_collaboration"),
            placement.index("closed = collaboration.acks_closed()"),
        )
        self.assertLess(
            placement.index("closed = collaboration.acks_closed()"),
            placement.index("model_descriptor = adapter.describe_model"),
        )

    def test_repo_node_uses_explicit_operator_state_root(self):
        inner = RANK_INNER_SCRIPT.read_text(encoding="utf-8")
        repo_node = REPO_NODE_SCRIPT.read_text(encoding="utf-8")
        self.assertIn(
            '--state-root "/scratch/operator-state"',
            inner,
        )
        self.assertIn(
            'parser.add_argument("--state-root", required=True)',
            repo_node,
        )
        self.assertIn("state_root=args.state_root", repo_node)
        self.assertNotIn("test_only_allow_ephemeral_state_root=True", repo_node)

    def test_sbatch_runs_read_only_rank_script_through_bash(self):
        text = SBATCH_SCRIPT.read_text(encoding="utf-8")
        self.assertIn(
            'bash "$partial/source/jobs/generation-rank.sh"',
            text,
        )
        self.assertIn("source-files-sha256.log", text)
        self.assertIn(
            'cp -a "$SPEC162_SMOKE_SOURCE_ROOT"/runtime "$partial/source/"',
            text,
        )
        self.assertIn("#SBATCH --signal=B:USR1@120", text)
        self.assertIn("terminal_state=CANCELLED", text)
        self.assertIn("trap request_cancel INT TERM USR1", text)
        self.assertIn('kill -TERM "$rank_step_pid"', text)
        self.assertIn('wait "$rank_step_pid"', text)
        self.assertIn('rm -f -- "$partial/bootstrap-tokens.txt"', text)
        self.assertIn(
            'SPEC162_FACE_SCHEME="${SPEC162_FACE_SCHEME:-tcp4}"',
            text,
        )

    def test_runtime_marker_gate_does_not_scan_sealed_source_bundle(self):
        text = SBATCH_SCRIPT.read_text(encoding="utf-8")
        self.assertIn('"$partial"/node-* "$partial"/rank-step.log', text)
        self.assertIn(
            "source bundle intentionally",
            text,
        )

    def test_ephemeral_bootstrap_is_removed_before_secret_scan(self):
        text = SBATCH_SCRIPT.read_text(encoding="utf-8")
        cleanup = 'rm -f -- "$partial/bootstrap-tokens.txt"'
        scan = 'test -z "$(find "$partial" -type f'
        self.assertGreaterEqual(text.count(cleanup), 2)
        self.assertLess(text.index(cleanup, text.index("test \"$(<")),
                        text.index(scan))
        self.assertNotIn(
            "grep -RniE \\\n  'double free|invalid pointer|munmap_chunk|Aborted|core dumped|CPUExecutionProvider|"
            "QWEN_STAGE_CPU_FALLBACK_FORBIDDEN|QWEN_STAGE_CUDA_UNAVAILABLE|"
            "reportOperationStatus.*incompatible function arguments' \\\n  \"$partial\";",
            text,
        )

    def test_candidate_policy_adds_artifact_service_without_mutating_input(self):
        module = load_module()
        self.assertIn(
            "jobs/build-generation-policy.py",
            module.SOURCES,
        )
        inner = RANK_INNER_SCRIPT.read_text(encoding="utf-8")
        self.assertIn(
            "/source/jobs/build-generation-policy.py",
            inner,
        )
        self.assertIn("export SPEC162_POLICY=/shared/policy.yaml", inner)
        self.assertIn("/shared/policy-provenance.json", inner)

    def test_minindn_repo_processes_do_not_reuse_compute_provider_identity(self):
        experiment = (
            ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
        ).read_text(encoding="utf-8")
        runner = (
            ROOT / "specs/162-itiger-qwen36-generation/jobs/run-repo-node.py"
        ).read_text(encoding="utf-8")
        policy = (
            ROOT / "specs/162-itiger-qwen36-generation/jobs/"
            "build-generation-policy.py"
        ).read_text(encoding="utf-8")
        self.assertIn('REPO_PROVIDER_PREFIX = APP_ROOT + "/repo"', experiment)
        self.assertIn('"--repo-provider-prefix", REPO_PROVIDER_PREFIX', experiment)
        self.assertIn('" --provider-prefix "', experiment)
        self.assertIn('parser.add_argument("--provider-prefix", required=True)', runner)
        self.assertIn('provider_prefix=args.provider_prefix', runner)
        self.assertIn('parser.add_argument("--repo-provider-prefix", required=True)', policy)
        inner = RANK_INNER_SCRIPT.read_text(encoding="utf-8")
        provider = (
            ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline/"
            "provider.py"
        ).read_text(encoding="utf-8")
        self.assertIn('repo_provider_identity="/NDNSF-DistributeInference/example/repo', inner)
        self.assertIn('--provider-prefix /NDNSF-DistributeInference/example/repo', inner)
        self.assertIn('--bootstrap-token-file "/scratch/provider-bootstrap-${rank}.token"', inner)
        self.assertIn('parser.add_argument("--bootstrap-token-file", default="")', provider)

    def test_runtime_override_is_source_sealed_and_read_only(self):
        module = load_module()
        rank = RANK_SCRIPT.read_text(encoding="utf-8")
        for name in (
            "__init__.py",
            "artifact_api.py",
            "artifact_lifecycle.py",
            "artifact_transfer.py",
            "network_artifact_backend.py",
            "orchestration.py",
            "persistence.py",
            "service_names.py",
        ):
            relative = f"runtime/py_repoclient/{name}"
            self.assertIn(relative, module.SOURCES)
            self.assertIn(
                f"$repo_override/{name}:/opt/ndnsf-app/python/"
                f"py_repoclient/{name}:ro",
                rank,
            )

    def test_sdk_placement_contract_is_bound_into_the_runtime(self):
        module = load_module()
        rank = RANK_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("runtime/sdk-placement.py", module.SOURCES)
        self.assertIn(
            "$di_sdk_placement_override:/opt/ndnsf-app/python/"
            "ndnsf_distributed_inference/sdk/placement.py:ro",
            rank,
        )

    def test_rank_scratch_fallback_does_not_duplicate_cluster_user(self):
        text = RANK_SCRIPT.read_text(encoding="utf-8")
        self.assertIn(
            'scratch="/tmp/${USER}/ndnsf-di/spec162-smoke-'
            '${SLURM_JOB_ID}-rank-${rank}"',
            text,
        )
        self.assertNotIn(
            'base="${SLURM_TMPDIR:-/tmp/${USER}}"',
            text,
        )
        self.assertIn(
            '--env "SPEC162_ARTIFACT_DIR=${SPEC162_ARTIFACT_DIR}"',
            text,
        )

    def test_contained_user_receives_container_visible_campaign_and_tokenizer(self):
        rank = RANK_SCRIPT.read_text(encoding="utf-8")
        inner = RANK_INNER_SCRIPT.read_text(encoding="utf-8")
        self.assertIn(
            'container_campaign="/shared/$(basename '
            '"$SPEC162_SMOKE_CAMPAIGN")"',
            rank,
        )
        self.assertIn(
            '--env "SPEC162_SMOKE_CAMPAIGN=${container_campaign}"',
            rank,
        )
        self.assertNotIn(
            '--env "SPEC162_SMOKE_CAMPAIGN=${SPEC162_SMOKE_CAMPAIGN}"',
            rank,
        )
        self.assertIn(
            '--generation-campaign-manifest "$SPEC162_SMOKE_CAMPAIGN"',
            inner,
        )
        self.assertIn(
            '--qwen-tokenizer-dir "$SPEC162_ARTIFACT_DIR/tokenizer"',
            inner,
        )

    def test_policy_barrier_covers_slow_cross_node_policy_generation(self):
        inner = RANK_INNER_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("wait_file /shared/policy-ready 1800", inner)
        self.assertIn("wait_file /shared/policy.yaml 1800", inner)
        self.assertIn(
            "wait_file /shared/policy-provenance.json 1800",
            inner,
        )
        self.assertNotIn("wait_file /shared/policy-ready 600", inner)

    def test_builds_self_verifying_bundle(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "bundle"
            result = module.build(output)
            manifest = output / "source-files.sha256"
            self.assertEqual(
                result["sourceCount"], len(module.SOURCES))
            self.assertEqual(
                result["sourceManifestSha256"],
                hashlib.sha256(manifest.read_bytes()).hexdigest(),
            )
            rows = manifest.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(rows), len(module.SOURCES))
            for row in rows:
                expected, relative = row.split("  ", 1)
                self.assertEqual(
                    hashlib.sha256((output / relative).read_bytes()).hexdigest(),
                    expected,
                )

    def test_rejects_nonempty_output(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "bundle"
            output.mkdir()
            (output / "old").write_text("old", encoding="utf-8")
            with self.assertRaisesRegex(
                    module.BundleError, "OUTPUT_DIRECTORY_NOT_EMPTY"):
                module.build(output)


if __name__ == "__main__":
    unittest.main()
