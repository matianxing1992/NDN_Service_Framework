"""Independent tiny-media tests for preparation, not tracking-quality evidence."""
from __future__ import annotations

import base64
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'NDNSF-UAV-APP/tracking'))
sys.path.insert(0, str(ROOT / 'Experiments/UAV'))
from person_profile import load_person_profile, prepare_person_profile
from tracking_artifacts import ArtifactError, load_person_source_manifest
from run_multicamera_tracking_demo import (RunnerError, prepare_person_sources, build_plan,
                                           _verify_person_plan, _video_presentation_timestamps, main)


class PersonSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
            raise unittest.SkipTest('real source preparation needs ffmpeg/ffprobe')
        cls.assets = tempfile.TemporaryDirectory()
        for index, color in enumerate(('red', 'green', 'blue')):
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                            f'color=c={color}:s=32x24:r=10:d=2', '-an', '-c:v', 'mpeg4', '-threads', '1',
                            str(Path(cls.assets.name) / f'view{index}.mp4')],
                           check=True, capture_output=True, timeout=10)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                        'testsrc2=s=32x24:r=10:d=2', '-an', '-c:v', 'libx264',
                        '-bf', '3', '-g', '20', '-x264-params', 'b-adapt=0:scenecut=0',
                        '-threads', '1', str(Path(cls.assets.name) / 'b-frames.mp4')],
                       check=True, capture_output=True, timeout=10)

    @classmethod
    def tearDownClass(cls):
        cls.assets.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.profile = load_person_profile(ROOT / 'NDNSF-UAV-APP/configs/uav_person_tracking_demo.json')
        self.value = dict(schema='uav-person-source-manifest-v1',
                          dataset=dict(name='synthetic UI-independent source test',
                                       uri='test://three-distinct-solid-color-videos', license='test-generated'),
                          commonStartUs=1000000, durationUs=1000000, cameras=[])
        for index, camera in enumerate(('UAV1', 'UAV2', 'UAV3')):
            name = f'view{index}.mp4'
            shutil.copyfile(Path(self.assets.name) / name, self.root / name)
            payload = (self.root / name).read_bytes()
            self.value['cameras'].append(dict(cameraId=camera, viewId=f'view-{index}',
                                              videoPath=name, videoStartUs=500000,
                                              videoDigest='sha256:' + hashlib.sha256(payload).hexdigest()))
        self.path = self.root / 'source.json'
        self.output = self.root / 'prepared'
        self.write()

    def write(self):
        self.path.write_text(json.dumps(self.value))

    def test_bounded_pts_keep_original_frame_ordinals_and_legacy_whole_video(self):
        video = self.root / 'view0.mp4'
        whole = _video_presentation_timestamps(video)
        bounded = _video_presentation_timestamps(video, end_time_us=1500000)
        self.assertEqual(whole, [index * 100000 for index in range(20)])
        self.assertEqual(bounded, whole[:15])
        reordered = Path(self.assets.name) / 'b-frames.mp4'
        metadata = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                                   '-show_entries', 'stream=has_b_frames', '-of', 'json', str(reordered)],
                                  check=True, capture_output=True, text=True, timeout=10)
        self.assertGreater(json.loads(metadata.stdout)['streams'][0]['has_b_frames'], 0)
        whole = _video_presentation_timestamps(reordered)
        self.assertEqual(whole, [index * 100000 for index in range(20)])
        self.assertEqual(_video_presentation_timestamps(reordered, end_time_us=1500000), whole[:15])
        with patch('run_multicamera_tracking_demo.subprocess.run') as launch:
            for invalid in (0, -1, True, 1.5, '1500000', 2**63):
                with self.subTest(invalid=invalid):
                    with self.assertRaisesRegex(RunnerError, 'PTS interval end'):
                        _video_presentation_timestamps(video, end_time_us=invalid)
            launch.assert_not_called()
        future_frame = subprocess.CompletedProcess([], 0, json.dumps({
            'frames': [{'best_effort_timestamp_time': '1.000000'}]}), '')
        with patch('run_multicamera_tracking_demo.subprocess.run', return_value=future_frame):
            with self.assertRaisesRegex(RunnerError, 'no decoded frames before'):
                _video_presentation_timestamps(video, end_time_us=500000)

    def test_dataset_origin_is_explicit_and_survives_preparation(self):
        legacy = load_person_source_manifest(self.path, self.profile.limits)
        self.assertEqual(legacy['datasetOrigin'], 'unspecified')
        for origin in ('real-captured', 'synthetic-rendered'):
            with self.subTest(origin=origin):
                self.value['datasetOrigin'] = origin
                self.write()
                admitted = load_person_source_manifest(self.path, self.profile.limits)
                self.assertEqual(admitted['datasetOrigin'], origin)
                self.assertEqual(admitted['sourceIds'], legacy['sourceIds'])
                self.assertNotEqual(admitted['manifestDigest'], legacy['manifestDigest'])
                receipt = prepare_person_sources(self.path, self.root / origin, self.profile)
                self.assertEqual(receipt['datasetOrigin'], origin)
        for invalid in ('unspecified', 'synthetic', None, True, {}):
            with self.subTest(invalid=invalid):
                self.value['datasetOrigin'] = invalid
                self.write()
                with self.assertRaisesRegex(ArtifactError, 'dataset origin'):
                    load_person_source_manifest(self.path, self.profile.limits)

    def test_person_plan_keeps_prepared_identity_and_detects_input_tampering(self):
        # Weights and descriptor here test planning only; native inference is not exercised.
        detector = self.root / 'detector.pt'
        reid = self.root / 'reid.pth'
        detector.write_bytes(b'plan-detector-fixture')
        reid.write_bytes(b'plan-reid-fixture')
        profile = prepare_person_profile(ROOT / 'NDNSF-UAV-APP/configs/uav_person_tracking_demo.json',
                                         detector, reid)
        receipt = prepare_person_sources(self.path, self.output, profile)
        descriptor = self.output / 'private/config/person-computation.json'
        descriptor.write_text('{}')
        person = dict(profile=receipt['trackingProfile'], computationManifest=str(descriptor),
                      reidModel=str(reid), workerPython=sys.executable)
        def installed_script(name):
            path = self.root / 'libexec' / name
            path.parent.mkdir(exist_ok=True)
            path.write_text(name)
            return path
        # Resolve installed-file provenance to a real fixture file, without requiring host install.
        with patch('run_multicamera_tracking_demo._installed_program', return_value=str(detector)), \
                patch('run_multicamera_tracking_demo._installed_tracking_script', side_effect=installed_script):
            plan = build_plan(source=self.root, model=detector, output=self.output,
                              license_text='test', person_tracking=person, run_seconds=1, headless=True)
            bound = profile.limits['runDeadlineMs']
            bounded = build_plan(source=self.root, model=detector, output=self.output,
                                 license_text='test', person_tracking=person, run_seconds=1,
                                 headless=True, global_deadline_ms=bound)
            self.assertEqual(bounded['settings']['globalDeadlineMs'], bound)
            with self.assertRaisesRegex(RunnerError, 'invalid person deadline'):
                build_plan(source=self.root, model=detector, output=self.output,
                           license_text='test', person_tracking=person, run_seconds=1,
                           headless=True, global_deadline_ms=bound + 1)
        self.assertEqual(plan['observation']['sourceEpoch'], receipt['sourceEpoch'])
        self.assertEqual(plan['observation']['sourceIds'], receipt['sourceIds'])
        self.assertEqual(plan['settings']['windowCount'], 2)
        self.assertEqual(plan['settings']['requestedDevice'], 'cpu')
        self.assertNotIn('cpuOnly', plan['settings'])
        self.assertFalse(plan['security']['credentialsPrepared'])
        self.assertFalse((self.output / 'private/keychain').exists())
        self.assertFalse((self.output / 'person-network-attempt.json').exists())
        _verify_person_plan(plan)
        clock = self.root / 'libexec/display_clock.py'
        original_clock = clock.read_bytes()
        clock.write_bytes(b'changed display clock')
        with self.assertRaisesRegex(RunnerError, 'frozen person execution input changed'):
            _verify_person_plan(plan)
        clock.write_bytes(original_clock)
        _verify_person_plan(plan)
        target = self.output / 'private/observations/UAV2/window-1.jsonl'
        target.write_bytes(target.read_bytes() + b' ')
        with self.assertRaisesRegex(RunnerError, 'frozen person execution input changed'):
            _verify_person_plan(plan)

    def test_person_cli_rejects_partial_mode_and_reuse_before_preparing(self):
        with patch('run_multicamera_tracking_demo.prepare_person_sources') as prepare:
            self.assertEqual(main(['--prepare', '--tracking-profile', str(self.path)]), 2)
            self.output.mkdir()
            (self.output / 'person-network-attempt.json').write_text('{}')
            self.assertEqual(main(['--prepare', '--tracking-profile', str(self.path),
                '--source-manifest', str(self.path), '--reid-model', str(self.root / 'model'),
                '--output-dir', str(self.output)]), 2)
            prepare.assert_not_called()

    def test_three_distinct_videos_are_pinned_without_evaluator_fields(self):
        result = load_person_source_manifest(self.path, self.profile.limits)
        self.assertEqual(result['windowCount'], 2)
        self.assertEqual(result['commonStartUs'], 1000000)
        self.assertEqual(len(set(result['sourceIds'].values())), 3)
        self.assertEqual([item['framePeriodUs'] for item in result['cameras']], [100000] * 3)
        self.assertTrue(all(Path(item['videoPath']).is_absolute() for item in result['cameras']))
        self.assertFalse(any('evaluation' in key.lower() for key in result))

    def test_manifest_rejects_labels_duplicate_keys_and_invalid_intervals(self):
        original = copy.deepcopy(self.value)
        for location, key in [('root', 'evaluationManifest'), ('camera', 'globalIds'),
                              ('dataset', 'groundTruthPath')]:
            with self.subTest(location=location):
                self.value = copy.deepcopy(original)
                target = (self.value if location == 'root' else
                          self.value['cameras'][0] if location == 'camera' else self.value['dataset'])
                target[key] = 'forbidden'
                self.write()
                with self.assertRaises(ArtifactError):
                    load_person_source_manifest(self.path, self.profile.limits)
        for key, value in [('durationUs', 0), ('durationUs', 500001),
                           ('commonStartUs', True), ('commonStartUs', 2**63 - 1)]:
            with self.subTest(key=key, value=value):
                self.value = copy.deepcopy(original)
                self.value[key] = value
                self.write()
                with self.assertRaises(ArtifactError):
                    load_person_source_manifest(self.path, self.profile.limits)
        self.path.write_text('{"schema":"first","schema":"second"}')
        with self.assertRaises(ArtifactError):
            load_person_source_manifest(self.path, self.profile.limits)

    def test_repeated_video_view_hash_and_wrong_camera_order_are_rejected(self):
        original = copy.deepcopy(self.value)
        for change in ('file', 'view', 'copy', 'order'):
            with self.subTest(change=change):
                self.value = copy.deepcopy(original)
                if change in ('file', 'copy'):
                    first = self.value['cameras'][0]
                    other = self.value['cameras'][1]
                    other['videoDigest'] = first['videoDigest']
                    if change == 'file':
                        other['videoPath'] = first['videoPath']
                    else:
                        shutil.copyfile(self.root / first['videoPath'], self.root / 'byte-copy.mp4')
                        other['videoPath'] = 'byte-copy.mp4'
                elif change == 'view':
                    self.value['cameras'][1]['viewId'] = self.value['cameras'][0]['viewId']
                else:
                    self.value['cameras'].reverse()
                self.write()
                with self.assertRaises(ArtifactError):
                    load_person_source_manifest(self.path, self.profile.limits)

    def test_video_digest_and_profile_image_bounds_are_enforced_before_decode(self):
        self.value['cameras'][0]['videoDigest'] = 'sha256:' + '0' * 64
        self.write()
        with self.assertRaisesRegex(ArtifactError, 'digest mismatch'):
            load_person_source_manifest(self.path, self.profile.limits)
        self.value['cameras'][0]['videoDigest'] = 'sha256:' + hashlib.sha256((self.root / 'view0.mp4').read_bytes()).hexdigest()
        self.write()
        limits = dict(self.profile.limits, maxFrameWidth=16)
        with self.assertRaisesRegex(ArtifactError, 'dimensions'):
            load_person_source_manifest(self.path, limits)

    def test_preparation_reuses_shared_writer_and_records_actual_raw_common_mapping(self):
        result = prepare_person_sources(self.path, self.output, self.profile)
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['synchronizationQualification'], 'unverified')
        inputs = json.loads(Path(result['sourceFrameInputs']).read_text())
        for camera in ('UAV1', 'UAV2', 'UAV3'):
            sampling = inputs['sampling'][camera]
            self.assertEqual(sampling['frameIndices'], [5, 10])
            self.assertEqual(sampling['rawPresentationTimeUs'], [500000, 1000000])
            self.assertEqual(sampling['targetTimeUs'], [1000000, 1500000])
            self.assertEqual(sampling['captureTimeUs'], [1000000, 1500000])
            self.assertEqual(sampling['targetOffsetUs'], [0, 0])
            for window, path in enumerate(inputs['frames'][camera]):
                events = [json.loads(line) for line in Path(path).read_text().splitlines()]
                frame = next(event['payload'] for event in events if event['kind'] == 'frame')
                self.assertEqual(frame['deviceTimeUs'], str(1000000 + window * 500000))
                self.assertEqual(frame['width'], '32')
                self.assertEqual(frame['height'], '24')
                payload = base64.b64decode(frame['imageBase64'], validate=True)
                self.assertEqual('sha256:' + hashlib.sha256(payload).hexdigest(), sampling['inputDigests'][window])
                self.assertEqual(frame['key']['sourceId'], result['sourceIds'][camera])
                self.assertEqual(frame['key']['epoch'], str(result['sourceEpoch']))
        observation = json.loads(Path(result['observationProfile']).read_text())
        self.assertEqual(observation['acquisitionKind'], 'simulated')
        self.assertEqual(observation['geometryValidity'], 'illustrative')
        self.assertEqual(observation['accuracyQualification'], 'unvalidated')

    def test_retry_keeps_epoch_and_bytes_and_tampering_cannot_produce_stale_pass(self):
        first = prepare_person_sources(self.path, self.output, self.profile)
        path = self.output / 'private/observations/UAV1/window-0.jsonl'
        original = path.read_bytes()
        second = prepare_person_sources(self.path, self.output, self.profile)
        self.assertEqual(first, second)
        self.assertEqual(path.read_bytes(), original)
        path.write_bytes(original + b'altered')
        with self.assertRaisesRegex(RunnerError, 'immutable prepared frame'):
            prepare_person_sources(self.path, self.output, self.profile)
        self.assertEqual(json.loads((self.output / 'prepared-person-sources.json').read_text())['status'], 'FAIL')
        self.assertEqual(path.read_bytes(), original + b'altered')

    def test_source_epoch_restart_changes_all_three_valid_frames_after_window_zero(self):
        receipt = prepare_person_sources(
            self.path, self.output, self.profile, source_restart_after_window_zero=True)
        self.assertEqual(receipt['status'], 'PASS')
        self.assertTrue(receipt['sourceRestartAfterWindowZero'])
        generations = json.loads(Path(receipt['sourceGenerations']).read_text())
        self.assertEqual(generations['windowCount'], 2)
        self.assertEqual([item['firstWindowIndex'] for item in generations['generations']], [0, 1])
        first, restarted = generations['generations']
        self.assertEqual(first['sourceIds'], receipt['sourceIds'])
        self.assertEqual(first['sourceEpoch'], receipt['sourceEpoch'])
        self.assertEqual(restarted['sourceEpoch'], receipt['sourceEpoch'] + 1)
        self.assertEqual(set(first['sourceIds']), {'UAV1', 'UAV2', 'UAV3'})
        for camera in ('UAV1', 'UAV2', 'UAV3'):
            self.assertNotEqual(first['sourceIds'][camera], restarted['sourceIds'][camera])
            self.assertTrue(restarted['sourceIds'][camera].endswith('-g2'))
            for window, generation in enumerate((first, restarted)):
                path = Path(self.output / f'private/observations/{camera}/window-{window}.jsonl')
                events = [json.loads(line) for line in path.read_text().splitlines()]
                frame = next(event['payload'] for event in events if event['kind'] == 'frame')
                self.assertEqual(frame['key']['sourceId'], generation['sourceIds'][camera])
                self.assertEqual(frame['key']['epoch'], str(generation['sourceEpoch']))

    def test_source_epoch_restart_fault_is_person_only_and_requires_two_windows(self):
        with self.assertRaisesRegex(RunnerError, 'person-specific fault requires person tracking'):
            build_plan(source=self.root, model=self.root / 'unused.pt',
                       output=self.root / 'non-person', license_text='',
                       fault='person-source-epoch-restart-after-window-0')
        self.value['durationUs'] = 500000
        self.write()
        output = self.root / 'single-window'
        with self.assertRaisesRegex(RunnerError, 'source switches must occur'):
            prepare_person_sources(
                self.path, output, self.profile, source_restart_after_window_zero=True)
        receipt = json.loads((output / 'prepared-person-sources.json').read_text())
        self.assertEqual(receipt['status'], 'FAIL')
        self.assertIn('source switches must occur', receipt['failureReason'])

    def test_post_pass_video_admission_failure_invalidates_only_matching_owned_receipt(self):
        prepare_person_sources(self.path, self.output, self.profile)
        video = self.root / 'view0.mp4'
        video.write_bytes(video.read_bytes() + b'changed after successful preparation')
        with self.assertRaisesRegex(ArtifactError, 'digest mismatch'):
            prepare_person_sources(self.path, self.output, self.profile)
        receipt = json.loads((self.output / 'prepared-person-sources.json').read_text())
        self.assertEqual(receipt['status'], 'FAIL')
        self.assertIn('digest mismatch', receipt['failureReason'])

    def test_too_short_input_and_foreign_output_do_not_publish_success(self):
        self.output.mkdir()
        (self.output / 'unrelated.txt').write_text('keep')
        with self.assertRaisesRegex(RunnerError, 'owned directory'):
            prepare_person_sources(self.path, self.output, self.profile)
        self.assertEqual((self.output / 'unrelated.txt').read_text(), 'keep')
        self.value['durationUs'] = 3000000
        self.write()
        with self.assertRaisesRegex(RunnerError, 'declared interval'):
            prepare_person_sources(self.path, self.root / 'too-short', self.profile)
        self.assertEqual(json.loads((self.root / 'too-short/prepared-person-sources.json').read_text())['status'], 'FAIL')

    def test_lateness_and_decoder_failure_release_owned_capture_and_fail_receipt(self):
        import run_multicamera_tracking_demo as runner
        with patch.object(runner, '_video_presentation_timestamps', return_value=[700000, 1200000, 1700000]):
            with self.assertRaisesRegex(RunnerError, 'lateness'):
                prepare_person_sources(self.path, self.output, self.profile)
        with patch('cv2.VideoCapture') as factory:
            factory.return_value.isOpened.return_value = True
            factory.return_value.read.return_value = (False, None)
            with self.assertRaisesRegex(RunnerError, 'cannot decode PTS-selected frame'):
                prepare_person_sources(self.path, self.output, self.profile)
            factory.return_value.release.assert_called_once()
        self.assertEqual(json.loads((self.output / 'prepared-person-sources.json').read_text())['status'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
