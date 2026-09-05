#!/usr/bin/env python3
"""Run one real native lifecycle probe in an isolated MiniNDN namespace.

Use the ordinary Spec180 Y-B input environment, a fresh CASE_OUTPUT_DIR, and
unshare with private user/net/mount/PID namespaces and tmpfs /run and /tmp.
This is focused T004 integration, never a qualification matrix.
"""
import argparse
import json
import os
from pathlib import Path
import site
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Experiments"))
import NDNSF_DI_YoloAckDriven_Minindn as runner


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", required=True,
                        choices=("provider-idle", "margin", "cancel"))
    args = parser.parse_args()
    uid_map = Path("/proc/self/uid_map").read_text().split()
    if len(uid_map) != 3 or uid_map[0] != "0" or uid_map[1] == "0" or uid_map[2] != "1":
        raise RuntimeError("lifecycle probe requires an isolated mapped user namespace")
    mounts = Path("/proc/self/mountinfo").read_text().splitlines()
    if not all(any(line.split()[4] == path and " - tmpfs " in line
                   for line in mounts) for path in ("/run", "/tmp")):
        raise RuntimeError("lifecycle probe requires private tmpfs /run and /tmp")

    output, inputs = runner.validate_inputs("Y-B", os.environ)
    runner._validate_native_library_closure()
    inputs = dict(inputs)
    binding = runner.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    inputs["runtime_publication_file"] = runner.build_runtime_publication_file(binding, inputs)
    runtime = runner.MiniNdnCaseRuntime(binding, inputs)
    processes = []
    try:
        ndn = runtime.start_network()
        runtime.configure_routing(ndn)
        runtime.initialize_keychains(ndn)
        spec = runtime.process_specs("control")[0]
        original = str(ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py")
        replacement = str(ROOT / "tests/fixtures/spec181/controller-lifecycle.py")
        if original not in spec.command:
            raise RuntimeError("controller executable not found in production command")
        env = runner._child_process_environment(os.environ)
        env["PYTHONPATH"] = ":".join([
            str(ROOT / "NDNSF-DistributedInference"),
            str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"),
            str(ROOT / "pythonWrapper"),
            str(ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2"),
            site.getusersitepackages(), env.get("PYTHONPATH", "")])
        env.setdefault("NDNSF_HANDLER_THREADS", "1")
        env.setdefault("NDNSF_ACK_THREADS", "1")
        env["PYTHONFAULTHANDLER"] = "1"
        env["SPEC181_LIFECYCLE_PROBE"] = args.mode
        env["NDN_CLIENT_TRANSPORT"] = "unix:///run/nfd/" + spec.node + ".sock"
        process, log_path = runtime._legacy_module().start(
            ndn.net[spec.node], spec.name, spec.command.replace(original, replacement),
            env, processes, output_dir=output,
            artifact_cache_root=output / "artifact-cache")
        code = process.wait(timeout=30)
        prefix = "SPEC181_LIFECYCLE_PASS "
        records = [json.loads(line[len(prefix):])
                   for line in log_path.read_text().splitlines() if line.startswith(prefix)]
        if code != 0 or len(records) != 1 or records[0].get("mode") != args.mode:
            raise RuntimeError(f"lifecycle probe failed: mode={args.mode} exit={code}")
    finally:
        runtime._legacy_module().stop_process_group(processes)
        for process, _log, _path in processes:
            process.wait(timeout=5)
        runtime.stop()
    # A successful child is insufficient until process collection and the
    # network cleanup have also returned successfully.
    (output / "lifecycle-probe-result.json").write_text(
        json.dumps({"status": "PASS", "layer": "focused-integration",
                    **records[0], "childrenCollected": True,
                    "networkCleanupCompleted": True},
                   sort_keys=True, indent=2) + "\n")
    print("SPEC181_LIFECYCLE_RESULT status=PASS mode=" + args.mode, flush=True)


if __name__ == "__main__":
    main()
