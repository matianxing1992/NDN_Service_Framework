"""Execute the reviewed matrix; existing runner remains the oracle owner."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tests/minindn"))
import spec193_preflight as preflight
import importlib.util

runtime_audit_path = HERE / "network-runtime-audit.py"
runtime_spec = importlib.util.spec_from_file_location("runtime_audit", runtime_audit_path)
runtime_audit = importlib.util.module_from_spec(runtime_spec)
runtime_spec.loader.exec_module(runtime_audit)
case_audit_path = HERE / "network-case-oracles.py"
case_spec = importlib.util.spec_from_file_location("case_oracles", case_audit_path)
case_audit = importlib.util.module_from_spec(case_spec)
case_spec.loader.exec_module(case_audit)


def read(path):
    return json.loads(path.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    output = args.output_root.resolve()
    output.relative_to(ROOT / "specs/202-distributedrepo-convergence/evidence")
    if output.exists() or os.geteuid() == 0 or sys.flags.optimize:
        parser.error("use a fresh output root, unprivileged parent and enabled assertions")
    plan_path = HERE / "network-execution-plan.json"
    plan = read(plan_path)
    expected = plan["source_candidate_sha256"]
    diagnostic = HERE.parent / "diagnostic-e529c434-20261010"
    normal_path = diagnostic / "normal-all-seven-r49/manifest.json"
    dynamic_path = diagnostic / "native-all-seven-r47/manifest.json"
    normal, dynamic = read(normal_path), read(dynamic_path)
    release_path = HERE / "qualification-release.json"
    release = read(release_path)
    assert release["status"] == "PASS"
    assert release["source_candidate_sha256"] == expected
    for path, digest in release["inputs"].items():
        assert preflight.sha256(ROOT / path) == digest, "release input changed: " + path
    for manifest in (normal, dynamic):
        assert manifest["state"] == "PASS" and len(manifest["results"]) == 7
        assert all(row["exit"] == 0 and row["actual_maps_captured"]
                   for row in manifest["results"])
    assert all(row["sanitizer_reports"] == 0 for row in dynamic["results"])
    python = read(diagnostic / "python-conformance-r50.json")
    assert python["exit"] == 0 and python["files"] == 33 and python["source"] == expected
    admission = (diagnostic / "admission-oracles-r51.log").read_text()
    assert "Ran 41 tests" in admission and "\nOK\n" in admission
    reproduction = read(diagnostic / "reproduction-r54.json")
    assert reproduction["status"] == "PASS" and reproduction["original_global_restored"] is True
    assert reproduction["source"] == expected
    assert len(reproduction["records"]) == 10
    assert all(row["exit"] == 0 for row in reproduction["records"])
    assert all(row.get("processes") for row in reproduction["records"][1:8])
    review = read(diagnostic / "scoped-convergence-r55.json")
    assert review["source_candidate_sha256"] == expected
    assert not review["controlling_code_findings_in_reviewed_delta"]
    assert release["convergence_status"] == "PASS_READY_FOR_NETWORK"
    observer = HERE / "network-runtime-observer.py"
    assert preflight.source_candidate(ROOT)[0] == expected
    assert preflight.sha256(ROOT / "build/spec202-source-build-receipt.json") == plan["build_receipt_sha256"]
    assert preflight.sha256(ROOT / "tests/minindn/run_spec192_real_cpp_repo.py") == plan["runner_sha256"]
    assert plan["rounds"] == [1, 2] and len(plan["scenarios"]) == 68
    assert len(plan["stable_ids"]) == 59 and len(plan["profiles"]) == 6
    environment = os.environ.copy()
    trace = ("NDN_LOG", "NDNSF_SPEC192_NDN_LOG_TRACE_PROVIDER",
             "NDNSF_SPEC192_NDN_LOG_TRACE_CHILD", "NDNSF_SPEC192_SELECTED_REQUEST_TRACE",
             "NDNSF_SPEC192_SELECTED_REQUEST_TRACE_CHILD", "LD_PRELOAD",
             "ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS",
             "NDNSF_CONTROL_TIMING", "NDNSF_TIMELINE_TRACE")
    for name in trace:
        environment.pop(name, None)
    prefix = ["sudo", "-n", "-E", "/usr/bin/env"]
    for name in trace:
        prefix += ["-u", name]
    prefix += [
        "PYTHONPATH=" + ":".join(str(ROOT / name) for name in (
            "pythonWrapper", "NDNSF-DistributedRepo/pythonWrapper", "NDNSF-DistributedInference")),
        "LD_LIBRARY_PATH=/usr/local/src/spec192-ndn-svs-9f2d8a4/build:"
            + str(ROOT / "build") + ":/usr/local/lib:/opt/gstreamer-1.20/lib",
        "/usr/bin/python3", str(observer)]
    output.mkdir(parents=True)
    index = {"status": "RUNNING", "source_candidate_sha256": expected,
             "plan_sha256": preflight.sha256(plan_path),
             "driver_sha256": preflight.sha256(Path(__file__)),
             "observer_sha256": preflight.sha256(observer),
             "runtime_audit_sha256": preflight.sha256(runtime_audit_path),
             "case_audit_sha256": preflight.sha256(case_audit_path),
             "release_sha256": preflight.sha256(release_path),
             "profile_locks": {}, "runs": []}
    index_path = output / "qualification-index.json"

    def save():
        index_path.write_text(json.dumps(index, indent=2) + "\n")

    save()
    try:
        for round_id in plan["rounds"]:
            for scenario in plan["scenarios"]:
                assert preflight.source_candidate(ROOT)[0] == expected
                label, profile = scenario["label"], scenario["profile"]
                directory = output / ("round-" + str(round_id)) / "runs" / label
                directory.parent.mkdir(parents=True, exist_ok=True)
                assert not directory.exists()
                cap = scenario["outer_timeout_seconds"]
                assert isinstance(cap, int) and 0 < cap <= 21600
                argv = ["timeout", "--signal=INT", "--kill-after=60s", str(cap) + "s"] \
                    + prefix + ["--record", str(directory.parent / (label + ".runtime.json")),
                    "--", "/usr/bin/python3", str(ROOT / "tests/minindn/run_spec192_real_cpp_repo.py"),
                    "--campaign-case", scenario["campaign_case"],
                    "--topology-file", str(ROOT / plan["profiles"][profile]["topology"]),
                    "--output-dir", str(directory)] + scenario["flags"]
                lock = index["profile_locks"].get(profile)
                if lock is not None:
                    argv += ["--candidate-lock", lock["path"]]
                persistence = scenario["persistence"]
                if persistence:
                    durable = output / ("round-" + str(round_id)) / "durable" / persistence
                    argv += ["--durable-repo-root", str(durable)]
                    if scenario["campaign_case"].endswith("READ"):
                        seed = "PERSIST_SEED" if persistence == "object" else "PERSIST_ARTIFACT_SEED"
                        seed_manifest = directory.parent / seed / "run-manifest.json"
                        assert durable.is_dir() and read(seed_manifest)["status"] == "PASS"
                        argv += ["--seed-run-manifest", str(seed_manifest)]
                    else:
                        assert not durable.exists()
                log = directory.parent / (label + ".launcher.log")
                started = time.monotonic()
                with log.open("w") as stream:
                    result = subprocess.run(argv, cwd=ROOT, env=environment,
                                            stdout=stream, stderr=subprocess.STDOUT)
                record = {"round": round_id, "label": label, "profile": profile,
                    "stable_ids": scenario["stable_ids"], "argv": argv,
                    "exit_status": result.returncode, "seconds": time.monotonic() - started,
                    "output": str(directory), "launcher_log_sha256": preflight.sha256(log)}
                index["runs"].append(record)
                save()
                assert result.returncode == 0, "runner failed: " + label
                runtime_path = directory.parent / (label + ".runtime.json")
                runtime = read(runtime_path)
                assert runtime["status"] == "CAPTURED_PENDING_COVERAGE_AUDIT"
                assert runtime["runner_exit_status"] == 0 and not runtime["signals"]
                assert not runtime["errors"]
                assert runtime["observer_sha256"] == index["observer_sha256"]
                record["runtime_record_sha256"] = preflight.sha256(runtime_path)
                manifest_path = directory / "run-manifest.json"
                manifest = read(manifest_path)
                if scenario["campaign_case"] == "A05":
                    record["case_oracle_audit"] = case_audit.audit_a05(directory, manifest)
                record["runtime_audit"] = runtime_audit.audit(manifest_path, runtime_path,
                    ROOT / "build/spec202-source-build-receipt.json",
                    Path("/usr/local/src/spec192-ndn-svs-9f2d8a4/build/libndn-svs.so"))
                assert manifest["status"] == "PASS" and manifest["minindn_cleanup"] == "PASS"
                assert manifest["source_candidate_sha256"] == expected
                profile_path = directory / "preflight-candidate-lock.json"
                actual = read(profile_path)
                assert actual["source_candidate_sha256"] == expected and actual["spec"] == "202"
                assert manifest["preflight_candidate_lock_sha256"] == actual["candidate_lock_sha256"]
                if lock is None:
                    index["profile_locks"][profile] = {"path": str(profile_path),
                        "candidate_lock_sha256": actual["candidate_lock_sha256"]}
                else:
                    assert actual["candidate_lock_sha256"] == lock["candidate_lock_sha256"]
                captures = manifest["pcap_traces"]
                assert len(captures) == len(manifest["hosts"])
                assert all(capture["bytes"] > 0 for capture in captures)
                assert all(stop["stop_method"] != "SIGKILL-fallback"
                           for stop in manifest["cleanup_results"])
                record["run_manifest_sha256"] = preflight.sha256(manifest_path)
                record["candidate_lock_sha256"] = actual["candidate_lock_sha256"]
                record["status"] = "RUNNER_AND_CLOSURE_PASS"
                save()
                print("round", round_id, label, "runner and closure PASS", flush=True)
        assert len(index["runs"]) == 136 and len(index["profile_locks"]) == 6
        assert preflight.source_candidate(ROOT)[0] == expected
        index["status"] = "ALL_RUNNERS_PASS_PENDING_MATRIX_EVIDENCE_AUDIT"
        save()
    except BaseException as error:
        index["status"] = "FAIL_STOPPED"
        index["error"] = repr(error)
        save()
        raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
