"""Explicit opt-in qualification through the maintained installed MiniNDN path.

Set NDNSF_PERSON_QUALIFICATION_CONFIG to an absolute local input-config path.
Ordinary discovery never launches a model, network, or privileged process.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
CGROUP_ROOT = next((path for path in (Path("/sys/fs/cgroup"), Path("/sys/fs/cgroup/unified"))
                    if (path / "cgroup.controllers").is_file()), None)
sys.path.insert(0, str(ROOT))

from Experiments.UAV.person_tracking_metrics import _load, evaluate_person_tracking
from Experiments.UAV.tracking_artifacts import digest_file


def _check_output_socket_path(output):
    """Reject a run root that cannot hold the longest maintained person socket."""
    socket_path = output / "private/ground-station-display/display.sock"
    canonical_path = output.resolve() / "private/ground-station-display/display.sock"
    if max(len(os.fsencode(socket_path)), len(os.fsencode(canonical_path))) >= 108:
        raise ValueError("output root exceeds Linux AF_UNIX pathname capacity: " + str(socket_path))
    return socket_path


def _service_status(unit):
    properties = ("LoadState", "ActiveState", "SubState", "MainPID", "ControlGroup",
                  "ExecMainCode", "ExecMainStatus", "Result")
    probe = subprocess.run(["systemctl", "show", unit] +
                           ["--property=" + key for key in properties],
                           capture_output=True, text=True, timeout=3)
    state = dict(line.split("=", 1) for line in probe.stdout.splitlines() if "=" in line)
    if not state or (probe.returncode and state.get("LoadState") != "not-found"):
        raise ValueError("cannot inspect owned service: " + probe.stderr[:1024])
    return state


def _proc_identity(pid):
    """Return a bounded Linux process identity for the active worker oracle."""
    proc = Path("/proc") / str(pid)
    try:
        stat_text = (proc / "stat").read_text(encoding="ascii")
        fields = stat_text[stat_text.rfind(")") + 2:].split()
        if len(fields) <= 19:
            return None
        argv = [part.decode("utf-8", errors="replace")
                for part in (proc / "cmdline").read_bytes().split(b"\0") if part]
        cgroups = {}
        for line in (proc / "cgroup").read_text(encoding="ascii").splitlines():
            hierarchy, controllers, path = line.split(":", 2)
            cgroups[(hierarchy, controllers)] = path
        return {"state": fields[0], "parentPid": int(fields[1]),
                "startTimeTicks": int(fields[19]), "argv": argv, "cgroups": cgroups}
    except (OSError, ValueError, IndexError):
        return None


def _start_and_wait_service(command, unit, log, timeout=1340, observer=None):
    """Capture service status without a waiting CLI that resembles an owned app."""
    started = subprocess.run(command, capture_output=True, text=True, timeout=10)
    if started.returncode:
        with log.open("a") as stream:
            stream.write(started.stderr[:4096])
        return started, {"startReturnCode": started.returncode, "startError": started.stderr[:4096]}
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = _service_status(unit)
        if state.get("LoadState") == "not-found":
            raise RuntimeError("owned service disappeared before status capture: " + str(state))
        if state.get("SubState") in ("exited", "failed", "dead"):
            code, status = state.get("ExecMainCode"), state.get("ExecMainStatus")
            if not isinstance(status, str) or not status.isdecimal():
                raise RuntimeError("owned service exit status unavailable: " + str(state))
            if code == "1":  # CLD_EXITED
                return subprocess.CompletedProcess(command, int(status)), state
            if code in ("2", "3"):  # CLD_KILLED or CLD_DUMPED
                return subprocess.CompletedProcess(command, -int(status)), state
            raise RuntimeError("owned service did not report a process exit: " + str(state))
        if observer is not None:
            observer(state)
        time.sleep(.25)
    raise subprocess.TimeoutExpired(command, timeout)


def _drain_owned_service(unit):
    """Stop only this unit and confirm its entire v2 cgroup has drained."""
    if CGROUP_ROOT is None:
        raise RuntimeError("systemd cgroup2 mount unavailable; do not start an experiment")
    group = CGROUP_ROOT / "system.slice" / (unit + ".service")
    record = {"unit": unit, "cgroup": str(group), "drained": False, "actions": []}

    def inspect():
        try:
            state = _service_status(unit)
            if state.get("ControlGroup") and state["ControlGroup"] != "/system.slice/" + unit + ".service":
                raise ValueError("owned unit ControlGroup differs from its explicit slice")
            populated = True
            if not group.exists():
                populated = False
            else:
                with (group / "cgroup.events").open() as stream:
                    events = dict(line.split() for line in stream.read(4096).splitlines())
                populated = events.get("populated") != "0"
            gone = (state.get("LoadState") == "not-found" or
                    (state.get("ActiveState") in ("inactive", "failed") and state.get("MainPID") == "0"))
            record.setdefault("initialState", state)
            record.setdefault("initialCgroupPopulated", populated)
            record.update(lastState=state, cgroupPopulated=populated, drained=gone and not populated)
            return record["drained"]
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            record.update(drained=False, inspectionError=type(error).__name__ + ": " + str(error))
            return False

    try:
        if inspect():
            if record["lastState"].get("ActiveState") == "failed":
                released = subprocess.run(["systemctl", "reset-failed", unit],
                                          capture_output=True, text=True, timeout=3)
                record["actions"].append({"action": "release-failed-unit",
                                          "returncode": released.returncode})
                inspect()
            return record
        for action, command, timeout in (
                ("stop", ["systemctl", "stop", unit], 20),
                ("kill-owned-unit", ["systemctl", "kill", "--kill-who=all", "--signal=KILL", unit], 5)):
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
                record["actions"].append({"action": action, "returncode": result.returncode,
                                          "stderr": result.stderr[:1024]})
            except (OSError, subprocess.SubprocessError) as error:
                record["actions"].append({"action": action, "error": type(error).__name__ + ": " + str(error)})
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if inspect():
                    return record
                time.sleep(.1)
        if group.exists():
            with (group / "cgroup.kill").open("w") as stream:
                stream.write("1")
            record["actions"].append({"action": "kill-owned-cgroup"})
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if inspect():
                    return record
                time.sleep(.1)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        record["error"] = type(error).__name__ + ": " + str(error)
    return record


@unittest.skipUnless(os.environ.get("NDNSF_PERSON_QUALIFICATION_CONFIG"),
                     "explicit person MiniNDN qualification config required")
class PersonTrackingQualificationTests(unittest.TestCase):
    def _normal(self, label):
        self.assertEqual(os.geteuid(), 0, "explicit MiniNDN qualification requires root")
        config_path = Path(os.environ["NDNSF_PERSON_QUALIFICATION_CONFIG"])
        self.assertTrue(config_path.is_absolute())
        config = _load(config_path, 65536)
        paths = {"sourceManifest", "evaluationManifest", "dataAdmission", "trackingProfile",
                 "detectorModel", "reidModel", "workerPython", "outputRoot"}
        self.assertEqual(set(config), paths | {"headless"})
        self.assertIs(type(config["headless"]), bool)
        for key in paths:
            self.assertIsInstance(config[key], str)
            self.assertTrue(Path(config[key]).is_absolute(), key)
            if key != "outputRoot":
                self.assertTrue(Path(config[key]).is_file(), key)
        self.assertTrue(shutil.which("systemd-run"), "bounded service owner unavailable")
        self.assertTrue(os.access("/bin/bash", os.X_OK), "MiniNDN requires the declared shell")
        self.assertIsNotNone(CGROUP_ROOT, "requires the systemd cgroup2 mount")
        output_root = Path(config["outputRoot"])
        _check_output_socket_path(output_root / label)
        output_root.mkdir(parents=True, exist_ok=True)
        output = output_root / label
        self.assertFalse(output.exists(), "preserve existing experiment attempt: " + str(output))
        if label == "N2":
            first_assessment = _load(output_root / "N1-assessment.json", 65536)
            first = _load(output_root / "N1/person-run-receipt.json", 16 * 1024 * 1024)
            counts = first.get("counts", {})
            first_gates = first_assessment.get("qualification", {}).get("gates", {})
            self.assertTrue(first_assessment.get("runnerReturnCode") == 0 and
                            "assessmentError" not in first_assessment and
                            first_assessment.get("serviceCleanup", {}).get("drained") is True and
                            first_gates and all(value is True for key, value in first_gates.items()
                                                if key != "accuracy") and
                            first.get("terminal") is True and
                            counts.get("committedWindows") == 120 and
                            counts.get("receivedFrames") == counts.get("displayedFrames") == 360 and
                            first.get("remainingOwnedProcesses") == 0 and
                            first.get("cleanupErrors") == [],
                            "N2 NOT_RUN: diagnose N1 incomplete runtime before another launch")
        command = [sys.executable, "-m", "Experiments.UAV.run_multicamera_tracking_demo",
                   "--source-manifest", config["sourceManifest"],
                   "--tracking-profile", config["trackingProfile"],
                   "--model", config["detectorModel"], "--reid-model", config["reidModel"],
                   "--worker-python", config["workerPython"], "--output-dir", str(output),
                   "--run", "--run-seconds", "60", "--window-count", "120",
                   "--global-deadline-ms", "900000", "--fault", "none"]
        if config["headless"]:
            command.append("--headless")
        unit = "ndnsf-person-" + hashlib.sha256(str(output).encode()).hexdigest()[:8] + "-" + secrets.token_hex(8)
        unit_probe = subprocess.run(["systemctl", "show", unit, "--property=LoadState"],
                                    capture_output=True, text=True, timeout=5)
        self.assertEqual(unit_probe.stdout.strip(), "LoadState=not-found",
                         "preserve preexisting unit: " + unit)
        log = output_root / (label + "-launcher.log")
        service = ["systemd-run", "--quiet", "--unit=" + unit,
                   "--working-directory=" + str(ROOT), "--property=RuntimeMaxSec=1300",
                   "--property=KillMode=control-group", "--property=KillSignal=SIGINT",
                   "--property=TimeoutStopSec=15", "--property=Slice=system.slice", "--property=Restart=no",
                   "--property=RemainAfterExit=yes", "--property=StandardOutput=append:" + str(log),
                   "--property=StandardError=inherit", "--setenv=SHELL=/bin/bash"]
        environment_keys = ("PYTHONPATH",) if config["headless"] else (
            "PYTHONPATH", "DISPLAY", "XAUTHORITY")
        for key in environment_keys:
            self.assertTrue(os.environ.get(key), "missing explicit experiment environment: " + key)
            service.append("--setenv=" + key + "=" + os.environ[key])
        service.extend(command)
        plan = {"case": label, "command": command, "serviceCommand": service,
                "inputs": {key: {"path": config[key], "digest": digest_file(Path(config[key]))}
                           for key in paths - {"outputRoot"}},
                "testSourceDigest": digest_file(Path(__file__)),
                "configurationDigest": digest_file(config_path),
                "runnerSourceDigest": digest_file(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
                "evaluatorSourceDigest": digest_file(ROOT / "Experiments/UAV/person_tracking_metrics.py")}
        with (output_root / (label + "-plan.json")).open("x") as stream:
            json.dump(plan, stream, indent=2)
            stream.write("\n")
        if label == "N2":
            first_plan = _load(output_root / "N1-plan.json", 65536)
            for key in ("inputs", "testSourceDigest", "configurationDigest",
                        "runnerSourceDigest", "evaluatorSourceDigest"):
                self.assertEqual(plan[key], first_plan[key], "N2 frozen condition changed: " + key)
            first_manifest = _load(output_root / "N1/manifest.json", 4 * 1024 * 1024)
            from Experiments.UAV.run_multicamera_tracking_demo import _verify_person_plan
            _verify_person_plan(first_manifest)
            descriptor = _load(Path(first_manifest["personTracking"]["computationManifest"]), 65536)
            for name, digest in descriptor["nativeInventory"].items():
                self.assertEqual(digest_file(Path(name.split(":", 1)[1])), digest,
                                 "N2 installed native closure changed: " + name)
        result, service_state, execution_error = None, None, None
        try:
            log.touch(exist_ok=False)
            result, service_state = _start_and_wait_service(service, unit, log)
        except BaseException as error:
            execution_error = error
            try:
                service_state = _service_status(unit)
            except (OSError, ValueError, subprocess.SubprocessError):
                pass  # The cleanup record retains any subsequent inspection failure.
        finally:
            cleanup = _drain_owned_service(unit)
        receipt_path = output / "person-run-receipt.json"
        assessment = {"case": label, "runnerReturnCode": result.returncode if result else None,
                      "launcherLog": str(log), "serviceTerminalState": service_state,
                      "serviceCleanup": cleanup}
        if execution_error is not None:
            assessment["executionError"] = type(execution_error).__name__ + ": " + str(execution_error)
            with (output_root / (label + "-assessment.json")).open("x") as stream:
                json.dump(assessment, stream, indent=2)
                stream.write("\n")
            raise execution_error
        metrics = None
        try:
            metrics = evaluate_person_tracking(
                output / "private/ground-station/presentation", Path(config["evaluationManifest"]),
                run_receipt=receipt_path, data_admission=Path(config["dataAdmission"]))
            with (output_root / (label + "-metrics.json")).open("x") as stream:
                json.dump(metrics, stream, indent=2, allow_nan=False)
                stream.write("\n")
            assessment["qualification"] = metrics["qualification"]
            assessment["frozenConditionMatch"] = label == "N1" or all(
                metrics.get(key) == first.get(key) for key in
                ("sourceManifestDigest", "profileDigest", "computationDigest", "datasetOrigin"))
        except (OSError, ValueError, KeyError, TypeError) as error:
            assessment["assessmentError"] = type(error).__name__ + ": " + str(error)
        with (output_root / (label + "-assessment.json")).open("x") as stream:
            json.dump(assessment, stream, indent=2)
            stream.write("\n")
        self.assertEqual(result.returncode, 0, "native launcher failed; inspect " + str(log))
        self.assertTrue(cleanup["drained"], "owned service did not drain: " + str(cleanup))
        self.assertIsNotNone(metrics, assessment.get("assessmentError"))
        self.assertTrue(assessment["frozenConditionMatch"], "N1/N2 computation or data conditions differ")
        self.assertTrue(metrics["qualification"]["qualified"],
                        "formal qualification failed: " + str(metrics["qualification"]["failedGates"]))

    def test_normal_n1(self):
        self._normal("N1")

    def test_normal_n2(self):
        self._normal("N2")


@unittest.skipUnless(os.environ.get("NDNSF_PERSON_FAULT_CONFIG"),
                     "explicit person fault config required")
class PersonTrackingFaultTests(unittest.TestCase):
    """Installed preflight and network faults; never claim normal accuracy qualification."""

    def _installed_case(self, root, config, label, profile, detector, *, run,
                        fault="none", network_expected=False, observer=None):
        output = root / label
        self.assertFalse(output.exists(), "preserve previous attempt")
        _check_output_socket_path(output)
        command = [config["workerPython"], "-m", "Experiments.UAV.run_multicamera_tracking_demo",
                   "--source-manifest", str(root / "source-manifest.json"),
                   "--tracking-profile", str(profile), "--model", str(detector),
                   "--reid-model", config["reidModel"],
                   "--worker-python", config["workerPython"], "--output-dir", str(output),
                   "--headless", "--run-seconds", "1.5", "--window-count", "3",
                   "--global-deadline-ms", "90000", "--fault", fault,
                   "--run" if run else "--preflight"]
        unit = "ndnsf-person-fault-" + secrets.token_hex(8)
        self.assertEqual(_service_status(unit).get("LoadState"), "not-found")
        log = root / (label + ".log")
        log.touch(exist_ok=False)
        service = ["systemd-run", "--quiet", "--unit=" + unit,
                   "--working-directory=" + str(ROOT), "--property=RuntimeMaxSec=180",
                   "--property=KillMode=control-group", "--property=KillSignal=SIGINT",
                   "--property=TimeoutStopSec=15", "--property=Slice=system.slice",
                   "--property=Restart=no", "--property=RemainAfterExit=yes",
                   "--property=StandardOutput=append:" + str(log),
                   "--property=StandardError=inherit", "--setenv=SHELL=/bin/bash",
                   "--setenv=PYTHONPATH=" + os.environ["PYTHONPATH"]] + command
        plan = {"case": label, "command": command, "serviceCommand": service,
                "sourceDigest": digest_file(root / "source-manifest.json"),
                "profileDigest": digest_file(profile), "detectorDigest": digest_file(detector),
                "reidDigest": digest_file(Path(config["reidModel"])),
                "testSourceDigest": digest_file(Path(__file__)),
                "runnerSourceDigest": digest_file(ROOT / "Experiments/UAV/run_multicamera_tracking_demo.py"),
                "installedNodeDigest": digest_file(Path(shutil.which("UavTrackingNode"))),
                "expectedNetworkStarted": network_expected,
                "externalObserver": observer is not None}
        with (root / (label + "-plan.json")).open("x") as stream:
            json.dump(plan, stream, indent=2)
        result, state, execution_error = None, None, None
        try:
            result, state = _start_and_wait_service(
                service, unit, log, timeout=220,
                observer=(lambda state: observer(output, unit, state)) if observer else None)
        except BaseException as error:
            execution_error = error
        finally:
            cleanup = _drain_owned_service(unit)
        record = {"case": label, "exitCode": result.returncode if result else None,
                  "serviceState": state, "cleanup": cleanup,
                  "networkStarted": (output / "person-network-attempt.json").exists(),
                  "computeStarted": (output / "compute.log").exists(),
                  "workerRootCreated": (output / "private/compute/worker").exists(),
                  "securityProvisioned": ((output / "private/keychain").exists() or
                                          (output / "private/config/root.cert").exists()),
                  "logDigest": digest_file(log)}
        if execution_error is not None:
            record["executionError"] = type(execution_error).__name__ + ": " + str(execution_error)
        with (root / (label + "-execution.json")).open("x") as stream:
            json.dump(record, stream, indent=2)
        if execution_error is not None:
            raise execution_error
        self.assertTrue(cleanup["drained"], cleanup)
        self.assertEqual(record["networkStarted"], network_expected, record)
        if not network_expected:
            for key in ("computeStarted", "workerRootCreated", "securityProvisioned"):
                self.assertFalse(record[key], (label, key))
        return result.returncode, output

    def _network_fixture(self, name):
        self.assertEqual(os.geteuid(), 0, "explicit MiniNDN fault validation requires root")
        self.assertIsNotNone(CGROUP_ROOT)
        config_path = Path(os.environ["NDNSF_PERSON_FAULT_CONFIG"])
        self.assertTrue(config_path.is_absolute())
        config = _load(config_path, 65536)
        self.assertEqual(set(config), {"sourceManifest", "trackingProfile", "detectorModel",
                                      "reidModel", "workerPython", "outputRoot"})
        for key, value in config.items():
            self.assertIsInstance(value, str)
            self.assertTrue(Path(value).is_absolute(), key)
            if key != "outputRoot":
                self.assertTrue(Path(value).is_file(), key)
        root = Path(config["outputRoot"]) / name
        _check_output_socket_path(root / "recovery")
        self.assertFalse(root.exists(), "use a new fault fixture root")
        root.mkdir(parents=True)
        source = _load(Path(config["sourceManifest"]), 65536)
        self.assertGreaterEqual(source["durationUs"], 1500000)
        source["durationUs"] = 1500000
        for camera in source["cameras"]:
            path = Path(camera["videoPath"])
            if not path.is_absolute():
                camera["videoPath"] = str((Path(config["sourceManifest"]).parent / path).resolve())
        (root / "source-manifest.json").write_text(json.dumps(source, sort_keys=True))
        profile, detector = Path(config["trackingProfile"]), Path(config["detectorModel"])
        expected_profile = _load(profile, 16384)
        self.assertEqual(expected_profile["requestedDevice"], "cpu")
        return root, config, profile, detector

    def test_f6_precommit_cancel_then_fresh_mission_recovers(self):
        root, config, profile, detector = self._network_fixture("F6")
        code, output = self._installed_case(
            root, config, "cancel", profile, detector, run=True,
            fault="user-cancel-before-person-commit", network_expected=True)
        compute = (output / "compute.log").read_text()
        gs = (output / "ground-station.log").read_text()
        receipt = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
        manifests = list((output / "private/compute/worker/transactions").rglob("manifest.json"))
        observation = {"status": "OBSERVED_NOT_QUALIFIED", "exitCode": code,
                       "computeLogDigest": digest_file(output / "compute.log"),
                       "gsLogDigest": digest_file(output / "ground-station.log"),
                       "workerCalls": compute.count("SPEC210_TRACKING_WORKER_INVOKED"),
                       "transactionManifests": [str(path) for path in manifests],
                       "receiptDigest": digest_file(output / "person-run-receipt.json")}
        (root / "F6-precommit-observation.json").write_text(json.dumps(observation, indent=2))
        self.assertEqual(code, 1, "require actual cancellation, not startup failure")
        self.assertTrue(receipt["terminal"])
        self.assertEqual(receipt["outcome"], "collaboration-cancelled")
        self.assertEqual(receipt["counts"]["committedWindows"], 0)
        self.assertEqual(receipt["counts"]["receivedFrames"], 0)
        self.assertEqual(receipt["counts"]["displayedFrames"], 0)
        for marker in (
                "SPEC219_PERSON_PRECOMMIT_READY window=window-0",
                "SPEC210_USER_CANCEL_ACK_QUEUED window=window-0 authority_revoked=true",
                "SPEC210_USER_CANCEL_CANDIDATE_DISCARDED window=window-0",
                "SPEC210_USER_CANCEL_ROLE_CLOSED"):
            self.assertEqual(compute.count(marker), 1, marker)
        self.assertEqual(observation["workerCalls"], 1)
        self.assertEqual(manifests, [])
        events = [json.loads(line[len("SPEC219_TRACE "):]) for line in compute.splitlines()
                  if line.startswith("SPEC219_TRACE ")]
        self.assertFalse(any(event.get("event") == "committed" for event in events))
        self.assertEqual(gs.count("SPEC210_CANCEL_ACK_CONFIRMED window=0"), 1)
        self.assertEqual(gs.count("SPEC191_TRACKING_CANCELLED window=0 accepted=true"), 1)
        self.assertNotIn("status=success", gs)
        projection_path = output / "private/ground-station/presentation/latest.json"
        projection = _load(projection_path, 4 * 1024 * 1024)
        self.assertEqual(projection["state"], "failure")
        self.assertEqual(projection["windowId"], "window-0")
        self.assertEqual(projection["reason"], "collaboration cancelled")
        self.assertEqual(projection["runId"], output.name)
        self.assertEqual(projection["missionId"], receipt["missionId"])
        from Experiments.UAV.run_multicamera_tracking_demo import _person_failure_display_presented
        self.assertTrue(_person_failure_display_presented(
            (output / "ground-station-display.log").read_text(), run_id=output.name,
            window_index=0, projection_digest=digest_file(projection_path), mode="headless"))
        code, recovered_output = self._installed_case(
            root, config, "recovery", profile, detector, run=True, network_expected=True)
        self.assertEqual(code, 0)
        recovery = _load(recovered_output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(recovery["terminal"])
        self.assertEqual(recovery["outcome"], "collaboration-result")
        for key, expected in (("committedWindows", 3), ("receivedFrames", 9),
                              ("displayedFrames", 9)):
            self.assertEqual(recovery["counts"][key], expected)
        self.assertEqual(recovery["failedWindows"], 0)
        self.assertNotEqual(receipt["missionId"], recovery["missionId"])
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(receipt[key], recovery[key], key)
        self.assertEqual((recovered_output / "compute.log").read_text().count(
            "SPEC210_TRACKING_WORKER_INVOKED"), 3)
        sys.path.insert(0, str(Path(shutil.which("UavTrackingNode")).parent.parent /
                               "libexec/ndnsf-uav/tracking"))
        from tracking_display import PersonPresentationReader
        reader = PersonPresentationReader(
            recovered_output / "private/ground-station/presentation/context.json")
        for window in range(3):
            frames, failure = reader.load(reader.root / f"window-{window}/presentation.json")
            self.assertIsNone(failure)
            self.assertEqual(len(frames), 3)
        for current in (receipt, recovery):
            self.assertTrue(current["installedClosureVerified"])
            self.assertEqual(current["remainingOwnedProcesses"], 0)
            self.assertEqual(current["cleanupErrors"], [])
            self.assertLessEqual(current["cleanupSeconds"], 10)
        with (root / "F6-precommit-result.json").open("x") as stream:
            json.dump({"status": "PASS", "scope": "F6 precommit cancellation only",
                       "cancelledWorkerCalls": 1, "cancelledCommits": 0,
                       "failureProjectionDigest": digest_file(projection_path),
                       "recoveryWindows": 3, "recoveryFrames": 9,
                       "remaining": ["waiting/running cancellation", "completion race"]},
                      stream, indent=2)

    def test_f6_active_cancel_and_terminal_races_then_fresh_mission_recovers(self):
        root, config, profile, detector = self._network_fixture("F6-runtime")

        def require_clean(receipt, output):
            self.assertTrue(receipt["installedClosureVerified"], str(output))
            self.assertEqual(receipt["remainingOwnedProcesses"], 0, str(output))
            self.assertEqual(receipt["cleanupErrors"], [], str(output))
            self.assertLessEqual(receipt["cleanupSeconds"], 10, str(output))

        def require_cancel_projection(output, receipt):
            projection_path = output / "private/ground-station/presentation/latest.json"
            projection = _load(projection_path, 4 * 1024 * 1024)
            self.assertEqual(projection["state"], "failure")
            self.assertEqual(projection["windowId"], "window-0")
            self.assertEqual(projection["reason"], "collaboration cancelled")
            self.assertEqual(projection["runId"], output.name)
            self.assertEqual(projection["missionId"], receipt["missionId"])
            from Experiments.UAV.run_multicamera_tracking_demo import _person_failure_display_presented
            self.assertTrue(_person_failure_display_presented(
                (output / "ground-station-display.log").read_text(), run_id=output.name,
                window_index=0, projection_digest=digest_file(projection_path), mode="headless"))

        def require_recovery(label):
            code, output = self._installed_case(
                root, config, label, profile, detector, run=True, network_expected=True)
            receipt = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
            self.assertEqual(code, 0)
            self.assertTrue(receipt["terminal"])
            self.assertEqual(receipt["outcome"], "collaboration-result")
            for key, expected in (("committedWindows", 3), ("receivedFrames", 9),
                                  ("displayedFrames", 9)):
                self.assertEqual(receipt["counts"][key], expected, (label, key))
            self.assertEqual(receipt["failedWindows"], 0)
            require_clean(receipt, output)
            return output, receipt

        # Cancel only after the real Compute fetch and UAV3 source hold are both
        # observed. This exercises cancellation while a declared dependency is
        # in flight, before any worker result or transaction can be committed.
        code, active = self._installed_case(
            root, config, "active-fetch-cancel", profile, detector, run=True,
            fault="user-cancel-during-active-reference-fetch", network_expected=True)
        active_compute = (active / "compute.log").read_text()
        active_source = (active / "uav3-camera.log").read_text()
        active_gs = (active / "ground-station.log").read_text()
        active_receipt = _load(active / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertEqual(code, 1)
        self.assertTrue(active_receipt["terminal"])
        self.assertEqual(active_receipt["outcome"], "collaboration-cancelled")
        self.assertEqual(active_receipt["counts"]["committedWindows"], 0)
        self.assertEqual(active_receipt["counts"]["receivedFrames"], 0)
        self.assertEqual(active_receipt["counts"]["displayedFrames"], 0)
        self.assertEqual(active_compute.count(
            "SPEC210_ACTIVE_REFERENCE_FETCH_STARTED camera=UAV3 window=window-0 category=POSE"), 1)
        self.assertEqual(active_source.count(
            "SPEC210_ACTIVE_REFERENCE_SOURCE_HELD camera=UAV3 window=window-0 category=POSE"), 1)
        for marker in (
                "SPEC210_USER_CANCEL_ACK_QUEUED window=window-0 authority_revoked=true",
                "SPEC210_USER_CANCEL_ROLE_CLOSED"):
            self.assertEqual(active_compute.count(marker), 1, marker)
        self.assertEqual(active_gs.count("SPEC210_CANCEL_ACK_CONFIRMED window=0"), 1)
        self.assertEqual(active_gs.count(
            "SPEC191_TRACKING_CANCELLED window=0 accepted=true"), 1)
        require_cancel_projection(active, active_receipt)
        self.assertEqual(list((active / "private/compute/worker/transactions").rglob(
            "manifest.json")), [])
        require_clean(active_receipt, active)
        active_recovery, active_recovery_receipt = require_recovery("active-fetch-recovery")
        self.assertNotEqual(active_receipt["missionId"], active_recovery_receipt["missionId"])
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(active_receipt[key], active_recovery_receipt[key], key)

        worker_observation = {}

        def observe_worker_cancel(output, unit, state):
            gate = output / "private/spec219-person-worker-cancel"
            if not (gate / "cancel-ready").is_file() or worker_observation:
                return
            self.assertEqual(state.get("ActiveState"), "active")
            self.assertEqual(state.get("SubState"), "running")
            active = _load(gate / "detector-inference-active.json", 4096)
            self.assertEqual(active["schema"], "spec219-person-worker-active-v1")
            self.assertEqual(active["runId"], output.name)
            self.assertEqual(active["windowId"], "window-0")
            self.assertEqual(active["sequence"], 1)
            self.assertEqual(active["cameraId"], "UAV1")
            self.assertEqual(active["phase"], "after-first-detector-inference")
            process = _proc_identity(active["pid"])
            self.assertIsNotNone(process, "worker disappeared before cancellation")
            self.assertEqual(process["startTimeTicks"], active["startTimeTicks"])
            self.assertNotIn(process["state"], ("Z", "X", "T"))
            manifest = _load(output / "manifest.json", 4 * 1024 * 1024)
            scripts = [Path(item["path"]) for item in
                       manifest["personTracking"]["frozenFiles"]
                       if Path(item["path"]).name == "tracking_worker.py"]
            self.assertEqual(len(scripts), 1)
            self.assertEqual(process["argv"].count(str(scripts[0])), 1)
            self.assertIn("--person-session", process["argv"])
            for option, expected in (("--run-id", output.name),
                                     ("--mission-id", output.name),
                                     ("--window-id", "window-0"), ("--sequence", "1")):
                index = process["argv"].index(option)
                self.assertEqual(process["argv"][index + 1], expected)
            self.assertEqual(process["cgroups"].get(("0", "")), state.get("ControlGroup"))
            compute = (output / "compute.log").read_text(errors="replace")
            self.assertIn("SPEC210_TRACKING_WORKER_INVOKED window=window-0", compute)
            self.assertNotIn("SPEC219_PERSON_SESSION_DONE jobId=", compute)
            worker_observation.update({
                "pid": active["pid"], "startTimeTicks": active["startTimeTicks"],
                "phase": active["phase"], "state": process["state"],
                "cgroup": process["cgroups"][("0", "")],
            })
            with (root / "worker-active-cancel-observation.json").open("x") as stream:
                json.dump(worker_observation, stream, indent=2)
                stream.write("\n")
            (gate / "release-cancel").touch(exist_ok=False)

        code, worker_cancel = self._installed_case(
            root, config, "worker-active-cancel", profile, detector, run=True,
            fault="user-cancel-during-person-worker", network_expected=True,
            observer=observe_worker_cancel)
        self.assertTrue(worker_observation, "worker inference was not independently observed")
        worker_compute = (worker_cancel / "compute.log").read_text()
        worker_gs = (worker_cancel / "ground-station.log").read_text()
        worker_receipt = _load(worker_cancel / "person-run-receipt.json", 16 * 1024 * 1024)
        worker_runtime = _load(worker_cancel / "person-worker-cancel-runtime.json", 65536)
        self.assertEqual(code, 1)
        self.assertTrue(worker_receipt["terminal"])
        self.assertEqual(worker_receipt["outcome"], "collaboration-cancelled")
        self.assertEqual(worker_receipt["counts"]["committedWindows"], 0)
        self.assertEqual(worker_receipt["counts"]["displayedFrames"], 0)
        self.assertEqual(worker_compute.count(
            "SPEC210_TRACKING_WORKER_INVOKED window=window-0"), 1)
        self.assertNotIn("SPEC219_PERSON_SESSION_DONE jobId=", worker_compute)
        self.assertEqual(worker_compute.count(
            "SPEC210_USER_CANCEL_ACK_QUEUED window=window-0 authority_revoked=true"), 1)
        self.assertEqual(worker_compute.count("SPEC210_USER_CANCEL_ROLE_CLOSED"), 1)
        self.assertEqual(worker_gs.count("SPEC210_CANCEL_ACK_CONFIRMED window=0"), 1)
        self.assertEqual(worker_gs.count(
            "SPEC191_TRACKING_CANCELLED window=0 accepted=true"), 1)
        self.assertTrue((worker_cancel / "private/spec219-person-worker-cancel/cancel-sent").is_file())
        self.assertEqual(worker_runtime["schema"], "spec219-person-worker-cancel-runtime-v1")
        self.assertTrue(worker_runtime["workerExitedBeforeRunnerCleanup"])
        self.assertEqual(worker_runtime["workerPid"], worker_observation["pid"])
        self.assertEqual(worker_runtime["workerStartTimeTicks"],
                         worker_observation["startTimeTicks"])
        self.assertIn(worker_runtime["workerExitState"], ("missing", "Z"))
        self.assertFalse((worker_cancel / "private/compute/worker/tracking-result.json").exists())
        self.assertEqual(list((worker_cancel / "private/compute/worker/transactions").rglob(
            "manifest.json")), [])
        require_cancel_projection(worker_cancel, worker_receipt)
        require_clean(worker_receipt, worker_cancel)
        worker_recovery, worker_recovery_receipt = require_recovery("worker-cancel-recovery")
        self.assertNotEqual(worker_receipt["missionId"], worker_recovery_receipt["missionId"])
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(worker_receipt[key], worker_recovery_receipt[key], key)

        # The runner releases the terminal mutex only after the cancellation
        # request is waiting. The success response must win exactly once and the
        # already committed immutable transaction must remain the one consumed.
        code, response_wins = self._installed_case(
            root, config, "response-wins", profile, detector, run=True,
            fault="user-cancel-terminal-response-race", network_expected=True)
        response_compute = (response_wins / "compute.log").read_text()
        response_gs = (response_wins / "ground-station.log").read_text()
        response_receipt = _load(response_wins / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertEqual(code, 0)
        self.assertTrue(response_receipt["terminal"])
        self.assertEqual(response_receipt["outcome"], "collaboration-result")
        self.assertEqual(response_gs.count("SPEC191_TRACKING_RESULT status=success"), 1)
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(active_receipt[key], response_receipt[key], key)
        response_events = [(i, json.loads(line[len("SPEC219_TRACE "):]))
                           for i, line in enumerate(response_compute.splitlines())
                           if line.startswith("SPEC219_TRACE ")]
        response_commit = next(i for i, event in response_events
                               if event.get("event") == "committed" and
                               event.get("windowId") == "window-0")
        response_lines = response_compute.splitlines()
        response_locked = next(i for i, line in enumerate(response_lines)
                               if "SPEC210_TERMINAL_RESPONSE_LOCKED window=window-0" in line)
        response_published = next(i for i, line in enumerate(response_lines)
                                  if "SPEC210_TERMINAL_RESPONSE_PUBLISHED window=window-0" in line)
        self.assertLess(response_commit, response_locked)
        self.assertLess(response_locked, response_published)
        response_manifests = list((response_wins / "private/compute/worker/transactions")
                                  .rglob("manifest.json"))
        self.assertEqual(len(response_manifests), 3)
        require_clean(response_receipt, response_wins)

        cancel_snapshot = {}

        def snapshot_cancel_wins_before_signal(output, unit, state):
            gate = output / "private/spec210-terminal-response-race"
            if not (gate / "transaction-snapshot-ready").is_file() or cancel_snapshot:
                return
            self.assertEqual(state.get("ActiveState"), "active")
            self.assertFalse((gate / "cancel-sent").exists())
            compute = (output / "compute.log").read_text(errors="replace")
            self.assertIn("SPEC210_TERMINAL_RESPONSE_READY window=window-0", compute)
            self.assertNotIn("SPEC210_USER_CANCEL_ACK_QUEUED", compute)
            manifests = list((output / "private/compute/worker/transactions").rglob(
                "manifest.json"))
            self.assertEqual(len(manifests), 1)
            transaction_dir = manifests[0].parent
            self.assertEqual(transaction_dir.name, "window-0")
            digests = {name: digest_file(transaction_dir / name)
                       for name in ("manifest.json", "result.json", "state.json")}
            self.assertEqual(set(digests), {"manifest.json", "result.json", "state.json"})
            cancel_snapshot.update({"transactionDirectory": str(transaction_dir),
                                   "digests": digests})
            with (root / "cancel-wins-pre-cancel-snapshot.json").open("x") as stream:
                json.dump(cancel_snapshot, stream, indent=2)
                stream.write("\n")
            (gate / "release-cancel").touch(exist_ok=False)

        # In the inverse ordering, window-0 committed before the cancel gate.
        # Cancellation wins terminal publication, but cannot erase or rewrite
        # that immutable result. No later window may commit after cancellation.
        code, cancel_wins = self._installed_case(
            root, config, "cancel-wins", profile, detector, run=True,
            fault="user-cancel-terminal-response-cancel-wins-race", network_expected=True,
            observer=snapshot_cancel_wins_before_signal)
        cancel_compute = (cancel_wins / "compute.log").read_text()
        cancel_gs = (cancel_wins / "ground-station.log").read_text()
        cancel_receipt = _load(cancel_wins / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertEqual(code, 1)
        self.assertTrue(cancel_receipt["terminal"])
        self.assertEqual(cancel_receipt["outcome"], "collaboration-cancelled")
        cancel_events = [(i, json.loads(line[len("SPEC219_TRACE "):]))
                         for i, line in enumerate(cancel_compute.splitlines())
                         if line.startswith("SPEC219_TRACE ")]
        cancel_commit = next(i for i, event in cancel_events
                             if event.get("event") == "committed" and
                             event.get("windowId") == "window-0")
        cancel_lines = cancel_compute.splitlines()
        cancel_ready = next(i for i, line in enumerate(cancel_lines)
                            if "SPEC210_TERMINAL_RESPONSE_READY window=window-0" in line)
        cancel_ack = next(i for i, line in enumerate(cancel_lines)
                          if "SPEC210_USER_CANCEL_ACK_HELD window=window-0" in line)
        self.assertLess(cancel_commit, cancel_ready)
        self.assertLess(cancel_ready, cancel_ack)
        self.assertNotIn("SPEC210_TERMINAL_RESPONSE_PUBLISHED window=window-0", cancel_compute)
        self.assertEqual(cancel_gs.count("SPEC210_CANCEL_ACK_CONFIRMED window=0"), 1)
        self.assertEqual(cancel_gs.count(
            "SPEC191_TRACKING_CANCELLED window=0 accepted=true"), 1)
        require_cancel_projection(cancel_wins, cancel_receipt)
        cancel_manifests = list((cancel_wins / "private/compute/worker/transactions")
                                .rglob("manifest.json"))
        self.assertEqual(len(cancel_manifests), 1)
        self.assertEqual(cancel_manifests[0].parent.name, "window-0")
        self.assertTrue(cancel_snapshot, "pre-cancel transaction snapshot was not captured")
        self.assertTrue((root / "cancel-wins-pre-cancel-snapshot.json").is_file())
        self.assertTrue((cancel_wins / "private/spec210-terminal-response-race/cancel-sent").is_file())
        retained = cancel_snapshot["digests"]
        self.assertEqual(cancel_snapshot["transactionDirectory"], str(cancel_manifests[0].parent))
        self.assertEqual(cancel_receipt["counts"]["committedWindows"], 1)
        require_clean(cancel_receipt, cancel_wins)
        cancel_recovery, cancel_recovery_receipt = require_recovery("cancel-wins-recovery")
        self.assertNotEqual(cancel_receipt["missionId"], cancel_recovery_receipt["missionId"])
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(active_receipt[key], cancel_receipt[key], key)
            self.assertEqual(cancel_receipt[key], cancel_recovery_receipt[key], key)
        self.assertEqual({name: digest_file(cancel_manifests[0].parent / name)
                          for name in retained}, retained)

        with (root / "F6-runtime-result.json").open("x") as stream:
            json.dump({
                "status": "PASS_SCOPED",
                "scope": "F6 active fetch/worker cancellation, terminal lock orderings, and fresh recovery",
                "cases": {
                    "activeFetchCancel": {"outcome": active_receipt["outcome"],
                                           "missionId": active_receipt["missionId"],
                                           "recoveryMissionId": active_recovery_receipt["missionId"]},
                    "workerActiveCancel": {
                        "outcome": worker_receipt["outcome"],
                        "missionId": worker_receipt["missionId"],
                        "phase": worker_observation["phase"],
                        "workerPid": worker_runtime["workerPid"],
                        "workerExitedBeforeRunnerCleanup":
                            worker_runtime["workerExitedBeforeRunnerCleanup"],
                        "recoveryMissionId": worker_recovery_receipt["missionId"]},
                    "responseWins": {"outcome": response_receipt["outcome"],
                                     "missionId": response_receipt["missionId"],
                                     "committedWindows": response_receipt["counts"]["committedWindows"]},
                    "cancelWins": {"outcome": cancel_receipt["outcome"],
                                   "missionId": cancel_receipt["missionId"],
                                   "retainedWindowZero": retained,
                                   "preCancelSnapshotPath":
                                       str(root / "cancel-wins-pre-cancel-snapshot.json"),
                                   "recoveryMissionId": cancel_recovery_receipt["missionId"]}},
                "cleanup": {label: _load(path / "person-cleanup.json", 65536)["cleanupErrors"]
                            for label, path in (("active", active),
                                                ("activeRecovery", active_recovery),
                                                ("workerActive", worker_cancel),
                                                ("workerRecovery", worker_recovery),
                                                ("responseWins", response_wins),
                                                ("cancelWins", cancel_wins),
                                                ("cancelRecovery", cancel_recovery))},
            }, stream, indent=2)

    def test_f7_source_epoch_restart_rejected_then_fresh_mission_resets(self):
        root, config, profile, detector = self._network_fixture("F7")
        code, restarted = self._installed_case(
            root, config, "source-epoch-restart", profile, detector, run=True,
            fault="person-source-epoch-restart-after-window-0", network_expected=True)
        self.assertEqual(code, 1, "require native stale-state rejection, not startup failure")
        rejected = _load(restarted / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(rejected["terminal"])
        self.assertEqual(rejected["outcome"], "collaboration-failed")
        self.assertEqual(rejected["counts"]["committedWindows"], 1)

        plan = _load(restarted / "manifest.json", 4 * 1024 * 1024)
        generations = plan["observation"]["sourceGenerations"]["generations"]
        self.assertEqual([item["firstWindowIndex"] for item in generations], [0, 1])
        self.assertEqual(generations[1]["sourceEpoch"], generations[0]["sourceEpoch"] + 1)
        for camera in ("UAV1", "UAV2", "UAV3"):
            self.assertNotEqual(generations[0]["sourceIds"][camera],
                                generations[1]["sourceIds"][camera])

        compute = (restarted / "compute.log").read_text()
        invocations = [line for line in compute.splitlines()
                       if line.startswith("SPEC210_TRACKING_WORKER_INVOKED ")]
        self.assertEqual(invocations, ["SPEC210_TRACKING_WORKER_INVOKED window=window-0"])
        transactions = restarted / "private/compute/worker/transactions"
        manifests = list(transactions.rglob("manifest.json"))
        self.assertEqual(len(manifests), 1)
        transaction = manifests[0].parent
        self.assertEqual(manifests[0].parent.name, "window-0")
        committed = _load(manifests[0], 65536)
        self.assertEqual(committed["windowId"], "window-0")
        for filename, digest_key in (("result.json", "resultDigest"),
                                     ("state.json", "stateDigest")):
            self.assertEqual(list(transactions.rglob(filename)), [transaction / filename])
            self.assertEqual(digest_file(transaction / filename), committed[digest_key])

        prior_state = _load(transaction / "state.json", 16 * 1024 * 1024)
        binding = prior_state["binding"]
        self.assertEqual(binding["runId"], restarted.name)
        self.assertEqual(binding["missionId"], rejected["missionId"])
        for index, camera in enumerate(("UAV1", "UAV2", "UAV3")):
            self.assertEqual(binding["frames"][index]["sourceId"],
                             generations[0]["sourceIds"][camera])
            self.assertEqual(binding["frames"][index]["sourceEpoch"],
                             generations[0]["sourceEpoch"])
        projection_path = restarted / "private/ground-station/presentation/latest.json"
        projection = _load(projection_path, 4 * 1024 * 1024)
        self.assertEqual(projection["state"], "failure")
        self.assertEqual(projection["windowId"], "window-1")
        self.assertIn("SOURCE_RESTART_REQUIRES_NEW_MISSION", projection["reason"])
        from Experiments.UAV.run_multicamera_tracking_demo import _person_failure_display_presented
        self.assertTrue(_person_failure_display_presented(
            (restarted / "ground-station-display.log").read_text(), run_id=restarted.name,
            window_index=1, projection_digest=digest_file(projection_path), mode="headless"))

        sys.path.insert(0, str(Path(shutil.which("UavTrackingNode")).parent.parent /
                               "libexec/ndnsf-uav/tracking"))
        from tracking_display import PersonPresentationReader
        reader = PersonPresentationReader(
            restarted / "private/ground-station/presentation/context.json")
        prior_frames, prior_failure = reader.load(
            reader.root / "window-0/presentation.json")
        self.assertIsNone(prior_failure)
        self.assertEqual(len(prior_frames), 3)
        preserved_frames, failure = reader.load(projection_path)
        self.assertEqual(preserved_frames, prior_frames)
        self.assertEqual(failure["window"], 1)
        self.assertEqual(failure["projectionDigest"], digest_file(projection_path))

        code, recovery_output = self._installed_case(
            root, config, "fresh-mission", profile, detector, run=True, network_expected=True)
        self.assertEqual(code, 0)
        recovery = _load(recovery_output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(recovery["terminal"])
        self.assertEqual(recovery["outcome"], "collaboration-result")
        for key, expected in (("committedWindows", 3), ("receivedFrames", 9),
                              ("displayedFrames", 9)):
            self.assertEqual(recovery["counts"][key], expected)
        self.assertEqual(recovery["failedWindows"], 0)
        self.assertNotEqual(rejected["missionId"], recovery["missionId"])
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(rejected[key], recovery[key], key)
        fault_plan = _load(root / "source-epoch-restart-plan.json", 65536)
        recovery_plan = _load(root / "fresh-mission-plan.json", 65536)
        for key in ("sourceDigest", "profileDigest", "detectorDigest", "reidDigest",
                    "runnerSourceDigest", "testSourceDigest", "installedNodeDigest"):
            self.assertEqual(fault_plan[key], recovery_plan[key], key)
        recovery_manifest = _load(recovery_output / "manifest.json", 4 * 1024 * 1024)
        self.assertEqual([item["firstWindowIndex"] for item in
                          recovery_manifest["observation"]["sourceGenerations"]["generations"]], [0])
        recovered_sources = _load(recovery_output / "prepared-person-sources.json", 65536)
        self.assertFalse(recovered_sources["sourceRestartAfterWindowZero"])
        self.assertNotEqual(recovered_sources["sourceEpoch"], generations[0]["sourceEpoch"])

        recovery_transactions = recovery_output / "private/compute/worker/transactions"
        recovery_manifests = list(recovery_transactions.rglob("manifest.json"))
        self.assertEqual(len(recovery_manifests), 3)
        first_manifest_path = next(path for path in recovery_manifests
                                   if path.parent.name == "window-0")
        first_directory = first_manifest_path.parent
        first_result = _load(first_directory / "result.json", 16 * 1024 * 1024)
        first_state = _load(first_directory / "state.json", 16 * 1024 * 1024)
        self.assertEqual(first_result["missionId"], recovery["missionId"])
        self.assertEqual(first_state["binding"]["missionId"], recovery["missionId"])
        global_ids = [track["globalId"] for frame in first_result["frames"]
                      for track in frame["tracks"] if type(track.get("globalId")) is int]
        gallery_ids = [item["globalId"] for item in first_state["gallery"]]
        self.assertTrue(global_ids, "fresh first window must expose an assigned identity")
        self.assertTrue(gallery_ids, "fresh first window must initialize the global-ID gallery")
        self.assertEqual(min(global_ids), 1)
        self.assertEqual(min(gallery_ids), 1)
        self.assertLessEqual(set(global_ids), set(gallery_ids))
        self.assertEqual(first_state["nextGlobalId"], max(gallery_ids) + 1)

        recovered_reader = PersonPresentationReader(
            recovery_output / "private/ground-station/presentation/context.json")
        for window in range(3):
            frames, failure = recovered_reader.load(
                recovered_reader.root / f"window-{window}/presentation.json")
            self.assertIsNone(failure)
            self.assertEqual(len(frames), 3)
        for receipt, output in ((rejected, restarted), (recovery, recovery_output)):
            self.assertTrue(receipt["installedClosureVerified"], str(output))
            self.assertEqual(receipt["remainingOwnedProcesses"], 0, str(output))
            self.assertEqual(receipt["cleanupErrors"], [], str(output))
            self.assertLessEqual(receipt["cleanupSeconds"], 10, str(output))
        with (root / "F7-source-epoch-result.json").open("x") as stream:
            json.dump({"status": "PASS_SCOPED", "fault": "source epoch restart after window-0",
                       "rejectedWindow": "window-1", "preservedWindow": "window-0",
                       "sourceEpochs": [generations[0]["sourceEpoch"],
                                        generations[1]["sourceEpoch"]],
                       "preservedTransactionManifestDigest": digest_file(manifests[0]),
                       "failureProjectionDigest": digest_file(projection_path),
                       "recoveryMissionId": recovery["missionId"],
                       "freshWindowZeroGlobalIds": global_ids,
                       "freshNextGlobalId": first_state["nextGlobalId"],
                       "recoveryWindows": 3, "recoveryFrames": 9,
                       "scope": "EX-5 F7 source-epoch branch only; no accuracy or GUI qualification"},
                      stream, indent=2)

    def test_f4_committed_window_replay(self):
        root, config, profile, detector = self._network_fixture("F4")
        code, output = self._installed_case(
            root, config, "replay", profile, detector, run=True,
            fault="replay-committed-window", network_expected=True)
        gs = (output / "ground-station.log").read_text()
        compute = (output / "compute.log").read_text()
        requests = [line for line in gs.splitlines()
                    if line.startswith("SPEC191_TRACKING_GS_REQUEST ") and
                    " window=window-0" in line]
        successes = [line for line in gs.splitlines()
                     if line.startswith("SPEC191_TRACKING_WINDOW_RESULT window=0 ") and
                     "status=success" in line and "result_verified=true" in line]
        cached = [json.loads(line[len("SPEC219_TRACE "):])
                  for line in compute.splitlines() if line.startswith("SPEC219_TRACE ")]
        cached = [entry for entry in cached
                  if entry.get("event") == "cached" and entry.get("windowId") == "window-0"]
        observation = {"status": "OBSERVED_NOT_QUALIFIED", "exitCode": code,
                       "windowZeroRequests": requests, "windowZeroSuccesses": successes,
                       "cachedEvents": cached,
                       "workerCalls": compute.count("SPEC210_TRACKING_WORKER_INVOKED"),
                       "sourceDigest": digest_file(root / "source-manifest.json"),
                       "gsLogDigest": digest_file(output / "ground-station.log"),
                       "computeLogDigest": digest_file(output / "compute.log")}
        (root / "F4-replay-observation.json").write_text(json.dumps(observation, indent=2))
        # An infrastructure failure cannot stand in for a native replay defect.
        self.assertEqual(len(requests), 2)
        self.assertNotEqual(requests[0].split()[1], requests[1].split()[1])
        self.assertGreaterEqual(len(successes), 1)
        self.assertEqual(gs.count("SPEC219_COMMITTED_WINDOW_REPLAY_REQUESTED "), 1)
        self.assertEqual(len(cached), 1)
        self.assertEqual(code, 0, "native committed-window replay must succeed")
        self.assertEqual(len(successes), 2)
        self.assertEqual(gs.count("SPEC219_COMMITTED_WINDOW_REPLAY_VERIFIED "), 1)
        self.assertEqual(observation["workerCalls"], 3)

    def test_f4_replay_preserves_state_rejects_alteration_then_fresh_mission_recovers(self):
        self.assertTrue(hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"))
        root, config, profile, detector = self._network_fixture("F4-state")
        observation = {"checkpoints": []}
        installed_gs = Path(shutil.which("UavTrackingGroundStation")).resolve()

        def bounded_text(path, limit=1024 * 1024):
            with path.open("rb") as stream:
                payload = stream.read(limit + 1)
            self.assertLessEqual(len(payload), limit, str(path))
            return payload.decode("utf-8", errors="strict")

        def snapshot(output):
            transactions = output / "private/compute/worker/transactions"
            manifests = list(transactions.rglob("manifest.json"))
            self.assertEqual(len(manifests), 1)
            self.assertEqual(manifests[0].parent.name, "window-0")
            presentation = output / "private/ground-station/presentation/window-0"
            paths = [manifests[0].parent / name for name in
                     ("manifest.json", "result.json", "state.json")]
            paths += [presentation / name for name in
                      ("presentation.json", "UAV1.jpg", "UAV2.jpg", "UAV3.jpg")]
            state = _load(manifests[0].parent / "state.json", 16 * 1024 * 1024)
            self.assertEqual(set(state["backendState"]["cameras"]), {"UAV1", "UAV2", "UAV3"})
            counters = {"nextGlobalId": state["nextGlobalId"], "watermarkUs": state["watermarkUs"],
                        "cameraCounters": {camera: {key: value["tracker"][key]
                                                   for key in ("frameId", "idHighWatermark")}
                                           for camera, value in state["backendState"]["cameras"].items()}}
            return {"digests": {str(path.relative_to(output)): digest_file(path) for path in paths},
                    "counters": counters}

        def observe(output, unit, service_state):
            count = len(observation["checkpoints"])
            if count == 2 or not (output / "ground-station.log").exists():
                return
            checkpoint = ("original", "cached")[count]
            marker = "SPEC219_COMMITTED_WINDOW_REPLAY_READY checkpoint=" + checkpoint
            if marker not in bounded_text(output / "ground-station.log", 16 * 1024 * 1024):
                return
            current = snapshot(output)
            if count:
                self.assertEqual(current, observation["checkpoints"][0]["snapshot"])
            expected_group = "/system.slice/" + unit + ".service"
            self.assertEqual(service_state.get("ControlGroup"), expected_group)
            group = CGROUP_ROOT / expected_group.lstrip("/")
            pids = bounded_text(group / "cgroup.procs", 16384).splitlines()
            self.assertLessEqual(len(pids), 256)
            candidates = []
            for pid_text in pids:
                self.assertTrue(pid_text.isdecimal())
                proc = Path("/proc") / pid_text
                try:
                    argv = bounded_text(proc / "cmdline").rstrip("\0").split("\0")
                except FileNotFoundError:
                    continue
                expected = {"--run-id": output.name, "--fault": "alter-committed-window",
                            "--person-presentation-dir": str(output / "private/ground-station/presentation")}
                if (argv and Path(argv[0]).resolve() == installed_gs and
                        all(argv.count(key) == 1 and argv.index(key) + 1 < len(argv) and
                            argv[argv.index(key) + 1] == value for key, value in expected.items())):
                    candidates.append((int(pid_text), argv))
            self.assertEqual(len(candidates), 1)
            pid, argv = candidates[0]
            fd = os.pidfd_open(pid, 0)
            try:
                proc = Path("/proc") / str(pid)
                self.assertEqual(bounded_text(proc / "cmdline").rstrip("\0").split("\0"), argv)
                self.assertEqual([line for line in bounded_text(proc / "cgroup").splitlines()
                                  if line.startswith("0::")], ["0::" + expected_group])
                signal.pidfd_send_signal(fd, signal.SIGUSR2)
            finally:
                os.close(fd)
            observation["checkpoints"].append({"checkpoint": checkpoint, "snapshot": current,
                                                "pid": pid, "argv": argv,
                                                "signalledMonotonicNs": time.monotonic_ns()})
            (root / "replay-state-observation.json").write_text(json.dumps(observation, indent=2))

        code, output = self._installed_case(
            root, config, "altered", profile, detector, run=True,
            fault="alter-committed-window", network_expected=True, observer=observe)
        self.assertEqual(code, 1)
        self.assertEqual(len(observation["checkpoints"]), 2)
        self.assertEqual(snapshot(output), observation["checkpoints"][0]["snapshot"])
        gs = (output / "ground-station.log").read_text()
        compute = (output / "compute.log").read_text()
        source = (output / "uav3-camera.log").read_text()
        requests = [line.split()[1] for line in gs.splitlines()
                    if line.startswith("SPEC191_TRACKING_GS_REQUEST ") and " window=window-0" in line]
        self.assertEqual(len(requests), 3)
        self.assertEqual(len(set(requests)), 3)
        self.assertEqual(sum(line.startswith("SPEC191_TRACKING_WINDOW_RESULT window=0 ") and
                             "status=success" in line and "result_verified=true" in line
                             for line in gs.splitlines()), 2)
        self.assertEqual(gs.count("SPEC219_COMMITTED_WINDOW_REPLAY_VERIFIED "), 1)
        self.assertNotIn("altered committed window unexpectedly succeeded", gs)
        self.assertEqual(source.count("SPEC219_COMMITTED_WINDOW_ALTERED_FRAME "), 1)
        self.assertIn("immutable named Data publication conflict", source)
        traces = [json.loads(line[len("SPEC219_TRACE "):]) for line in compute.splitlines()
                  if line.startswith("SPEC219_TRACE ")]
        self.assertEqual(sum(item.get("event") == "cached" for item in traces), 1)
        self.assertEqual(compute.count("SPEC210_TRACKING_WORKER_INVOKED"), 1)
        rejected = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertEqual(rejected["outcome"], "collaboration-failed")
        self.assertEqual(rejected["counts"]["committedWindows"], 1)
        projection_path = output / "private/ground-station/presentation/latest.json"
        projection = _load(projection_path, 4 * 1024 * 1024)
        self.assertEqual(projection["state"], "failure")
        self.assertEqual(projection["windowId"], "window-0")
        self.assertEqual(projection["lastGoodPresentation"], "window-0/presentation.json")
        from Experiments.UAV.run_multicamera_tracking_demo import _person_failure_display_presented
        self.assertTrue(_person_failure_display_presented(
            (output / "ground-station-display.log").read_text(), run_id=output.name,
            window_index=0, projection_digest=digest_file(projection_path), mode="headless"))
        sys.path.insert(0, str(installed_gs.parent.parent / "libexec/ndnsf-uav/tracking"))
        from tracking_display import PersonPresentationReader
        reader = PersonPresentationReader(output / "private/ground-station/presentation/context.json")
        frames, failure = reader.load(projection_path)
        self.assertEqual(len(frames), 3)
        self.assertIsNotNone(failure)
        code, recovered_output = self._installed_case(
            root, config, "recovery", profile, detector, run=True, network_expected=True)
        self.assertEqual(code, 0)
        recovery = _load(recovered_output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertEqual(recovery["outcome"], "collaboration-result")
        for key, expected in (("committedWindows", 3), ("receivedFrames", 9), ("displayedFrames", 9)):
            self.assertEqual(recovery["counts"][key], expected)
        self.assertNotEqual(rejected["missionId"], recovery["missionId"])
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(rejected[key], recovery[key])
        recovered_reader = PersonPresentationReader(
            recovered_output / "private/ground-station/presentation/context.json")
        for window in range(3):
            frames, failure = recovered_reader.load(
                recovered_reader.root / f"window-{window}/presentation.json")
            self.assertIsNone(failure)
            self.assertEqual(len(frames), 3)
        for receipt in (rejected, recovery):
            self.assertTrue(receipt["terminal"])
            self.assertTrue(receipt["installedClosureVerified"])
            self.assertEqual(receipt["remainingOwnedProcesses"], 0)
            self.assertEqual(receipt["cleanupErrors"], [])
            self.assertLessEqual(receipt["cleanupSeconds"], 10)
        with (root / "F4-state-result.json").open("x") as stream:
            json.dump({"status": "PASS", "scope": "F4 positive/cache/state/alteration/recovery",
                       "requestIds": requests, "workerCalls": 1, "cachedEvents": 1,
                       "retained": observation["checkpoints"][0]["snapshot"],
                       "observationDigest": digest_file(root / "replay-state-observation.json"),
                       "failureProjectionDigest": digest_file(projection_path),
                       "recoveryWindows": 3, "recoveryFrames": 9}, stream, indent=2)

    def test_f2_unauthorized_source_from_cache_then_fresh_mission_recovers(self):
        import re
        from Experiments.UAV.run_multicamera_tracking_demo import (
            _person_failure_display_presented, _uav_socket_root)
        self.assertTrue(hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"))
        for key in ("ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS"):
            self.assertNotIn(key, os.environ, "do not suppress dynamic diagnostics")
        root, config, profile, detector = self._network_fixture("F2-cache")
        installed_node = Path(shutil.which("UavTrackingNode")).resolve()
        helper = installed_node.parent.parent / "libexec/ndnsf-tests/spec219-cached-signer-fixture"
        self.assertTrue(helper.is_file())
        dependencies = subprocess.run(["readelf", "-d", str(helper)], capture_output=True,
                                      text=True, timeout=5, check=True).stdout
        self.assertIn("libasan.so", dependencies)
        self.assertIn("libubsan.so", dependencies)
        observation = {}

        def bounded_text(path, limit=1024 * 1024):
            with path.open("rb") as stream:
                payload = stream.read(limit + 1)
            self.assertLessEqual(len(payload), limit)
            return payload.decode("utf-8", errors="strict")

        def prior_digests(output):
            manifests = list((output / "private/compute/worker/transactions").rglob("manifest.json"))
            self.assertEqual(len(manifests), 1)
            self.assertEqual(manifests[0].parent.name, "window-0")
            paths = [manifests[0].parent / name for name in
                     ("manifest.json", "result.json", "state.json")]
            presentation = output / "private/ground-station/presentation/window-0"
            paths += [presentation / name for name in
                      ("presentation.json", "UAV1.jpg", "UAV2.jpg", "UAV3.jpg")]
            return {str(path.relative_to(output)): digest_file(path) for path in paths}

        def observe(output, unit, service_state):
            if observation or not (output / "compute.log").exists():
                return
            compute = bounded_text(output / "compute.log", 16 * 1024 * 1024)
            marker = "SPEC219_UNAUTHORIZED_CACHE_FETCH_READY name="
            names = [line[len(marker):] for line in compute.splitlines() if line.startswith(marker)]
            if not names:
                return
            self.assertEqual(len(names), 1)
            frame_name = names[0]
            source_path = output / "uav3-camera.log"
            if (not source_path.exists() or
                    "window=window-1 name=" + frame_name not in bounded_text(source_path, 16 * 1024 * 1024)):
                return
            expected_group = "/system.slice/" + unit + ".service"
            self.assertEqual(service_state.get("ControlGroup"), expected_group)
            sockets = _uav_socket_root(output)
            self.assertTrue((sockets / "compute.sock").is_socket())
            self.assertTrue((sockets / "uav3.sock").is_socket())
            observation.update({"unit": unit, "frameName": frame_name,
                                "priorDigests": prior_digests(output),
                                "fixtureDigest": digest_file(helper), "commands": []})
            observation_path = root / "cache-observation.json"
            observation_path.write_text(json.dumps(observation, indent=2))
            fixture_output = root / "packet-fixture"
            keychain = output / "private/keychain"
            environment = dict(os.environ,
                               NDN_CLIENT_TRANSPORT="unix://" + str(sockets / "compute.sock"),
                               NDN_CLIENT_PIB="pib-sqlite3:" + str(keychain / "pib"),
                               NDN_CLIENT_TPM="tpm-file:" + str(keychain / "tpm"))
            command = [str(helper), "--origin-socket", str(sockets / "uav3.sock"),
                       "--cache-socket", str(sockets / "compute.sock"),
                       "--keychain", str(keychain), "--object", frame_name,
                       "--producer", "/example/uav/drone/UAV3",
                       "--signer", "/example/uav/drone/UAV1", "--output", str(fixture_output)]
            fixture_log_path = root / "packet-fixture.log"
            try:
                with fixture_log_path.open("xb") as stream:
                    result = subprocess.run(command, env=environment, stdout=stream,
                                            stderr=subprocess.STDOUT, timeout=25)
            except subprocess.TimeoutExpired as error:
                observation.update({"fixtureCommand": command, "fixtureTimedOut": True,
                                    "fixtureTimeoutSeconds": error.timeout,
                                    "fixtureLogDigest": digest_file(fixture_log_path)})
                observation_path.write_text(json.dumps(observation, indent=2))
                raise
            observation.update({"fixtureCommand": command, "fixtureExitCode": result.returncode,
                                "fixtureLogDigest": digest_file(fixture_log_path)})
            observation_path.write_text(json.dumps(observation, indent=2))
            self.assertEqual(result.returncode, 0)
            fixture_log = bounded_text(root / "packet-fixture.log")
            self.assertIn("SPEC219_UNAUTHORIZED_CACHE_SEEDED ", fixture_log)
            self.assertNotRegex(fixture_log, r"AddressSanitizer|LeakSanitizer|runtime error:")
            manifest = _load(fixture_output / "manifest.json", 1024 * 1024)
            self.assertEqual(manifest["allAssertionsPassed"], "true")
            self.assertEqual(manifest["prefixUnregistered"], "true")
            count = int(manifest["segmentsCount"])
            self.assertGreater(count, 1, "exercise a real segmented encrypted frame")
            self.assertEqual(len(manifest["segments"]), count)
            self.assertEqual(int(manifest["solicitedPublications"]), count)

            def nfdc(*arguments, timeout=5):
                command = ["nfdc", *arguments]
                result = subprocess.run(command, env=environment, capture_output=True,
                                        text=True, timeout=timeout)
                observation["commands"].append({"command": command, "exitCode": result.returncode,
                                                "stdout": result.stdout, "stderr": result.stderr})
                observation_path.write_text(json.dumps(observation, indent=2))
                self.assertEqual(result.returncode, 0)
                return result.stdout

            fib = nfdc("fib", "list")
            self.assertFalse(any(line.strip().startswith(frame_name + " ") for line in fib.splitlines()),
                             "fixture prefix must be removed before cache-only proof")
            created = nfdc("face", "create", "udp4://127.0.0.1:9", "permanent")
            self.assertIn("face-created", created)
            match = re.search(r"\bid=(\d+)\b", created)
            self.assertIsNotNone(match)
            face_id = match.group(1)
            accepted = nfdc("route", "add", frame_name, "nexthop", face_id, "capture")
            self.assertIn("capture", accepted)
            expected_route = frame_name + " nexthops={faceid=" + face_id + " (cost=0)}"
            route_deadline = time.monotonic() + 3
            while True:
                remaining = route_deadline - time.monotonic()
                self.assertGreater(remaining, 0,
                                   "accepted capture route did not reach FIB within 3s")
                fib = nfdc("fib", "list", timeout=remaining)
                routes = [line.strip() for line in fib.splitlines()
                          if line.strip().startswith(frame_name + " ")]
                if routes == [expected_route]:
                    break
                self.assertFalse(routes, "capture route reached FIB with an unexpected nexthop")
                self.assertLess(time.monotonic(), route_deadline,
                                "accepted capture route did not reach FIB within 3s")
                time.sleep(.05)
            probes = []
            for segment in manifest["segments"]:
                self.assertEqual(digest_file(fixture_output / segment["originalFile"]),
                                 segment["originalDigest"])
                expected = fixture_output / segment["forgedFile"]
                self.assertEqual(digest_file(expected), segment["forgedDigest"])
                command = ["ndnpeek", "-f", "-w", "1500", "-l", "1000", segment["name"]]
                result = subprocess.run(command, env=environment, capture_output=True, timeout=2)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, expected.read_bytes(), "require cached original wire")
                probes.append({"name": segment["name"], "wireDigest": segment["forgedDigest"]})
            observation.update({"cachedSegments": probes, "cacheOnlyRouteFaceId": int(face_id),
                                "fixtureManifestDigest": digest_file(fixture_output / "manifest.json")})
            pids = bounded_text(CGROUP_ROOT / expected_group.lstrip("/") / "cgroup.procs", 16384).splitlines()
            self.assertLessEqual(len(pids), 256)
            candidates = []
            for pid_text in pids:
                self.assertTrue(pid_text.isdecimal())
                try:
                    argv = bounded_text(Path("/proc") / pid_text / "cmdline").rstrip("\0").split("\0")
                except FileNotFoundError:
                    continue
                expected = {"--role": "compute", "--run-id": output.name,
                            "--fault": "unauthorized-frame-cache"}
                if (argv and Path(argv[0]).resolve() == installed_node and
                        all(argv.count(key) == 1 and argv.index(key) + 1 < len(argv) and
                            argv[argv.index(key) + 1] == value for key, value in expected.items())):
                    candidates.append((int(pid_text), argv))
            self.assertEqual(len(candidates), 1)
            pid, argv = candidates[0]
            fd = os.pidfd_open(pid, 0)
            try:
                proc = Path("/proc") / str(pid)
                self.assertEqual(bounded_text(proc / "cmdline").rstrip("\0").split("\0"), argv)
                self.assertEqual([line for line in bounded_text(proc / "cgroup").splitlines()
                                  if line.startswith("0::")], ["0::" + expected_group])
                signal.pidfd_send_signal(fd, signal.SIGUSR2)
            finally:
                os.close(fd)
            observation.update({"computePid": pid, "computeArgv": argv,
                                "releasedMonotonicNs": time.monotonic_ns()})
            observation_path.write_text(json.dumps(observation, indent=2))

        code, output = self._installed_case(root, config, "cached", profile, detector, run=True,
                                             fault="unauthorized-frame-cache", network_expected=True,
                                             observer=observe)
        self.assertEqual(code, 1)
        self.assertIn("releasedMonotonicNs", observation)
        self.assertEqual(prior_digests(output), observation["priorDigests"])
        compute = (output / "compute.log").read_text()
        prefix = "Failed fetching collaboration large Data " + observation["frameName"] + ": "
        errors = [line.split(prefix, 1)[1] for line in compute.splitlines() if prefix in line]
        self.assertEqual(len(errors), 1)
        self.assertTrue(errors[0].startswith("SegmentFetcher error 3:") or
                        errors[0] == "Data signer is not authorized for expected producer /example/uav/drone/UAV3",
                        "require native signature/Producer rejection, not a timeout: " + errors[0])
        self.assertEqual(compute.count("SPEC210_TRACKING_WORKER_INVOKED"), 1)
        self.assertNotIn("SPEC210_TRACKING_WORKER_INVOKED window=window-1", compute)
        traces = [json.loads(line[len("SPEC219_TRACE "):]) for line in compute.splitlines()
                  if line.startswith("SPEC219_TRACE ")]
        self.assertFalse(any(item.get("event") == "admitted" and
                             item.get("windowId") == "window-1" and
                             item.get("cameraId") == "UAV3" for item in traces))
        rejected = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertEqual(rejected["outcome"], "collaboration-failed")
        self.assertEqual(rejected["counts"]["committedWindows"], 1)
        projection = output / "private/ground-station/presentation/latest.json"
        value = _load(projection, 4 * 1024 * 1024)
        self.assertEqual(value["state"], "failure")
        self.assertEqual(value["windowId"], "window-1")
        self.assertEqual(value["lastGoodPresentation"], "window-0/presentation.json")
        self.assertTrue(_person_failure_display_presented(
            (output / "ground-station-display.log").read_text(), run_id=output.name,
            window_index=1, projection_digest=digest_file(projection), mode="headless"))
        code, recovered_output = self._installed_case(root, config, "recovery", profile, detector,
                                                       run=True, network_expected=True)
        self.assertEqual(code, 0)
        recovery = _load(recovered_output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertEqual(recovery["outcome"], "collaboration-result")
        for key, count in (("committedWindows", 3), ("receivedFrames", 9), ("displayedFrames", 9)):
            self.assertEqual(recovery["counts"][key], count)
        self.assertNotEqual(rejected["missionId"], recovery["missionId"])
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(rejected[key], recovery[key])
        sys.path.insert(0, str(installed_node.parent.parent / "libexec/ndnsf-uav/tracking"))
        from tracking_display import PersonPresentationReader
        for current_output, windows in ((output, 1), (recovered_output, 3)):
            reader = PersonPresentationReader(current_output / "private/ground-station/presentation/context.json")
            for window in range(windows):
                frames, failure = reader.load(reader.root / f"window-{window}/presentation.json")
                self.assertIsNone(failure)
                self.assertEqual(len(frames), 3)
        for receipt in (rejected, recovery):
            self.assertTrue(receipt["terminal"])
            self.assertTrue(receipt["installedClosureVerified"])
            self.assertEqual(receipt["remainingOwnedProcesses"], 0)
            self.assertEqual(receipt["cleanupErrors"], [])
            self.assertLessEqual(receipt["cleanupSeconds"], 10)
        with (root / "F2-cache-result.json").open("x") as stream:
            json.dump({"status": "PASS", "nativeRejection": errors[0],
                       "cachedSegments": len(observation["cachedSegments"]),
                       "priorDigests": observation["priorDigests"],
                       "observationDigest": digest_file(root / "cache-observation.json"),
                       "recoveryWindows": 3, "recoveryFrames": 9,
                       "scope": "F2 unauthorized source through actual cache; binding subcases B132"},
                      stream, indent=2)

    def test_f2_request_bindings_rejected_then_fresh_mission_recovers(self):
        root, config, profile, detector = self._network_fixture("F2")
        receipts = []
        for label, fault, key in (
                ("profile", "wrong-person-profile-binding", "tracking_profile_digest"),
                ("computation", "wrong-person-computation-binding", "tracking_computation_digest")):
            code, output = self._installed_case(root, config, label, profile, detector,
                                                 run=True, fault=fault, network_expected=True)
            self.assertEqual(code, 1, "must fail through native collaboration, not setup")
            receipt = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
            receipts.append(receipt)
            self.assertTrue(receipt["terminal"])
            self.assertEqual(receipt["outcome"], "collaboration-failed")
            self.assertEqual(receipt["counts"]["committedWindows"], 0)
            self.assertEqual(receipt["failedWindows"], 3)
            self.assertTrue(receipt["installedClosureVerified"])
            self.assertEqual(receipt["remainingOwnedProcesses"], 0)
            self.assertEqual(receipt["cleanupErrors"], [])
            self.assertLessEqual(receipt["cleanupSeconds"], 10)
            compute = (output / "compute.log").read_text()
            self.assertIn("SPEC191_TRACKING_NODE_READY", compute)
            self.assertNotIn("SPEC210_TRACKING_WORKER_INVOKED", compute)
            transactions = output / "private/compute/worker/transactions"
            for filename in ("manifest.json", "result.json", "state.json"):
                self.assertEqual(list(transactions.rglob(filename)), [], filename)
            gs = (output / "ground-station.log").read_text()
            self.assertIn("SPEC191_TRACKING_PLAN_COMMITTED", gs)
            self.assertIn("SPEC191_TRACKING_RESULT status=failure", gs)
            projection_path = output / "private/ground-station/presentation/latest.json"
            projection = _load(projection_path, 4 * 1024 * 1024)
            self.assertEqual(projection["state"], "failure")
            self.assertEqual(projection["windowId"], "window-0")
            self.assertEqual(projection["runId"], output.name)
            self.assertIn("person request binding mismatch: " + key, projection["reason"])
            from Experiments.UAV.run_multicamera_tracking_demo import _person_failure_display_presented
            self.assertTrue(_person_failure_display_presented(
                (output / "ground-station-display.log").read_text(), run_id=output.name,
                window_index=0, projection_digest=digest_file(projection_path), mode="headless"))
        code, output = self._installed_case(root, config, "recovery", profile, detector,
                                             run=True, network_expected=True)
        self.assertEqual(code, 0)
        recovery = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(recovery["terminal"])
        self.assertEqual(recovery["outcome"], "collaboration-result")
        self.assertEqual(recovery["counts"]["committedWindows"], 3)
        self.assertEqual(recovery["counts"]["receivedFrames"], 9)
        self.assertEqual(recovery["counts"]["displayedFrames"], 9)
        self.assertEqual(recovery["failedWindows"], 0)
        self.assertTrue(recovery["installedClosureVerified"])
        self.assertEqual(recovery["remainingOwnedProcesses"], 0)
        self.assertEqual(recovery["cleanupErrors"], [])
        self.assertLessEqual(recovery["cleanupSeconds"], 10)
        for rejected in receipts:
            self.assertNotEqual(rejected["missionId"], recovery["missionId"])
            for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
                self.assertEqual(rejected[key], recovery[key], key)
        self.assertEqual((output / "compute.log").read_text().count("SPEC210_TRACKING_WORKER_INVOKED"), 3)
        sys.path.insert(0, str(Path(shutil.which("UavTrackingNode")).parent.parent /
                               "libexec/ndnsf-uav/tracking"))
        from tracking_display import PersonPresentationReader
        reader = PersonPresentationReader(output / "private/ground-station/presentation/context.json")
        for window in range(3):
            frames, failure = reader.load(reader.root / f"window-{window}/presentation.json")
            self.assertIsNone(failure)
            self.assertEqual(len(frames), 3)
        with (root / "F2-binding-result.json").open("x") as stream:
            json.dump({"status": "PASS", "negativeCases": ["profile", "computation"],
                       "recoveryWindows": 3, "recoveryFrames": 9,
                       "scope": "F2 signed request-binding variants only; no accuracy claim",
                       "remaining": ["unauthorized signer and cache-return variants"]}, stream, indent=2)

    def test_f1_frame_sidecar_mismatch_then_fresh_mission_recovers(self):
        root, config, profile, detector = self._network_fixture("F1")
        code, output = self._installed_case(root, config, "corrupt", profile, detector,
                                             run=True, fault="bad-content", network_expected=True)
        self.assertEqual(code, 1, "require native collaboration failure, not startup failure")
        rejected = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(rejected["terminal"])
        self.assertEqual(rejected["outcome"], "collaboration-failed")
        self.assertEqual(rejected["counts"]["committedWindows"], 1)
        self.assertEqual(rejected["failedWindows"], 2)
        compute = (output / "compute.log").read_text()
        self.assertEqual(compute.count("SPEC210_TRACKING_WORKER_INVOKED"), 1)
        self.assertIn("SPEC210_TRACKING_WORKER_INVOKED window=window-0", compute)
        self.assertNotIn("SPEC210_TRACKING_WORKER_INVOKED window=window-1", compute)
        transactions = output / "private/compute/worker/transactions"
        manifests = list(transactions.rglob("manifest.json"))
        self.assertEqual(len(manifests), 1)
        manifest = _load(manifests[0], 65536)
        self.assertEqual(manifest["windowId"], "window-0")
        self.assertEqual(manifests[0].parent.name, "window-0")
        for filename, key in (("result.json", "resultDigest"), ("state.json", "stateDigest")):
            self.assertEqual(list(transactions.rglob(filename)), [manifests[0].parent / filename])
            self.assertEqual(digest_file(manifests[0].parent / filename), manifest[key])
        projection_path = output / "private/ground-station/presentation/latest.json"
        projection = _load(projection_path, 4 * 1024 * 1024)
        self.assertEqual(projection["state"], "failure")
        self.assertEqual(projection["windowId"], "window-1")
        self.assertEqual(projection["runId"], output.name)
        self.assertIn("observation FRAME DataRef does not match its exact object", projection["reason"])
        from Experiments.UAV.run_multicamera_tracking_demo import _person_failure_display_presented
        self.assertTrue(_person_failure_display_presented(
            (output / "ground-station-display.log").read_text(), run_id=output.name,
            window_index=1, projection_digest=digest_file(projection_path), mode="headless"))
        sys.path.insert(0, str(Path(shutil.which("UavTrackingNode")).parent.parent /
                               "libexec/ndnsf-uav/tracking"))
        from tracking_display import PersonPresentationReader
        reader = PersonPresentationReader(output / "private/ground-station/presentation/context.json")
        frames, failure = reader.load(reader.root / "window-0/presentation.json")
        self.assertIsNone(failure)
        self.assertEqual(len(frames), 3)
        self.assertEqual(projection["lastGoodPresentation"], "window-0/presentation.json")
        retained_frames, retained_failure = reader.load(projection_path)
        self.assertEqual(len(retained_frames), 3)
        self.assertEqual(retained_failure["window"], 1)
        self.assertEqual(retained_failure["projectionDigest"], digest_file(projection_path))
        self.assertEqual(retained_frames, frames)
        code, recovered_output = self._installed_case(root, config, "recovery", profile, detector,
                                                       run=True, network_expected=True)
        self.assertEqual(code, 0)
        recovery = _load(recovered_output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(recovery["terminal"])
        self.assertEqual(recovery["outcome"], "collaboration-result")
        self.assertEqual(recovery["counts"]["committedWindows"], 3)
        self.assertEqual(recovery["counts"]["receivedFrames"], 9)
        self.assertEqual(recovery["counts"]["displayedFrames"], 9)
        self.assertEqual(recovery["failedWindows"], 0)
        self.assertNotEqual(rejected["missionId"], recovery["missionId"])
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(rejected[key], recovery[key], key)
        self.assertEqual((recovered_output / "compute.log").read_text().count(
            "SPEC210_TRACKING_WORKER_INVOKED"), 3)
        recovered_reader = PersonPresentationReader(
            recovered_output / "private/ground-station/presentation/context.json")
        for window in range(3):
            frames, failure = recovered_reader.load(
                recovered_reader.root / f"window-{window}/presentation.json")
            self.assertIsNone(failure)
            self.assertEqual(len(frames), 3)
        for receipt in (rejected, recovery):
            self.assertTrue(receipt["installedClosureVerified"])
            self.assertEqual(receipt["remainingOwnedProcesses"], 0)
            self.assertEqual(receipt["cleanupErrors"], [])
            self.assertLessEqual(receipt["cleanupSeconds"], 10)
        with (root / "F1-content-result.json").open("x") as stream:
            json.dump({"status": "PASS", "negativeCase": "bad-content",
                       "rejectedWindow": "window-1", "preservedWindow": "window-0",
                       "preservedTransactionManifestDigest": digest_file(manifests[0]),
                       "failureProjectionDigest": digest_file(projection_path),
                       "recoveryWindows": 3, "recoveryFrames": 9,
                       "scope": "F1 FRAME/OBSERVATION content-binding variant only",
                       "remaining": ["signature/cache variants", "formal GUI fault capture"]},
                      stream, indent=2)

    def test_f5_actual_worker_crash_preserves_prior_transaction_then_recovers(self):
        self.assertTrue(hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"),
                        "pidfd identity is required; do not fall back to numeric kill")
        root, config, profile, detector = self._network_fixture("F5")
        injection = {}
        installed_node = Path(shutil.which("UavTrackingNode")).resolve()
        installed_worker = installed_node.parent.parent / "libexec/ndnsf-uav/tracking/tracking_worker.py"
        self.assertTrue(installed_worker.is_file())

        def bounded_text(path, limit=65536):
            with path.open("rb") as stream:
                payload = stream.read(limit + 1)
            self.assertLessEqual(len(payload), limit, str(path))
            return payload.decode("utf-8", errors="replace")

        def crash_owned_worker(output, unit, service_state):
            if injection:
                return
            compute_path = output / "compute.log"
            if not compute_path.exists():
                return
            triggers = []
            for line in bounded_text(compute_path, 16 * 1024 * 1024).splitlines():
                if line.startswith("SPEC219_TRACE ") and '"worker-start"' in line:
                    event = json.loads(line[len("SPEC219_TRACE "):])
                    if (event.get("event") == "worker-start" and
                            event.get("windowId") == "window-1" and
                            event.get("runId") == output.name and
                            event.get("missionId") == output.name):
                        triggers.append(event)
            if not triggers:
                return
            self.assertEqual(len(triggers), 1, "one actual affected worker exchange required")
            expected_group = "/system.slice/" + unit + ".service"
            self.assertEqual(service_state.get("ControlGroup"), expected_group)
            group = CGROUP_ROOT / expected_group.lstrip("/")
            pids = bounded_text(group / "cgroup.procs", 16384).splitlines()
            self.assertLessEqual(len(pids), 256)
            candidates = []
            expected_arguments = {"--run-id": output.name, "--mission-id": output.name,
                                  "--person-window": str(output / "private/compute/worker/person-backend-input.json")}
            for pid_text in pids:
                self.assertTrue(pid_text.isdecimal())
                proc = Path("/proc") / pid_text
                try:
                    argv = bounded_text(proc / "cmdline").rstrip("\0").split("\0")
                except FileNotFoundError:
                    continue
                if (len(argv) >= 3 and argv[:2] == [config["workerPython"], str(installed_worker)] and
                        "--person-session" in argv and
                        all(argv.count(key) == 1 and argv.index(key) + 1 < len(argv) and
                            argv[argv.index(key) + 1] == value for key, value in expected_arguments.items())):
                    candidates.append((int(pid_text), argv))
            self.assertEqual(len(candidates), 1, "signal only the single verified owned person worker")
            pid, argv = candidates[0]
            fd = os.pidfd_open(pid, 0)
            try:
                proc = Path("/proc") / str(pid)
                self.assertEqual(bounded_text(proc / "cmdline").rstrip("\0").split("\0"), argv)
                worker_membership = bounded_text(proc / "cgroup")
                self.assertEqual([line for line in worker_membership.splitlines() if line.startswith("0::")],
                                 ["0::" + expected_group])
                status = dict(line.split(":", 1) for line in bounded_text(proc / "status").splitlines())
                parent_pid = int(status["PPid"].strip())
                parent = Path("/proc") / str(parent_pid)
                parent_argv = bounded_text(parent / "cmdline").rstrip("\0").split("\0")
                self.assertEqual(Path(parent_argv[0]).resolve(), installed_node)
                self.assertIn("--role", parent_argv)
                self.assertEqual(parent_argv[parent_argv.index("--role") + 1], "compute")
                parent_membership = bounded_text(parent / "cgroup")
                self.assertEqual([line for line in parent_membership.splitlines() if line.startswith("0::")],
                                 ["0::" + expected_group])
                transactions = output / "private/compute/worker/transactions"
                manifests = list(transactions.rglob("manifest.json"))
                self.assertEqual(len(manifests), 1, "kill before affected transaction marker")
                self.assertEqual(manifests[0].parent.name, "window-0")
                original = {str(path.relative_to(output)): digest_file(path)
                            for path in (manifests[0], manifests[0].parent / "result.json",
                                         manifests[0].parent / "state.json")}
                plan = {"pid": pid, "parentPid": parent_pid, "argv": argv,
                        "unit": unit, "controlGroup": expected_group, "trigger": triggers[0],
                        "workerCgroupMembership": worker_membership,
                        "parentCgroupMembership": parent_membership,
                        "signal": "SIGKILL", "preSignalMonotonicNs": time.monotonic_ns(),
                        "priorTransactionDigests": original}
                with (root / "worker-kill-plan.json").open("x") as stream:
                    json.dump(plan, stream, indent=2)
                signal.pidfd_send_signal(fd, signal.SIGKILL)
                injection.update(plan, signalSent=True, signalSentMonotonicNs=time.monotonic_ns())
                with (root / "worker-kill-result.json").open("x") as stream:
                    json.dump(injection, stream, indent=2)
            finally:
                os.close(fd)

        code, output = self._installed_case(root, config, "crash", profile, detector,
                                             run=True, network_expected=True, observer=crash_owned_worker)
        self.assertTrue(injection.get("signalSent"), "a pre-worker failRole is not a worker crash")
        self.assertEqual(code, 1)
        rejected = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(rejected["terminal"])
        self.assertEqual(rejected["outcome"], "collaboration-failed")
        self.assertEqual(rejected["counts"]["committedWindows"], 1)
        self.assertEqual(rejected["failedWindows"], 2)
        compute = (output / "compute.log").read_text()
        self.assertEqual(compute.count("SPEC210_TRACKING_WORKER_INVOKED"), 2)
        self.assertIn("TRACKING_WORKER_FAILED outcome=2 exit=137 system_error=0", compute)
        transactions = output / "private/compute/worker/transactions"
        for name in ("manifest.json", "result.json", "state.json"):
            matches = list(transactions.rglob(name))
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0].parent.name, "window-0")
        for relative_path, before_digest in injection["priorTransactionDigests"].items():
            self.assertEqual(digest_file(output / relative_path), before_digest)
        projection_path = output / "private/ground-station/presentation/latest.json"
        projection = _load(projection_path, 4 * 1024 * 1024)
        self.assertEqual(projection["state"], "failure")
        self.assertEqual(projection["windowId"], "window-1")
        self.assertEqual(projection["lastGoodPresentation"], "window-0/presentation.json")
        self.assertIn("CPU tracking worker failed to produce a candidate result/state pair", projection["reason"])
        from Experiments.UAV.run_multicamera_tracking_demo import _person_failure_display_presented
        self.assertTrue(_person_failure_display_presented(
            (output / "ground-station-display.log").read_text(), run_id=output.name,
            window_index=1, projection_digest=digest_file(projection_path), mode="headless"))
        sys.path.insert(0, str(installed_worker.parent))
        from tracking_display import PersonPresentationReader
        reader = PersonPresentationReader(output / "private/ground-station/presentation/context.json")
        prior_frames, failure = reader.load(reader.root / "window-0/presentation.json")
        self.assertIsNone(failure)
        self.assertEqual(len(prior_frames), 3)
        retained_frames, failure = reader.load(projection_path)
        self.assertEqual(retained_frames, prior_frames)
        self.assertEqual(failure["window"], 1)
        self.assertEqual(failure["projectionDigest"], digest_file(projection_path))
        code, recovered_output = self._installed_case(root, config, "recovery", profile, detector,
                                                       run=True, network_expected=True)
        self.assertEqual(code, 0)
        recovery = _load(recovered_output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(recovery["terminal"])
        self.assertEqual(recovery["outcome"], "collaboration-result")
        self.assertEqual(recovery["counts"]["committedWindows"], 3)
        self.assertEqual(recovery["counts"]["receivedFrames"], 9)
        self.assertEqual(recovery["counts"]["displayedFrames"], 9)
        self.assertEqual(recovery["failedWindows"], 0)
        self.assertNotEqual(rejected["missionId"], recovery["missionId"])
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(rejected[key], recovery[key], key)
        self.assertEqual((recovered_output / "compute.log").read_text().count(
            "SPEC210_TRACKING_WORKER_INVOKED"), 3)
        recovered_reader = PersonPresentationReader(
            recovered_output / "private/ground-station/presentation/context.json")
        for window in range(3):
            frames, failure = recovered_reader.load(
                recovered_reader.root / f"window-{window}/presentation.json")
            self.assertIsNone(failure)
            self.assertEqual(len(frames), 3)
        for receipt in (rejected, recovery):
            self.assertTrue(receipt["installedClosureVerified"])
            self.assertEqual(receipt["remainingOwnedProcesses"], 0)
            self.assertEqual(receipt["cleanupErrors"], [])
            self.assertLessEqual(receipt["cleanupSeconds"], 10)
        with (root / "F5-crash-result.json").open("x") as stream:
            json.dump({"status": "PASS", "signalSent": True, "childExitCode": 137,
                       "rejectedWindow": "window-1", "preservedWindow": "window-0",
                       "priorTransactionDigests": injection["priorTransactionDigests"],
                       "recoveryWindows": 3, "recoveryFrames": 9,
                       "scope": "F5 actual worker crash only; F5 full row remains PARTIAL",
                       "remaining": ["oversized output", "bounded inference resource failure"]},
                      stream, indent=2)

    def test_f3_exact_frame_route_block_then_fresh_mission_recovers(self):
        import re
        import shlex
        from Experiments.UAV.run_multicamera_tracking_demo import (
            _person_failure_display_presented, _uav_socket_root)

        root, config, profile, detector = self._network_fixture("F3")
        observation = {"commands": [], "prior": {}}

        def traces(output, filename):
            path = output / filename
            if not path.exists():
                return []
            # A live writer may not have finished its final trace line yet.
            lines = path.read_text().splitlines(keepends=True)
            return [json.loads(line[len("SPEC219_TRACE "):]) for line in lines
                    if line.startswith("SPEC219_TRACE ") and line.endswith("\n")]

        def observe(output, unit, state):
            routes_path = output / "routes.json"
            if not routes_path.exists():
                return
            if "blockedMonotonicNs" not in observation:
                manifest = _load(output / "manifest.json", 4 * 1024 * 1024)
                epoch = manifest["observation"]["sourceEpoch"]
                self.assertIsInstance(epoch, int)
                self.assertGreater(epoch, 0)
                frame_name = f"/example/uav/drone/UAV3/UAV/OBS/UAV3/{epoch}/2/FRAME/v=1"
                transport = "unix://" + str(_uav_socket_root(output) / "compute.sock")
                routes = _load(routes_path, 65536)["routes"]
                compute_routes = [entry for entry in routes if entry["node"] == "compute"]
                self.assertTrue(compute_routes)
                for entry in compute_routes:
                    self.assertEqual(shlex.split(entry["command"])[0],
                                     "NDN_CLIENT_TRANSPORT=" + transport)
                self.assertTrue((_uav_socket_root(output) / "compute.sock").is_socket())
                self.assertFalse(any(t["windowId"] == "window-1" for t in
                                     traces(output, "uav3-camera.log")),
                                 "install isolation before this source frame exists")
                private = output / "private/f3-network-observer"
                private.mkdir(mode=0o700)
                environment = dict(os.environ, HOME=str(private),
                                   NDN_CLIENT_TRANSPORT=transport,
                                   NDN_CLIENT_PIB="pib-sqlite3:" + str(private / "pib"),
                                   NDN_CLIENT_TPM="tpm-file:" + str(private / "tpm"))
                observation.update({"unit": unit, "transport": transport,
                                    "frameName": frame_name})

                def nfdc(*arguments):
                    command = ["nfdc", *arguments]
                    result = subprocess.run(command, env=environment, text=True,
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                            timeout=5)
                    observation["commands"].append({"command": command,
                                                    "exitCode": result.returncode,
                                                    "stdout": result.stdout,
                                                    "stderr": result.stderr})
                    (root / "route-observation.json").write_text(json.dumps(observation, indent=2))
                    self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                    return result.stdout

                created = nfdc("face", "create", "udp4://127.0.0.1:9", "permanent")
                self.assertIn("face-created", created, "require a new run-owned discard face")
                self.assertIn("remote=udp4://127.0.0.1:9", created)
                face = re.search(r"\bid=(\d+)\b", created)
                self.assertIsNotNone(face)
                face_id = face.group(1)
                accepted = nfdc("route", "add", frame_name, "nexthop", face_id, "capture")
                self.assertIn("route-add-accepted", accepted)
                self.assertIn("capture", accepted)
                fib = nfdc("fib", "list")
                entries = [line.strip() for line in fib.splitlines()
                           if line.strip().startswith(frame_name + " ")]
                self.assertEqual(entries, [frame_name + " nexthops={faceid=" + face_id +
                                           " (cost=0)}"], "no parent-route bypass")
                observation.update({"faceId": int(face_id),
                                    "blockedMonotonicNs": time.monotonic_ns()})
                (root / "route-observation.json").write_text(json.dumps(observation, indent=2))
            if not observation["prior"]:
                transactions = output / "private/compute/worker/transactions"
                manifests = list(transactions.glob("*/window-0/manifest.json"))
                if not manifests:
                    return
                self.assertEqual(len(manifests), 1)
                paths = [manifests[0].parent / name for name in
                         ("manifest.json", "result.json", "state.json")]
                if not all(path.is_file() for path in paths):
                    return
                digests = {str(path.relative_to(output)): digest_file(path) for path in paths}
                observation["prior"] = {"digests": digests,
                                        "capturedMonotonicNs": time.monotonic_ns()}
                (root / "route-observation.json").write_text(json.dumps(observation, indent=2))

        code, output = self._installed_case(root, config, "blocked", profile, detector,
                                             run=True, network_expected=True, observer=observe)
        self.assertEqual(code, 1, "require native collaboration failure, not startup failure")
        self.assertIn("blockedMonotonicNs", observation)
        self.assertTrue(observation["prior"])
        rejected = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(rejected["terminal"])
        self.assertEqual(rejected["outcome"], "collaboration-failed")
        self.assertEqual(rejected["counts"]["committedWindows"], 1)
        self.assertEqual(rejected["failedWindows"], 2)
        fetches = [t for t in traces(output, "compute.log") if t["windowId"] == "window-1"
                   and t.get("cameraId") == "UAV3" and t["event"] == "fetch-start"]
        publications = [t for t in traces(output, "uav3-camera.log")
                        if t["windowId"] == "window-1" and t["event"] == "publication-start"]
        self.assertEqual(len(fetches), 1)
        self.assertEqual(len(publications), 1)
        self.assertLess(observation["blockedMonotonicNs"], publications[0]["monotonicNs"])
        self.assertLess(observation["blockedMonotonicNs"], fetches[0]["monotonicNs"])
        self.assertLess(observation["prior"]["capturedMonotonicNs"], fetches[0]["monotonicNs"])
        self.assertTrue(any(t["windowId"] == "window-1" and t["event"] == "published"
                            for t in traces(output, "uav3-camera.log")))
        self.assertFalse(any(t["windowId"] == "window-1" and t.get("cameraId") == "UAV3"
                             and t["event"] == "admitted" for t in traces(output, "compute.log")))
        reason = "tracking compute could not fetch " + observation["frameName"]
        compute = (output / "compute.log").read_text()
        self.assertIn(reason, compute)
        self.assertEqual(compute.count("SPEC210_TRACKING_WORKER_INVOKED"), 1)
        self.assertIn("SPEC210_TRACKING_WORKER_INVOKED window=window-0", compute)
        transactions = output / "private/compute/worker/transactions"
        manifests = list(transactions.rglob("manifest.json"))
        self.assertEqual(len(manifests), 1)
        self.assertEqual(manifests[0].parent.name, "window-0")
        for relative, expected in observation["prior"]["digests"].items():
            self.assertEqual(digest_file(output / relative), expected)
        for filename in ("result.json", "state.json"):
            self.assertEqual(list(transactions.rglob(filename)), [manifests[0].parent / filename])
        projection_path = output / "private/ground-station/presentation/latest.json"
        projection = _load(projection_path, 4 * 1024 * 1024)
        self.assertEqual(projection["state"], "failure")
        self.assertEqual(projection["windowId"], "window-1")
        self.assertEqual(projection["runId"], output.name)
        self.assertIn(reason, projection["reason"])
        self.assertTrue(_person_failure_display_presented(
            (output / "ground-station-display.log").read_text(), run_id=output.name,
            window_index=1, projection_digest=digest_file(projection_path), mode="headless"))
        sys.path.insert(0, str(Path(shutil.which("UavTrackingNode")).parent.parent /
                               "libexec/ndnsf-uav/tracking"))
        from tracking_display import PersonPresentationReader
        reader = PersonPresentationReader(output / "private/ground-station/presentation/context.json")
        frames, failure = reader.load(reader.root / "window-0/presentation.json")
        self.assertIsNone(failure)
        self.assertEqual(len(frames), 3)
        retained, failure = reader.load(projection_path)
        self.assertEqual(retained, frames)
        self.assertEqual(failure["window"], 1)
        self.assertEqual(failure["projectionDigest"], digest_file(projection_path))
        code, recovered_output = self._installed_case(root, config, "recovery", profile, detector,
                                                       run=True, network_expected=True)
        self.assertEqual(code, 0)
        recovery = _load(recovered_output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(recovery["terminal"])
        self.assertEqual(recovery["outcome"], "collaboration-result")
        self.assertEqual(recovery["counts"]["committedWindows"], 3)
        self.assertEqual(recovery["counts"]["receivedFrames"], 9)
        self.assertEqual(recovery["counts"]["displayedFrames"], 9)
        self.assertEqual(recovery["failedWindows"], 0)
        self.assertNotEqual(rejected["missionId"], recovery["missionId"])
        for key in ("profileDigest", "computationDigest", "sourceManifestDigest"):
            self.assertEqual(rejected[key], recovery[key], key)
        self.assertEqual((recovered_output / "compute.log").read_text().count(
            "SPEC210_TRACKING_WORKER_INVOKED"), 3)
        reader = PersonPresentationReader(
            recovered_output / "private/ground-station/presentation/context.json")
        for window in range(3):
            frames, failure = reader.load(reader.root / f"window-{window}/presentation.json")
            self.assertIsNone(failure)
            self.assertEqual(len(frames), 3)
        for receipt in (rejected, recovery):
            self.assertTrue(receipt["installedClosureVerified"])
            self.assertEqual(receipt["remainingOwnedProcesses"], 0)
            self.assertEqual(receipt["cleanupErrors"], [])
            self.assertLessEqual(receipt["cleanupSeconds"], 10)
        with (root / "F3-retrieval-result.json").open("x") as stream:
            json.dump({"status": "PASS", "blockedFrameName": observation["frameName"],
                       "routeObservationDigest": digest_file(root / "route-observation.json"),
                       "preservedTransactionDigests": observation["prior"]["digests"],
                       "failureProjectionDigest": digest_file(projection_path),
                       "recoveryWindows": 3, "recoveryFrames": 9,
                       "scope": "actual exact FRAME network isolation, no model parameter changes"},
                      stream, indent=2)

    def test_f5_bounded_resource_and_outputs_then_fresh_mission_recovers(self):
        root, config, profile, detector = self._network_fixture("F5L")
        source_path = root / "source-manifest.json"
        source = _load(source_path, 65536)
        for camera in source["cameras"]:
            self.assertEqual(camera["videoStartUs"], 77500000,
                             "this bounded fixture requires the selected CHIRLA clip")
            camera["videoStartUs"] += 500000
        source_path.write_text(json.dumps(source, sort_keys=True))
        original = _load(profile, 16384)
        original_digest = digest_file(profile)
        expected_first_images = [
            "sha256:92eda784c335f7992bb1253941dc15baca6e934e5a2e19765c2b18b3c0e80a1c",
            "sha256:c258b1f7b636455ff07fe0e4465af10ab2d208d5cda1711d6ba2a7bdd1043b38",
            "sha256:c7288f5d2a18934a4a817c3b2566564d8dfc0a214bc5030ce452e949337261eb"]
        sys.path.insert(0, str(Path(shutil.which("UavTrackingNode")).parent.parent /
                               "libexec/ndnsf-uav/tracking"))
        from person_profile import load_person_profile
        from tracking_display import PersonPresentationReader
        from Experiments.UAV.run_multicamera_tracking_demo import _person_failure_display_presented
        rejected_receipts, cases = [], []
        for label, field, bound, committed, outcome, backend_error in (
                ("persons", "maxPersonsPerFrame", 4, 1, 2, "PERSON_LIMIT"),
                ("stdout", "maxChildLogBytes", 1024, 1, 5, None),
                ("json", "maxResultBytes", 1024, 0, 2, "BACKEND_RESULT_LIMIT")):
            bounded_profile = {**original, "limits": {**original["limits"], field: bound}}
            self.assertNotEqual(original["limits"][field], bound)
            self.assertEqual({key for key in original["limits"]
                              if bounded_profile["limits"][key] != original["limits"][key]}, {field})
            variant = root / (label + "-profile.json")
            with variant.open("x") as stream:
                json.dump(bounded_profile, stream, sort_keys=True)
            parsed_profile = load_person_profile(variant)
            prior = {}

            def snapshot_prior(output, unit, service_state):
                if prior:
                    return
                transactions = output / "private/compute/worker/transactions"
                manifests = list(transactions.rglob("manifest.json"))
                if not manifests:
                    return
                self.assertEqual(len(manifests), 1, "capture before the affected next commit")
                self.assertEqual(manifests[0].parent.name, "window-0")
                digests = {str(path.relative_to(output)): digest_file(path)
                           for path in (manifests[0], manifests[0].parent / "result.json",
                                        manifests[0].parent / "state.json")}
                prior.update(capturedMonotonicNs=time.monotonic_ns(), unit=unit, digests=digests)
                with (root / (label + "-prior.json")).open("x") as stream:
                    json.dump(prior, stream, indent=2)

            code, output = self._installed_case(root, config, label, variant, detector,
                                                 run=True, network_expected=True,
                                                 observer=snapshot_prior if committed else None)
            self.assertEqual(code, 1, "require production collaboration failure, not preflight")
            receipt = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
            rejected_receipts.append(receipt)
            self.assertTrue(receipt["terminal"])
            self.assertEqual(receipt["outcome"], "collaboration-failed")
            self.assertEqual(receipt["counts"]["committedWindows"], committed)
            self.assertEqual(receipt["failedWindows"], 3 - committed)
            self.assertEqual(receipt["profileDigest"], parsed_profile.digest)
            compute = (output / "compute.log").read_text()
            self.assertEqual(compute.count("SPEC210_TRACKING_WORKER_INVOKED"), committed + 1)
            self.assertIn("TRACKING_WORKER_FAILED outcome=" + str(outcome) + " exit=", compute)
            if backend_error:
                self.assertIn("TRACKING_WORKER_FAILED outcome=2 exit=2 system_error=0", compute)
                errors = [json.loads(line) for line in compute.splitlines()
                          if line.startswith('{"event": "TRACKING_FAILED",')]
                self.assertEqual([error["error"] for error in errors], [backend_error])
            transactions = output / "private/compute/worker/transactions"
            for filename in ("manifest.json", "result.json", "state.json"):
                matches = list(transactions.rglob(filename))
                self.assertEqual(len(matches), committed)
                if matches:
                    self.assertEqual(matches[0].parent.name, "window-0")
            self.assertFalse((output / f"private/compute/worker/candidate-state/window-{committed}.json").exists())
            projection_path = output / "private/ground-station/presentation/latest.json"
            projection = _load(projection_path, 4 * 1024 * 1024)
            self.assertEqual(projection["state"], "failure")
            self.assertEqual(projection["windowId"], f"window-{committed}")
            self.assertIn("CPU tracking worker failed to produce a candidate result/state pair", projection["reason"])
            self.assertTrue(_person_failure_display_presented(
                (output / "ground-station-display.log").read_text(), run_id=output.name,
                window_index=committed, projection_digest=digest_file(projection_path), mode="headless"))
            reader = PersonPresentationReader(output / "private/ground-station/presentation/context.json")
            retained, failure = reader.load(projection_path)
            self.assertEqual(failure["window"], committed)
            self.assertEqual(failure["projectionDigest"], digest_file(projection_path))
            self.assertEqual(len(retained), 3 if committed else 0)
            if committed:
                self.assertTrue(prior, "must observe the real prior transaction before failure")
                starts = [json.loads(line[len("SPEC219_TRACE "):]) for line in compute.splitlines()
                          if line.startswith("SPEC219_TRACE ")]
                affected = [event for event in starts if event.get("event") == "worker-start" and
                            event.get("windowId") == "window-1"]
                self.assertEqual(len(affected), 1)
                self.assertLess(prior["capturedMonotonicNs"], affected[0]["monotonicNs"])
                for relative_path, before_digest in prior["digests"].items():
                    self.assertEqual(digest_file(output / relative_path), before_digest)
                self.assertEqual(projection["lastGoodPresentation"], "window-0/presentation.json")
                prior_path = reader.root / "window-0/presentation.json"
                frames, no_failure = reader.load(prior_path)
                self.assertIsNone(no_failure)
                self.assertEqual(frames, retained)
                first = _load(prior_path, 4 * 1024 * 1024)
                self.assertEqual([item["inputDigest"] for item in first["images"]], expected_first_images)
            cases.append({"case": label, "profileDigest": parsed_profile.digest,
                          "changedLimit": field, "bound": bound, "nativeOutcome": outcome,
                          "priorCommittedWindows": committed,
                          "failureProjectionDigest": digest_file(projection_path), "prior": prior})
        code, output = self._installed_case(root, config, "recovery", profile, detector,
                                             run=True, network_expected=True)
        self.assertEqual(code, 0)
        recovery = _load(output / "person-run-receipt.json", 16 * 1024 * 1024)
        self.assertTrue(recovery["terminal"])
        self.assertEqual(recovery["outcome"], "collaboration-result")
        self.assertEqual(recovery["counts"]["committedWindows"], 3)
        self.assertEqual(recovery["counts"]["receivedFrames"], 9)
        self.assertEqual(recovery["counts"]["displayedFrames"], 9)
        self.assertEqual(recovery["failedWindows"], 0)
        self.assertEqual(recovery["profileDigest"], load_person_profile(profile).digest)
        self.assertEqual((output / "compute.log").read_text().count("SPEC210_TRACKING_WORKER_INVOKED"), 3)
        reader = PersonPresentationReader(output / "private/ground-station/presentation/context.json")
        for window in range(3):
            frames, failure = reader.load(reader.root / f"window-{window}/presentation.json")
            self.assertIsNone(failure)
            self.assertEqual(len(frames), 3)
        for rejected in rejected_receipts:
            self.assertNotEqual(rejected["missionId"], recovery["missionId"])
            self.assertNotEqual(rejected["profileDigest"], recovery["profileDigest"])
            self.assertNotEqual(rejected["computationDigest"], recovery["computationDigest"])
            self.assertEqual(rejected["sourceManifestDigest"], recovery["sourceManifestDigest"])
        for receipt in rejected_receipts + [recovery]:
            self.assertTrue(receipt["installedClosureVerified"])
            self.assertEqual(receipt["remainingOwnedProcesses"], 0)
            self.assertEqual(receipt["cleanupErrors"], [])
            self.assertLessEqual(receipt["cleanupSeconds"], 10)
        self.assertEqual(digest_file(profile), original_digest)
        with (root / "F5-limits-result.json").open("x") as stream:
            json.dump({"status": "PASS", "cases": cases, "recoveryWindows": 3,
                       "recoveryFrames": 9, "originalProfileDigest": original_digest,
                       "scope": "F5 bounded actual resource/output guards; combine B134 crash evidence",
                       "remaining": ["other fault rows", "original accuracy", "formal GUI"]},
                      stream, indent=2)

    def test_f8_preflight_fail_closed_then_corrected(self):
        self.assertEqual(os.geteuid(), 0, "explicit installed fault validation requires root")
        self.assertIsNotNone(CGROUP_ROOT)
        self.assertTrue(os.environ.get("PYTHONPATH"), "explicit MiniNDN import environment required")
        config_path = Path(os.environ["NDNSF_PERSON_FAULT_CONFIG"])
        self.assertTrue(config_path.is_absolute())
        config = _load(config_path, 65536)
        self.assertEqual(set(config), {"sourceManifest", "trackingProfile", "detectorModel",
                                      "reidModel", "workerPython", "outputRoot"})
        for key, value in config.items():
            self.assertIsInstance(value, str)
            self.assertTrue(Path(value).is_absolute(), key)
            if key != "outputRoot":
                self.assertTrue(Path(value).is_file(), key)
        self.assertIsNotNone(shutil.which("UavTrackingNode"))
        self.assertIsNotNone(shutil.which("systemd-run"))
        probe = subprocess.run([config["workerPython"], "-c",
                                "import json,torch; print(json.dumps(torch.cuda.is_available()))"],
                               capture_output=True, text=True, timeout=15,
                               env=dict(os.environ, PYTHONNOUSERSITE="1", PYTHONPATH=""))
        self.assertEqual(probe.returncode, 0, probe.stderr[:1024])
        if json.loads(probe.stdout) is not False:
            self.skipTest("requires an actually unavailable CUDA device; never launch a negative on GPU")
        root = Path(config["outputRoot"]) / "F8"
        _check_output_socket_path(root / "cpu-recovery")
        self.assertFalse(root.exists(), "use a new fault-validation purpose root")
        root.mkdir(parents=True)
        source = _load(Path(config["sourceManifest"]), 65536)
        self.assertGreaterEqual(source["durationUs"], 1500000)
        source["durationUs"] = 1500000
        for camera in source["cameras"]:
            video = Path(camera["videoPath"])
            if not video.is_absolute():
                camera["videoPath"] = str((Path(config["sourceManifest"]).parent / video).resolve())
        # Freeze a bounded source interval for preflight checks, not an accuracy run.
        (root / "source-manifest.json").write_text(json.dumps(source, sort_keys=True))
        profile_path = Path(config["trackingProfile"])
        profile = _load(profile_path, 16384)
        self.assertEqual(profile["requestedDevice"], "cpu")
        bad_weights = root / "wrong-checkpoint.pt"
        bad_weights.write_bytes(b"deliberately wrong nonempty checkpoint; must never be loaded")
        code, output = self._installed_case(root, config, "wrong-weights", profile_path,
                                           bad_weights, run=True)
        self.assertEqual(code, 2)
        diagnostic = (output / "person-computation-inspection.stderr").read_text()
        self.assertIn("person model files do not match the tracking profile", diagnostic)
        self.assertFalse((output / "person-computation-inspection.stdout").read_bytes())
        cuda_profile = root / "cuda-profile.json"
        profile["requestedDevice"] = "cuda:0"
        cuda_profile.write_text(json.dumps(profile, sort_keys=True))
        code, output = self._installed_case(root, config, "cuda-unavailable", cuda_profile,
                                           Path(config["detectorModel"]), run=True)
        self.assertEqual(code, 2)
        failed = _load(output / "preflight.json", 65536)
        self.assertFalse(failed["ok"])
        self.assertNotEqual(failed["personDeviceProbe"]["returncode"], 0)
        self.assertIn("cuda", failed["personDeviceProbe"]["stderr"].lower())
        self.assertNotIn('"device": "cpu"', failed["personDeviceProbe"]["stdout"])
        code, output = self._installed_case(root, config, "cpu-recovery", profile_path,
                                           Path(config["detectorModel"]), run=False)
        self.assertEqual(code, 0)
        corrected = _load(output / "preflight.json", 65536)
        self.assertTrue(corrected["ok"], corrected["errors"])
        self.assertEqual(corrected["personDeviceProbe"]["returncode"], 0)
        self.assertIn('"device": "cpu"', corrected["personDeviceProbe"]["stdout"])
        from Experiments.UAV.run_multicamera_tracking_demo import preflight
        frozen = _load(output / "manifest.json", 4 * 1024 * 1024)
        installed_node = Path(shutil.which("UavTrackingNode")).resolve()
        entries = [entry for entry in frozen["personTracking"]["frozenFiles"]
                   if Path(entry["path"]).resolve() == installed_node]
        self.assertEqual(len(entries), 1, "one authoritative installed Node identity required")
        original_digest = digest_file(installed_node)
        self.assertEqual(entries[0]["digest"], original_digest)
        # Exercise the production closure guard without touching a shared installed binary.
        entries[0]["digest"] = "sha256:" + "0" * 64
        self.assertNotEqual(entries[0]["digest"], original_digest)
        refused = preflight(frozen, run=True, headless=True)
        with (root / "installed-closure-assessment.json").open("x") as stream:
            json.dump({"boundary": "declared expected installed digest differs from actual bytes",
                       "installedPath": str(installed_node), "actualDigest": original_digest,
                       "injectedExpectedDigest": entries[0]["digest"], "preflight": refused},
                      stream, indent=2)
        self.assertFalse(refused["ok"])
        self.assertIsNone(refused["personDeviceProbe"], "reject closure before backend probing")
        self.assertTrue(any("frozen person execution input changed:" in error and
                            entries[0]["path"] in error for error in refused["errors"]),
                        refused["errors"])
        self.assertEqual(digest_file(installed_node), original_digest)
        self.assertFalse((output / "person-network-attempt.json").exists())
        self.assertFalse((output / "compute.log").exists())
        self.assertFalse((output / "private/compute/worker").exists())
        self.assertFalse((output / "private/keychain").exists())
        with (root / "F8-preflight-result.json").open("x") as stream:
            json.dump({"status": "PASS", "cases": ["wrong-weights", "cuda-unavailable", "cpu-recovery",
                                                   "declared-installed-closure-mismatch"],
                       "scope": "installed preflight variants only; no model inference or MiniNDN run",
                       "remaining": ["F1-F7 and full recovery matrix"]},
                      stream, indent=2)


if __name__ == "__main__":
    unittest.main()
