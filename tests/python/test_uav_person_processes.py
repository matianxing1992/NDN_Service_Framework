"""Role boundary assertions; these do not qualify native distributed execution."""
from pathlib import Path
import json
import os
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Experiments/UAV"))
from run_multicamera_tracking_demo import (_process_specs, _describe_person_computation,
                                           _uav_socket_root, _uav_unix_socket_path,
                                           _provision_run_security, _person_final_display_shown,
                                           _person_failure_display_presented,
                                           build_plan, preflight, RunnerError)


class PersonProcessTest(unittest.TestCase):
    def setUp(self):
        self.output = Path("/tmp/spec219-process-plan")
        self.person = dict(profile="/frozen/person.json", computationManifest="/frozen/closure.json",
                           reidModel="/models/osnet.pth", workerPython="/locked/bin/python")
        self.kwargs = dict(source=Path("/original/media"), model=Path("/models/yolo.pt"),
                           headless=True, window_count=120, fault="none", request_timeout_ms=60000,
                           motion_dir=None, source_ids={c: f"source-{c}" for c in
                                                       ("UAV1", "UAV2", "UAV3")},
                           source_epoch=37, observation_profile=dict(worldFrameId="indoor-replay",
                               acquisitionKind="simulated", deliveryMode="replay"),
                           source_generations_path=Path("/frozen/generations.json"),
                           trust_schema=Path("/frozen/trust.conf"))

    def specs(self, **overrides):
        return {s.name: s for s in _process_specs(
            ROOT, self.output, **(self.kwargs | overrides), person_tracking=self.person)}

    def test_sources_receive_observation_inputs_without_models_or_raw_fallbacks(self):
        specs = self.specs()
        for camera in ("uav1", "uav2", "uav3"):
            spec = specs[f"{camera}-camera"]
            self.assertEqual(spec.node, camera)
            args = spec.command
            self.assertIn("--observation-stream-dir", args)
            self.assertEqual(args[args.index("--source-epoch") + 1], "37")
            for forbidden in ("--model", "--video-source", "--frame-dir", "--reid-model",
                              "--tracking-profile", "--person-computation-manifest"):
                self.assertNotIn(forbidden, args)
        self.assertEqual({s.node for s in specs.values()}, {"gs", "compute", "uav1", "uav2", "uav3"})

    def test_model_and_verification_arguments_belong_to_actual_roles(self):
        specs = self.specs()
        compute = specs["compute"].command
        gs = specs["ground-station"].command
        for flag, expected in (("--tracking-profile", self.person["profile"]),
                               ("--reid-model", self.person["reidModel"]),
                               ("--worker-python", self.person["workerPython"])):
            self.assertEqual(compute[compute.index(flag) + 1], expected)
        self.assertEqual(gs[gs.index("--person-computation-manifest") + 1],
                         self.person["computationManifest"])
        self.assertNotIn("--reid-model", gs)
        self.assertIn("--no-demo-identities", specs["controller"].command)

    def test_renderer_reads_only_ground_station_projection(self):
        specs = self.specs()
        self.assertNotIn("compute-display", specs)
        display = specs["ground-station-display"]
        self.assertEqual(display.node, "gs")
        self.assertEqual(display.command[0], self.person["workerPython"])
        socket = Path(display.command[display.command.index("--socket") + 1])
        self.assertEqual(socket, _uav_unix_socket_path(
            _uav_socket_root(self.output), "display"))
        self.assertLessEqual(len(os.fsencode(socket)), 107)
        self.assertIn(str(self.output / "private/ground-station/presentation/latest.json"),
                      display.command)
        self.assertIn(str(self.output / "private/ground-station/presentation/context.json"),
                      display.command)
        for arg in display.command:
            self.assertNotIn("private/compute/", arg)
            self.assertNotIn("private/observations/", arg)
        self.assertNotIn("--replay-dir", display.command)
        self.assertNotIn("--playback-clock-manifest", display.command)

    def test_precommit_cancellation_fault_uses_person_roles_and_normal_display(self):
        fault = "user-cancel-before-person-commit"
        specs = self.specs(fault=fault)
        for role in ("compute", "ground-station"):
            command = specs[role].command
            self.assertEqual(command[command.index("--fault") + 1], fault)
        self.assertNotIn(fault, specs["ground-station-display"].command)
        with self.assertRaisesRegex(RunnerError, "requires person tracking"):
            _process_specs(ROOT, self.output, **(self.kwargs | dict(fault=fault)))

    def test_person_binding_faults_only_reach_request_writer(self):
        for fault in ("wrong-person-profile-binding", "wrong-person-computation-binding"):
            with self.subTest(fault=fault):
                specs = self.specs(fault=fault)
                gs = specs["ground-station"].command
                self.assertEqual(gs[gs.index("--fault") + 1], fault)
                for name, spec in specs.items():
                    if name != "ground-station":
                        self.assertNotIn("--fault", spec.command)
                with self.assertRaisesRegex(RunnerError, "requires person tracking"):
                    _process_specs(ROOT, self.output, **(self.kwargs | dict(fault=fault)))
                with self.assertRaisesRegex(RunnerError, "requires person tracking"):
                    build_plan(source=Path("/missing/source"), model=Path("/missing/model"),
                               output=self.output, license_text="", fault=fault)

    def test_incompatible_inputs_and_incomplete_person_paths_fail(self):
        for overrides in (dict(observation_enabled=False), dict(motion_dir=Path("/motion")),
                          dict(external_source_adapter=Path("/adapter")),
                          dict(observation_profile=self.kwargs["observation_profile"] |
                               dict(deliveryMode="live-push"))):
            with self.subTest(overrides=overrides), self.assertRaises(RunnerError):
                self.specs(**overrides)
        for key in self.person:
            original = self.person[key]
            for bad in ("relative/path", None, ""):
                self.person[key] = bad
                with self.subTest(key=key, bad=bad), self.assertRaises(RunnerError):
                    self.specs()
            self.person[key] = original

    def describe_with_fixture(self, root, body):
        # This executable exercises only launcher ownership/environment, not native inference.
        executable = root / "launcher-fixture"
        executable.write_text(f"#!{sys.executable}\n" + body)
        executable.chmod(0o700)
        worker = root / "tracking_worker.py"
        worker.write_text("# fixture path only\n")
        with patch("run_multicamera_tracking_demo._installed_program", return_value=str(executable)), \
                patch("run_multicamera_tracking_demo._installed_tracking_script", return_value=worker):
            return _describe_person_computation(self.person, root / "yolo.pt", root)

    def test_description_uses_isolated_compute_environment_and_exact_native_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.dict(os.environ, {"PYTHONPATH": "/host/repository:/host/minindn",
                                         "LD_LIBRARY_PATH": "/obsolete/sdk"}):
                os.environ.pop("NDNSF_UAV_LIBRARY_PATH", None)
                result = self.describe_with_fixture(root,
                    "import os,json\nprint(json.dumps(dict(home=os.environ['HOME'], "
                    "pythonpath=os.environ['PYTHONPATH'], nousersite=os.environ['PYTHONNOUSERSITE'], "
                    "loader=os.environ.get('LD_LIBRARY_PATH'))))\n")
            descriptor = Path(result["computationManifest"])
            self.assertEqual(descriptor.read_bytes(),
                             (root / "person-computation-inspection.stdout").read_bytes())
            value = json.loads(descriptor.read_bytes())
            self.assertEqual(value["home"], str(root / "private/compute/home"))
            self.assertEqual(value["nousersite"], "1")
            self.assertNotIn("/host/", value["pythonpath"])
            self.assertIsNone(value["loader"])

    def test_failed_description_invalidates_prior_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            descriptor = root / "private/config/person-computation.json"
            descriptor.parent.mkdir(parents=True)
            for body in ("raise SystemExit(7)\n", "print('[]')\n", "print('x' * 65537)\n"):
                descriptor.write_text('{"old":"success"}')
                with self.subTest(body=body), self.assertRaises((RunnerError, ValueError)):
                    self.describe_with_fixture(root, body)
                self.assertFalse(descriptor.exists())

    def test_exited_inspector_leader_does_not_leave_term_ignoring_group_member(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pid_path = root / "descendant.pid"
            child = ("import os,signal,time; from pathlib import Path; "
                     "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                     f"Path({str(pid_path)!r}).write_text(str(os.getpid())); time.sleep(15)")
            body = ("import subprocess,sys,time; from pathlib import Path\n"
                    f"subprocess.Popen([sys.executable, '-c', {child!r}])\n"
                    f"ready=Path({str(pid_path)!r})\n"
                    "deadline=time.monotonic()+3\n"
                    "while not ready.exists():\n"
                    " if time.monotonic()>deadline: raise RuntimeError('child not ready')\n"
                    " time.sleep(.01)\n"
                    "print('{}')\n")
            started = time.monotonic()
            self.describe_with_fixture(root, body)
            self.assertLess(time.monotonic() - started, 6)
            pid = int(pid_path.read_text())
            try:
                deadline = time.monotonic() + 1
                while time.monotonic() < deadline:
                    status = Path(f"/proc/{pid}/stat")
                    if not status.exists() or status.read_text().split(")", 1)[1].split()[0] == "Z":
                        break
                    time.sleep(.02)
                else:
                    self.fail("owned group member survived inspection cleanup")
            finally:
                # Bound even a failed regression; init owns reaping of this orphan.
                try:
                    os.kill(pid, 9)
                except ProcessLookupError:
                    pass

    def test_provisioner_uses_root_signed_rsa_actors_and_rejects_keychain_reuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'private/config').mkdir(parents=True)
            commands = []
            def invoke(command, **kwargs):
                commands.append((command, kwargs))
                return SimpleNamespace(returncode=0, stdout=b'Zml4dHVyZQ==\n', stderr=b'')
            with patch('run_multicamera_tracking_demo.shutil.which', return_value='/installed/ndnsec'), \
                    patch('run_multicamera_tracking_demo.subprocess.run', side_effect=invoke):
                result = _provision_run_security(root)
            self.assertTrue(result['credentialsPrepared'])
            self.assertEqual(len(result['publicCertificates']), 7)
            identities = {item['identity'] for item in result['publicCertificates']}
            self.assertEqual(identities, {'/example/uav', '/example/uav/controller',
                '/example/uav/drone/UAV1', '/example/uav/drone/UAV2', '/example/uav/drone/UAV3',
                '/example/uav/compute', '/example/uav/gs'})
            generated = [args for args, _ in commands if args[1] == 'key-gen']
            self.assertEqual(len(generated), 7)
            self.assertTrue(all(args[2:4] == ['-t', 'r'] for args in generated))
            signed = [args for args, _ in commands if args[1] == 'cert-gen']
            self.assertEqual(len(signed), 6)
            self.assertTrue(all(args[args.index('-s') + 1] == '/example/uav' for args in signed))
            for _args, kwargs in commands:
                self.assertEqual(kwargs['env']['NDN_CLIENT_PIB'],
                                 'pib-sqlite3:' + str(root / 'private/keychain/pib'))
                self.assertLessEqual(kwargs['timeout'], 10)
            with self.assertRaisesRegex(RunnerError, 'fresh run-owned keychain'):
                _provision_run_security(root)

    def test_final_display_barrier_matches_integer_window_and_complete_result_scope(self):
        records = [dict(event='DISPLAY_FRAME_SHOWN', cameraId=camera, runId='run', sessionId='run',
                        epoch=17, window=2, sequence=3, personDisplay=dict(resultDigest='sha256:result'))
                   for camera in ('UAV1', 'UAV2', 'UAV3')]
        def check(items):
            return _person_final_display_shown('\n'.join(json.dumps(x) for x in items),
                run_id='run', epoch=17, window_index=2, result_digest='sha256:result')
        self.assertTrue(check(records))
        self.assertFalse(check(records[:2]))
        for field, wrong in (('runId', 'other'), ('sessionId', 'other-mission'), ('epoch', 18),
                             ('window', 'window-2'), ('window', True), ('sequence', 2),
                             ('personDisplay', dict(resultDigest='sha256:old'))):
            bad = [dict(x) for x in records]
            bad[2][field] = wrong
            with self.subTest(field=field, wrong=wrong):
                self.assertFalse(check(bad))

    def test_failure_presentation_barrier_requires_exact_scope_and_three_post_flush_records(self):
        records = [dict(event='DISPLAY_PERSON_FAILURE_SHOWN', cameraId=camera, mode='windows',
                        runId='run', missionId='run', window=1, projectionDigest='sha256:projection',
                        clockDomain='CLOCK_MONOTONIC_NS', monotonicNs=100)
                   for camera in ('UAV1', 'UAV2', 'UAV3')]
        def check(items, mode='windows'):
            return _person_failure_display_presented('\n'.join(json.dumps(x) for x in items),
                run_id='run', window_index=1, projection_digest='sha256:projection', mode=mode)
        self.assertTrue(check(records))
        self.assertFalse(check(records[:2] + records[:1]))
        for field, wrong in (('runId', 'old'), ('missionId', 'old'), ('window', True),
                             ('window', '1'), ('window', 0), ('projectionDigest', 'sha256:old'),
                             ('event', 'DISPLAY_PERSON_FAILURE'), ('mode', 'headless'),
                             ('cameraId', 'UAV4'), ('cameraId', []),
                             ('clockDomain', 'wall'), ('monotonicNs', True), ('monotonicNs', 0)):
            bad = [dict(x) for x in records]
            bad[2][field] = wrong
            with self.subTest(field=field, wrong=wrong):
                self.assertFalse(check(bad))
        observed = [dict(x, event='DISPLAY_PERSON_FAILURE_OBSERVED', mode='headless') for x in records]
        self.assertTrue(check(observed, mode='headless'))
        self.assertFalse(check(observed))
        self.assertFalse(check(records, mode='headless'))
        self.assertFalse(check(records, mode='other'))

    def test_person_preflight_uses_configured_backend_interpreter(self):
        with tempfile.TemporaryDirectory() as temporary:
            plan = dict(personTracking=dict(preparationReceipt=str(Path(temporary) / 'receipt.json'),
                        workerPython='/locked/bin/python', requestedDevice='cpu', frozenFiles=[]),
                        topology=dict(nodes=['gs', 'compute', 'uav1', 'uav2', 'uav3']), processes=[])
            with patch('run_multicamera_tracking_demo._check_python_dependencies') as host_models, \
                    patch('run_multicamera_tracking_demo.subprocess.run', return_value=
                          SimpleNamespace(returncode=0, stdout=b'Creating new settings\n'
                              b'SPEC219_DEVICE_PROBE {"schema":"uav-person-device-probe-v1","device":"cpu"}\n',
                              stderr=b'')) as probe:
                result = preflight(plan, run=False, headless=True)
            self.assertTrue(result['ok'])
            host_models.assert_not_called()
            self.assertEqual(probe.call_args.args[0][0], '/locked/bin/python')
            self.assertIn('ultralytics,lap,torchreid', probe.call_args.args[0][2])
            self.assertNotIn('/host/', probe.call_args.kwargs['env']['PYTHONPATH'])
            self.assertIn('Creating new settings', result['personDeviceProbe']['stdout'])
            valid = b'SPEC219_DEVICE_PROBE {"schema":"uav-person-device-probe-v1","device":"cpu"}\n'
            for code, stdout in ((1, valid), (0, b'cpu\n'), (0, valid + valid),
                                  (0, b'SPEC219_DEVICE_PROBE broken\n'),
                                  (0, valid.replace(b'cpu', b'cuda:0'))):
                with self.subTest(code=code, stdout=stdout), \
                        patch('run_multicamera_tracking_demo.subprocess.run', return_value=
                              SimpleNamespace(returncode=code, stdout=stdout, stderr=b'probe detail')):
                    failed = preflight(plan, run=False, headless=True)
                    self.assertFalse(failed['ok'])
                    self.assertEqual(failed['personDeviceProbe']['stderr'], 'probe detail')


if __name__ == "__main__":
    unittest.main()
