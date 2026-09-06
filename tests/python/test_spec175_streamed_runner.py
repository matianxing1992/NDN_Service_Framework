from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "packaging/ndnsf-di-container/jobs/spec175/run-streamed-generation.sh"


def _fixture(tmp_path: Path, groups: int = 7):
    bundle = tmp_path / "bundle"
    model = tmp_path / "model"
    output = tmp_path / "output"
    fake_bin = tmp_path / "bin"
    for path in (bundle, model, fake_bin):
        path.mkdir(parents=True)
    process_groups = []
    for index in range(groups):
        group_id = f"group-{index:02d}"
        relative = f"process-groups/{group_id}/user.args"
        user_args = bundle / relative
        user_args.parent.mkdir(parents=True)
        user_args.write_text("python3\nuser.py\n", encoding="utf-8")
        process_groups.append({
            "id": group_id,
            "userArgs": {"path": relative, "sha256": "sha256:" + "1" * 64},
        })
    (bundle / "spec175-functional-manifest.json").write_text(
        json.dumps({
            "schemaVersion": "ndnsf-di-spec175-functional-bundle-v2",
            "processGroups": process_groups,
        }),
        encoding="utf-8",
    )
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"exact-sif")
    log = tmp_path / "apptainer.jsonl"
    fake = fake_bin / "apptainer"
    fake.write_text(
        "#!/usr/bin/env python3\n"
        "import json, os, pathlib, sys\n"
        "path = pathlib.Path(os.environ['SPEC175_FAKE_LOG'])\n"
        "rows = path.read_text().splitlines() if path.exists() else []\n"
        "rows.append(json.dumps(sys.argv[1:]))\n"
        "path.write_text('\\n'.join(rows) + '\\n')\n"
        "fail_at = int(os.environ.get('SPEC175_FAKE_FAIL_AT', '0'))\n"
        "raise SystemExit(23 if fail_at and len(rows) == fail_at else 0)\n",
        encoding="utf-8",
    )
    fake.chmod(0o755)
    env = os.environ.copy()
    env.update({
        "PATH": f"{fake_bin}:{env['PATH']}",
        "SPEC175_SIF": str(sif),
        "SPEC175_SIF_SHA256": hashlib.sha256(sif.read_bytes()).hexdigest(),
        "SPEC175_BUNDLE": str(bundle),
        "SPEC175_OUTPUT": str(output),
        "SPEC175_GATE": "multi-provider",
        "SPEC175_MODEL_ROOT": str(model),
        "SPEC175_FAKE_LOG": str(log),
        "SLURM_JOB_ID": "spec175-test",
    })
    return env, output, log


def _run(env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(RUNNER)],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


def test_formal_runner_launches_each_process_group_with_isolated_evidence(
        tmp_path: Path) -> None:
    env, output, log = _fixture(tmp_path)
    result = _run(env)
    assert result.returncode == 0, result.stderr
    calls = [json.loads(line) for line in log.read_text().splitlines()]
    assert len(calls) == 7
    for index, call in enumerate(calls):
        bind = call[call.index("--bind") + 1]
        expected = output / "process-groups" / f"group-{index:02d}"
        assert f"{expected}:/evidence" in bind
        assert f"SPEC175_PROCESS_GROUP_ID=group-{index:02d}" in call
        assert (
            f"SPEC175_USER_ARGS=/bundle/process-groups/group-{index:02d}/user.args"
            in call
        )
    terminal = json.loads((output / "spec175-gate-terminal.json").read_text())
    assert terminal["status"] == "PASS"
    assert terminal["expectedProcessGroups"] == 7
    assert terminal["completedProcessGroups"] == 7
    assert [row["processGroupId"] for row in terminal["processGroups"]] == [
        f"group-{index:02d}" for index in range(7)
    ]


def test_formal_runner_stops_after_first_failed_process_group(tmp_path: Path) -> None:
    env, output, log = _fixture(tmp_path)
    env["SPEC175_FAKE_FAIL_AT"] = "3"
    result = _run(env)
    assert result.returncode == 23
    calls = [json.loads(line) for line in log.read_text().splitlines()]
    assert len(calls) == 3
    terminal = json.loads((output / "spec175-gate-terminal.json").read_text())
    assert terminal["status"] == "FAIL"
    assert terminal["exitCode"] == 23
    assert terminal["expectedProcessGroups"] == 7
    assert terminal["completedProcessGroups"] == 2
    assert [row["status"] for row in terminal["processGroups"]] == [
        "PASS", "PASS", "FAIL"
    ]
    assert not (output / "process-groups/group-03").exists()
