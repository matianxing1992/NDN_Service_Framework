"""Production role launcher through a fake Apptainer OS boundary, not a SIF gate."""
from pathlib import Path
import json
import os
import signal
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def prepared(tmp_path, rank=0):
    from runtime.yolo_worker import assigned_roles
    homes = {role: tmp_path / "private" / role
             for role in assigned_roles("two-node-gpu", rank)}
    for home in homes.values():
        (home / ".ndn/ndnsec-key-file").mkdir(parents=True)
        (home / ".ndn/pib.db").write_bytes(b"not-a-pib-layout-fixture")
        (home / ".ndn/ndnsec-key-file/key.privkey").write_bytes(b"not-a-private-key")
    launcher = tmp_path / "fake-apptainer"
    launcher.write_text("#!/usr/bin/python3\nimport os,sys\n"
                        "args=sys.argv[1:]; i=args.index('/usr/bin/env')\n"
                        "if '/opt/ndnsf-di/current/bin/nfd' in args:\n"
                        " os.execv(sys.executable,[sys.executable,'-c','import time;time.sleep(60)'])\n"
                        "os.execv('/usr/bin/env',args[i:])\n")
    launcher.chmod(0o700)
    for name in ("bundle", "public", "node"):
        (tmp_path / name).mkdir()
    return dict(profile={"apptainer": str(launcher), "sif": str(tmp_path / "fixture.sif")},
                mode="two-node-gpu", rank=rank, bundle=tmp_path / "bundle", homes=homes,
                public=tmp_path / "public", output=tmp_path / "output", node=tmp_path / "node",
                gpu_device="0", cleanup_seconds=1)


