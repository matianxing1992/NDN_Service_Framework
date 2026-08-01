from pathlib import Path
import os


ROOT = Path(__file__).resolve().parents[2]
JOBS = ROOT / "specs/166-spec165-itiger-validation/jobs"


def test_standalone_uses_candidate_virtualenv_for_application_imports():
    source = (JOBS / "standalone-gpu-reference.sbatch").read_text(
        encoding="utf-8")

    assert "/opt/venv/bin/python /source/run_standalone_gpu_reference.py" in source
    assert "\n  python3 /source/run_standalone_gpu_reference.py" not in source
    assert (
        "--env PYTHONPATH=/source/llm_pipeline:/opt/ndnsf-app/python"
        in source
    )
    assert "SPEC166_SOURCE_ROOT_REQUIRED" in source


def test_multinode_application_processes_use_candidate_virtualenv():
    source = (JOBS / "multinode-rank-inner.sh").read_text(encoding="utf-8")

    assert source.count("/opt/venv/bin/python") >= 3
    assert "export PYTHONPATH=" in source
    assert "/opt/ndnsf-app/python" in source


def test_jobs_require_versioned_source_root_instead_of_shared_mutable_path():
    standalone = (JOBS / "standalone-gpu-reference.sbatch").read_text(
        encoding="utf-8")
    multinode = (JOBS / "multinode-qwen.sbatch").read_text(encoding="utf-8")

    assert 'readonly SOURCE="${SPEC166_SOURCE_ROOT}"' in standalone
    assert 'readonly SPEC166_SOURCE="${SPEC166_SOURCE_ROOT}"' in multinode
    assert 'readonly SOURCE="${ROOT}/jobs/spec166/source"' not in standalone
    assert 'readonly SPEC166_SOURCE="${ROOT}/jobs/spec166/source"' not in multinode


def test_pre_submit_probe_derives_environment_from_actual_sbatch():
    source = (
        JOBS / "validate-standalone-container-contract.sh"
    ).read_text(encoding="utf-8")

    assert "standalone-gpu-reference.sbatch" in source
    assert "sed -n" in source
    assert '-e "PYTHONPATH=${python_path}"' in source
    assert '--entrypoint "$interpreter"' in source
    assert "SPEC166_EXACT_SBATCH_ENV_IMPORT_PASS" in source


def test_multinode_entrypoints_are_executable_and_checked_before_srun():
    rank = JOBS / "multinode-rank.sh"
    inner = JOBS / "multinode-rank-inner.sh"
    source = (JOBS / "multinode-qwen.sbatch").read_text(encoding="utf-8")

    assert os.access(rank, os.X_OK)
    assert os.access(inner, os.X_OK)
    assert 'test -x "$SPEC166_SOURCE/multinode-rank.sh"' in source
    assert 'test -x "$SPEC166_SOURCE/multinode-rank-inner.sh"' in source


def test_multinode_host_wrapper_reads_nfd_config_from_source_root():
    source = (JOBS / "multinode-rank.sh").read_text(encoding="utf-8")

    assert ': "${SPEC166_SOURCE:?}"' in source
    assert '"$SPEC166_SOURCE/nfd.conf.in"' in source
    assert "    /source/nfd.conf.in >" not in source


def test_multinode_uses_manifest_bound_python_api_overlay():
    job = (JOBS / "multinode-qwen.sbatch").read_text(encoding="utf-8")
    inner = (JOBS / "multinode-rank-inner.sh").read_text(encoding="utf-8")

    assert (
        'test -f "$SPEC166_SOURCE/ndnsf_distributed_inference/'
        'app_sdk/client.py"' in job
    )
    assert (
        'test -f "$SPEC166_SOURCE/ndnsf_distributed_inference/'
        'compatibility/manifest.json"' in job
    )
    assert (
        'export PYTHONPATH="/source:/source/llm_pipeline:'
        '/opt/ndnsf-app/python:${PYTHONPATH:-}"' in inner
    )


def test_multinode_removes_bootstrap_tokens_before_completion_markers():
    job = (JOBS / "multinode-qwen.sbatch").read_text(encoding="utf-8")
    inner = (JOBS / "multinode-rank-inner.sh").read_text(encoding="utf-8")

    assert 'test -f "$SPEC166_SOURCE/analyze_multinode.py"' in job
    token_cleanup = inner.index("rm -f -- /shared/bootstrap-tokens.txt")
    user_done = inner.index("touch /shared/user-done")
    rank_complete = inner.index('touch "/shared/rank-complete-${rank}"')
    assert token_cleanup < user_done < rank_complete
