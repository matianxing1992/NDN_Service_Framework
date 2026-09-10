"""Focused launch-boundary tests; these do not qualify a SIF or GPU run."""
import importlib.util
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("yolo_baseline_test", ROOT / "runtime/baseline.py")
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)


def command(tmp_path, **options):
    return baseline.container_command(
        {"apptainer": "/usr/local/bin/apptainer", "sif": "/cache/runtime.sif"},
        tmp_path / "bundle", tmp_path / "private/backbone", tmp_path / "public",
        tmp_path / "out", ["/opt/venv/bin/python", "app.py"], **options)


@pytest.mark.parametrize("device", ["0", "3", "GPU-12345678-abcd-1234-abcd-123456789abc"])
def test_gpu_device_is_explicit_and_package_is_read_only(tmp_path, device):
    argv = command(tmp_path, gpu=True, gpu_device=device, artifacts=tmp_path / "package")
    assert argv.count("--nv") == 1
    assert argv.index("--nv") < argv.index("/cache/runtime.sif")
    assert "CUDA_VISIBLE_DEVICES=" + device in argv
    assert str(tmp_path / "package") + ":/artifacts:ro" in argv
    assert argv[argv.index("--pwd") + 1] == "/bundle"
    assert argv[argv.index("--home") + 1] == "/identities/backbone"
    assert "LD_LIBRARY_PATH=/opt/ndnsf-di/current/lib:/opt/onnxruntime/lib:/.singularity.d/libs" in argv
    assert not any(":/identities:rw" in arg for arg in argv)


@pytest.mark.parametrize("options", [
    {"gpu": True}, {"gpu": False, "gpu_device": "0"},
    {"gpu": 1, "gpu_device": "0"}, {"gpu": True, "gpu_device": ""},
    {"gpu": True, "gpu_device": "0,1"}, {"gpu": True, "gpu_device": "all"},
    {"gpu": True, "gpu_device": "0\nLD_PRELOAD=/host.so"},
    {"gpu": True, "gpu_device": 0},
])
def test_gpu_assignment_must_name_exactly_one_device(tmp_path, options):
    with pytest.raises(ValueError, match="GPU_DEVICE"):
        command(tmp_path, **options)


@pytest.mark.parametrize("mount", ["node", "prepare", "artifacts"])
@pytest.mark.parametrize("path", ["relative", "/tmp/a:/etc", "/tmp/a,b", "/tmp/a\nb"])
def test_optional_mounts_cannot_escape_fixed_destinations(tmp_path, mount, path):
    with pytest.raises(ValueError, match="BIND_PATH"):
        command(tmp_path, **{mount: Path(path)})


def test_cpu_command_has_no_gpu_or_model_mount_by_default(tmp_path):
    argv = command(tmp_path)
    assert "--nv" not in argv
    assert not any(arg.startswith("CUDA_VISIBLE_DEVICES=") or ":/artifacts:" in arg for arg in argv)


def test_host_runtime_environment_is_not_an_input(monkeypatch):
    for key in ("CUDA_VISIBLE_DEVICES", "NVIDIA_VISIBLE_DEVICES", "ORT_LOG_LEVEL",
                "APPTAINERENV_LD_PRELOAD", "NDNSF_CONFIG", "PYTHONPATH", "LD_PRELOAD"):
        monkeypatch.setenv(key, "untrusted-host-value")
    env = baseline.container_env()
    assert "untrusted-host-value" not in env.values()


def test_real_child_uses_explicit_working_directory(tmp_path):
    work = tmp_path / "bundle"
    work.mkdir()
    (work / "artifact.txt").write_text("expected-model-input")
    children = baseline.Processes(tmp_path / "logs")
    try:
        child = children.start("cwd-probe", [sys.executable, "-c",
            "from pathlib import Path; print(Path('artifact.txt').read_text())"], cwd=work)
        assert child.wait(timeout=5) == 0
    finally:
        rows = children.close()
    assert rows[0]["reaped"] and not rows[0]["forced"]
    assert (tmp_path / "logs/cwd-probe.log").read_text().strip() == "expected-model-input"