def test_real_child_is_started_by_role_launcher_and_cleaned_up(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        proc = worker.start_service("BackboneNeck", [sys.executable, "-c", "import time;time.sleep(60)"])
        assert proc.poll() is None
        launch, = worker.launches
        assert launch["role"] == "BackboneNeck"
        argv = launch["argv"]
        assert "--nv" in argv
        assert not any(":/artifacts:" in arg for arg in argv)
        assert "NDNSF_DI_STATE_ROOT=/output/state" in argv
        assert "NDNSF_DI_ORT_PROFILE_PREFIX=/output/ort/session" in argv
        assert not any(":/identities:rw" in item for item in argv)
    finally:
        rows = worker.close()
    assert rows[0]["reaped"] and not rows[0]["forced"]


@pytest.mark.parametrize("role,rank", [("BackboneNeck", 0), ("Merge", 0), ("DetectShard0", 1), ("DetectShard1", 1)])
def test_provider_starts_without_out_of_band_model_mount(tmp_path, role, rank):
    from runtime.yolo_worker import NodeRuntime
    inputs = prepared(tmp_path, rank=rank)
    worker = NodeRuntime(**inputs)
    try:
        worker.start_service(role, [sys.executable, "-c", "import time;time.sleep(60)"])
        assert not any(":/artifacts:" in arg for arg in worker.launches[0]["argv"])
    finally:
        worker.close()


def test_second_worker_cannot_open_the_same_provider_pib(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    inputs = prepared(tmp_path)
    first, second = NodeRuntime(**inputs), NodeRuntime(**inputs)
    try:
        first.start_service("BackboneNeck", [sys.executable, "-c", "import time;time.sleep(60)"])
        with pytest.raises(ValueError, match="ROLE_HOME_IN_USE"):
            second.start_service("BackboneNeck", [sys.executable, "-c", "pass"])
        assert second.launches == []
    finally:
        first.close()
        second.close()


def test_role_layout_preserves_four_distinct_providers():
    from runtime.yolo_worker import assigned_roles, PROVIDER_ROLES
    a = set(assigned_roles("two-node-gpu", 0))
    b = set(assigned_roles("two-node-gpu", 1))
    assert a & b == set()
    assert a & PROVIDER_ROLES == {"BackboneNeck", "Merge"}
    assert b & PROVIDER_ROLES == {"DetectShard0", "DetectShard1"}
    assert set(assigned_roles("local-cpu", 0)) & PROVIDER_ROLES == PROVIDER_ROLES


def test_merge_does_not_receive_gpu_or_model_projection(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        worker.start_service("Merge", [sys.executable, "-c", "import time;time.sleep(60)"])
        argv = worker.launches[0]["argv"]
        assert "--nv" not in argv
        assert not any(":/artifacts:" in arg for arg in argv)
    finally:
        worker.close()


def test_failed_spawn_releases_identity_lease(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    inputs = prepared(tmp_path)
    broken = dict(inputs, profile=dict(inputs["profile"], apptainer=str(tmp_path / "missing-apptainer")))
    first, retry = NodeRuntime(**broken), NodeRuntime(**inputs)
    try:
        with pytest.raises(FileNotFoundError):
            first.start_service("BackboneNeck", [sys.executable, "-c", "pass"])
        assert first.launches[0]["startError"] == "FileNotFoundError"
        child = retry.start_service("BackboneNeck", [sys.executable, "-c", "import time;time.sleep(60)"])
        assert child.poll() is None
    finally:
        first.close()
        retry.close()


@pytest.mark.parametrize("role", ["DetectShard0", "user", "root", "../controller"])
def test_wrong_node_or_non_service_role_never_spawns(tmp_path, role):
    from runtime.yolo_worker import NodeRuntime
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        with pytest.raises(ValueError, match="WORKER_SERVICE_ROLE"):
            worker.start_service(role, [sys.executable, "-c", "pass"])
        assert worker.launches == [] and worker.children.children == []
    finally:
        worker.close()


def test_ready_marker_requires_live_child_and_real_environment(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        worker.start_service("BackboneNeck", [sys.executable, "-c",
            "import os,time;print('ENV:'+os.environ['NDNSF_DI_STATE_ROOT']+':'"
            "+os.environ['CUDA_VISIBLE_DEVICES'],flush=True);time.sleep(60)"])
        worker.wait_marker("BackboneNeck", "ENV:/output/state:0", seconds=3)
    finally:
        worker.close()


def test_home_lease_is_visible_to_a_separate_process(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    inputs = prepared(tmp_path)
    worker = NodeRuntime(**inputs)
    try:
        worker.start_service("Merge", [sys.executable, "-c", "import time;time.sleep(60)"])
        script = ("import sys;sys.path.insert(0,sys.argv[1]);"
                  "from pathlib import Path;from runtime.identities import RoleHomeLease;"
                  "RoleHomeLease(Path(sys.argv[2]))")
        done = subprocess.run([sys.executable, "-c", script, str(Path(__file__).resolve().parents[1]),
                               str(inputs["homes"]["Merge"])], capture_output=True, text=True, timeout=3)
        assert done.returncode != 0 and "ROLE_HOME_IN_USE" in done.stderr
    finally:
        rows = worker.close()
    assert rows[0]["leaseReleased"]


def test_dead_process_cannot_qualify_with_a_ready_marker(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        proc = worker.start_service("Merge", [sys.executable, "-c", "print('READY',flush=True);raise SystemExit(3)"])
        assert proc.wait(timeout=3) == 3
        with pytest.raises(RuntimeError, match="CHILD_EXIT"):
            worker.wait_marker("Merge", "READY", seconds=1)
    finally:
        worker.close()


def test_peer_failure_wins_over_existing_ready_marker(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        worker.start_service("Merge", [sys.executable, "-c", "import time;print('READY',flush=True);time.sleep(60)"])
        worker.wait_marker("Merge", "READY", seconds=3)
        failed = tmp_path / "peer.failed"
        failed.touch()
        with pytest.raises(RuntimeError, match="PEER_FAILED"):
            worker.wait_marker("Merge", "READY", seconds=1, peer_failure=failed)
    finally:
        worker.close()


def test_marker_wait_times_out_and_closed_worker_cannot_restart(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        worker.start_service("Merge", [sys.executable, "-c", "import time;time.sleep(60)"])
        with pytest.raises(TimeoutError, match="WORKER_MARKER_TIMEOUT"):
            worker.wait_marker("Merge", "NEVER", seconds=0.05)
    finally:
        worker.close()
    with pytest.raises(ValueError, match="WORKER_CLOSED"):
        worker.start_service("repo", [sys.executable, "-c", "pass"])


def test_forwarder_uses_shared_config_and_explicit_port(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        child = worker.start_forwarder(20383)
        assert child.poll() is None
        config = (tmp_path / "output/nfd0/nfd.conf").read_text()
        assert "port 20383" in config and "path /node/nfd.sock" in config
        assert worker.launches[0]["argv"][-3:] == ["/opt/ndnsf-di/current/bin/nfd", "--config", "/output/nfd.conf"]
    finally:
        worker.close()


@pytest.mark.parametrize("port", [80, -1, 65536, True, "6363\nmalicious"])
def test_invalid_port_fails_before_forwarder_spawn(tmp_path, port):
    from runtime.yolo_worker import NodeRuntime
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        with pytest.raises(ValueError, match="NFD_PORT"):
            worker.start_forwarder(port)
        assert worker.launches == []
    finally:
        worker.close()


@pytest.mark.parametrize("field", ["bundle", "public", "node"])
def test_missing_working_directory_or_mount_rejected_before_spawn(tmp_path, field):
    from runtime.yolo_worker import NodeRuntime
    inputs = prepared(tmp_path)
    inputs[field] = tmp_path / "missing"
    with pytest.raises(ValueError, match="WORKER_DIRECTORY"):
        NodeRuntime(**inputs)


def test_out_of_band_model_parameter_is_not_a_supported_launch_path(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    with pytest.raises(TypeError, match="model_artifacts"):
        NodeRuntime(**dict(prepared(tmp_path), model_artifacts={"BackboneNeck": tmp_path}))


def test_output_cannot_overwrite_immutable_bundle(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    inputs = prepared(tmp_path)
    inputs["output"] = inputs["bundle"] / "output"
    with pytest.raises(ValueError, match="WORKER_OUTPUT_OVERLAP"):
        NodeRuntime(**inputs)


def test_role_output_alias_cannot_write_into_bundle(tmp_path):
    from runtime.yolo_worker import NodeRuntime
    inputs = prepared(tmp_path)
    worker = NodeRuntime(**inputs)
    (inputs["output"] / "Merge").symlink_to(inputs["bundle"], target_is_directory=True)
    try:
        with pytest.raises(ValueError, match="WORKER_DIRECTORY"):
            worker.start_service("Merge", [sys.executable, "-c", "pass"])
        assert worker.launches == []
        assert not (inputs["bundle"] / "state").exists()
    finally:
        worker.close()


def test_second_scheduler_signal_cannot_interrupt_owned_teardown(tmp_path):
    inputs = prepared(tmp_path)
    inputs["cleanup_seconds"] = 0.4
    plan = tmp_path / "test-plan.json"
    plan.write_text(json.dumps(inputs, default=str))
    pid_path = tmp_path / "owned.pid"
    script = r'''
import json,os,signal,sys,threading,time
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from runtime.yolo_worker import NodeRuntime
worker=NodeRuntime(**json.loads(Path(sys.argv[2]).read_text()))
child=worker.start_service("Merge",[sys.executable,"-c",
    "import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);print('READY',flush=True);time.sleep(60)"])
Path(sys.argv[3]).write_text(str(child.pid))
worker.wait_marker("Merge","READY",seconds=3)
thread=threading.Thread(target=lambda: (time.sleep(.1),os.kill(os.getpid(),signal.SIGTERM)))
thread.start()
try:
    print(json.dumps(worker.close()))
finally:
    thread.join()
'''
    try:
        done = subprocess.run([sys.executable, "-c", script,
            str(Path(__file__).resolve().parents[1]), str(plan), str(pid_path)],
            capture_output=True, text=True, timeout=5)
        assert done.returncode == 0, done.stderr
        row, = json.loads(done.stdout)
        assert row["forced"] and row["reaped"]
    finally:
        if pid_path.exists():
            try:
                os.killpg(int(pid_path.read_text()), signal.SIGKILL)
            except ProcessLookupError:
                pass
