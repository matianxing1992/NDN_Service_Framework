from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
PREFLIGHT = ROOT / "packaging/ndnsf-di-container/bin/ndnsf-di-spec175-functional-preflight"
ROLES = (
    "/LLM/Pipeline/Stage/0",
    "/LLM/Pipeline/Stage/1",
    "/LLM/Pipeline/Stage/2",
)


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def make_bundle(root: Path, gate: str = "multi-provider") -> Path:
    bundle = root / "bundle"
    (bundle / "providers").mkdir(parents=True)
    (bundle / "nfd.conf").write_text("face_system { };\n", encoding="utf-8")
    (bundle / "controller-wrapper.sh").write_text(
        "controller.start()\n"
        "controller.start_background()\n"
        "print('NDNSF_DI_CONTROLLER_READY', flush=True)\n"
        "# background event loop owns controller.run()\n",
        encoding="utf-8",
    )
    (bundle / "controller.args").write_text(
        "bash\n-lc\nexec /bundle/controller-wrapper.sh\n",
        encoding="utf-8",
    )
    (bundle / "user.args").write_text(
        "python3\nuser.py\n--runtime\nqwen-onnx\n"
        "--generation-campaign-manifest\ncampaign.json\n"
        "--automatic-planning-manifest\nautomatic-planning.json\n"
        "--qwen-stage-manifest\n/model/stage-manifest.json\n"
        "--qwen-tokenizer-dir\n/model/qwen-onnx-tokenizer\n"
        "--stages\n3\n",
        encoding="utf-8",
    )
    for index, role in enumerate(ROLES):
        (bundle / "providers" / f"provider-{index}.args").write_text(
            f"python3\nprovider.py\n--runtime\nqwen-onnx\n"
            f"--roles\n{role}\n--device\ncuda:{index}\n"
            f"--selection-local-artifact\n{role}=/model/qwen-onnx-stage-artifacts/stage-{index}.onnx\n"
            "--require-cuda\n--require-onnx-runtime\n--stages\n3\n",
            encoding="utf-8",
        )
    stage = {
        "repository": "Qwen/Qwen3.6-27B",
        "revision": "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        "decodeMode": "single-token-autoregressive",
        "modality": "text-only",
        "mtpEnabled": False,
        "stages": [
            {"role": role, "layerRange": {"start": start, "endExclusive": end}}
            for role, (start, end) in zip(ROLES, ((0, 21), (21, 42), (42, 64)))
        ],
    }
    for name, value in {
        "stage-manifest.json": stage,
        "workload.json": {"schemaVersion": "spec175-workload-v1"},
        "campaign.json": {"schemaVersion": "ndnsf-di-qwen-generation-campaign-v1"},
        "automatic-planning.json": {"schemaVersion": "ndnsf-di-spec162-automatic-planning-v1"},
    }.items():
        (bundle / name).write_text(json.dumps(value), encoding="utf-8")
    records = {
        name: {"path": name, "sha256": digest(bundle / name)}
        for name in ("stage-manifest.json", "workload.json", "campaign.json", "automatic-planning.json")
    }
    manifest = {
        "schemaVersion": "ndnsf-di-spec175-functional-bundle-v1",
        "gate": gate,
        "candidateId": "candidate-175",
        "model": {
            "repository": "Qwen/Qwen3.6-27B",
            "revision": "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
        },
        "roles": list(ROLES),
        **{"stageManifest": records["stage-manifest.json"],
           "workload": records["workload.json"],
           "generationCampaign": records["campaign.json"],
           "automaticPlanningManifest": records["automatic-planning.json"]},
        "invocations": [{"id": f"invocation-{index}"} for index in range(6)],
    }
    (bundle / "spec175-functional-manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8")
    return bundle


def run(bundle: Path, gate: str = "multi-provider") -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(PREFLIGHT), "--bundle", str(bundle), "--gate", gate],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )


def test_functional_bundle_preflight_accepts_complete_subject(tmp_path: Path) -> None:
    result = run(make_bundle(tmp_path))
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["status"] == "PASS"


def test_functional_bundle_rejects_control_only_bundle(tmp_path: Path) -> None:
    result = run(make_bundle(tmp_path, gate="control"))
    assert result.returncode != 0
    assert "gate does not match" in result.stdout


def test_functional_bundle_rejects_missing_invocation(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    path = bundle / "spec175-functional-manifest.json"
    value = json.loads(path.read_text())
    value["invocations"] = value["invocations"][:-1]
    path.write_text(json.dumps(value))
    result = run(bundle)
    assert result.returncode != 0
    assert "exactly six invocations" in result.stdout


def test_functional_bundle_rejects_provider_role_map(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    path = bundle / "spec175-functional-manifest.json"
    value = json.loads(path.read_text())
    value["providerRoleMap"] = {"P0": ROLES[0]}
    path.write_text(json.dumps(value))
    result = run(bundle)
    assert result.returncode != 0
    assert "Provider-role map is not allowed" in result.stdout


def test_functional_bundle_rejects_runtime_source_overlay(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    provider = bundle / "providers/provider-0.args"
    provider.write_text(provider.read_text() + "/source/provider.py\n")
    result = run(bundle)
    assert result.returncode != 0
    assert "runtime source overlay" in result.stdout


def test_functional_bundle_requires_external_model_mount(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    provider = bundle / "providers/provider-0.args"
    provider.write_text(
        provider.read_text().replace("=/model/", "=/tmp/model/"),
        encoding="utf-8",
    )
    result = run(bundle)
    assert result.returncode != 0
    assert "mounted below /model" in result.stdout


def test_functional_bundle_accepts_explicit_shell_exec_wrappers(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    for path in [bundle / "user.args", *sorted((bundle / "providers").glob("*.args"))]:
        argv = path.read_text(encoding="utf-8").splitlines()
        command = " ".join(shlex.quote(value) for value in argv)
        path.write_text(
            "bash\n-lc\nexport HOME=/evidence/home; exec " + command + "\n",
            encoding="utf-8",
        )
    result = run(bundle)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["status"] == "PASS"


def test_functional_bundle_rejects_shell_wrapper_without_exec(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    provider = bundle / "providers/provider-0.args"
    argv = provider.read_text(encoding="utf-8").splitlines()
    provider.write_text(
        "bash\n-lc\n" + " ".join(shlex.quote(value) for value in argv) + "\n",
        encoding="utf-8",
    )
    result = run(bundle)
    assert result.returncode != 0
    assert "shell wrapper must use an explicit exec" in result.stdout


def test_functional_bundle_rejects_controller_ready_before_start(tmp_path: Path) -> None:
    bundle = make_bundle(tmp_path)
    wrapper = bundle / "controller-wrapper.sh"
    wrapper.write_text(
        "print('NDNSF_DI_CONTROLLER_READY', flush=True)\n"
        "controller.start()\ncontroller.run()\n",
        encoding="utf-8",
    )
    result = run(bundle)
    assert result.returncode != 0
    assert "controller readiness marker precedes controller.start()" in result.stdout
