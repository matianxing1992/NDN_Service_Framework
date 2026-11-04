"""GS presentation admission fixtures; these are not tracking-quality evidence."""
from __future__ import annotations

import copy
import hashlib
import io
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'NDNSF-UAV-APP/tracking'))
from tracking_display import (CAMERAS, ComputeRenderer, PersonPresentationReader,
                              _track_color, _track_metric_label, _track_status_label, serve)


def digest(payload: bytes) -> str:
    return 'sha256:' + hashlib.sha256(payload).hexdigest()


class PersonDisplayTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.scope = dict(runId='display-run', missionId='display-mission',
                          profileDigest='sha256:' + '1' * 64,
                          computationDigest='sha256:' + '2' * 64)
        self.context = dict(self.scope, schema='uav-person-display-context-v1',
                            requestedDevice='cpu', windowCount=4, windowPrefix='window',
                            maxFrameBytes=4194304, maxFrameWidth=1920, maxFrameHeight=1080,
                            maxPersonsPerFrame=64,
                            generations=[dict(firstWindowIndex=0, sourceEpoch=7,
                                              sourceIds=['kitchen', 'corridor', 'phd_room']),
                                         dict(firstWindowIndex=2, sourceEpoch=8,
                                              sourceIds=['kitchen-b', 'corridor-b', 'phd_room-b'])])
        self.context_path = self.root / 'context.json'
        self.context_path.write_text(json.dumps(self.context))
        buffer = io.BytesIO()
        Image.new('RGB', (16, 12), 'white').save(buffer, format='JPEG')
        self.jpeg = buffer.getvalue()
        # Repeated tiny images are UI fixtures, never a multi-camera dataset.
        self.result = dict(self.scope, schema='uav-person-tracking-result-v1', terminal=True,
                           windowId='window-0', sequence=1, trackingMode='indoor-person-reid',
                           reidInferenceBatches=3, frames=[])
        for key in ('requestedDevice', 'actualDetectorDevice', 'detectorModelDevice',
                    'detectorInputDevice', 'actualReidDevice', 'reidModelDevice', 'reidInputDevice'):
            self.result[key] = 'cpu'
        images = []
        (self.root / 'window-0').mkdir()
        for index, camera in enumerate(CAMERAS):
            path = f'window-0/{camera}.jpg'
            (self.root / path).write_bytes(self.jpeg)
            reference = dict(name=f'/source/{camera}/FRAME', digest=digest(self.jpeg))
            track = dict(cameraId=camera, sourceId=self.context['generations'][0]['sourceIds'][index],
                         sourceEpoch=7, localId=10 + index, globalId=42, box=[1, 1, 15, 11],
                         **{'class': 'person'}, associationStatus='MATCHED', appearanceDistance=0.125)
            self.result['frames'].append(dict(cameraId=camera, sourceId=track['sourceId'],
                                              sourceEpoch=7, sequence=1, inputDigest=digest(self.jpeg),
                                              commonPtsUs=1000000, width=16, height=12,
                                              tracks=[track], observation=dict(imageRef=reference)))
            images.append(dict(cameraId=camera, path=path, inputDigest=digest(self.jpeg)))
        self.projection = dict(self.scope, schema='uav-person-display-v1', state='ready',
                               images=images, gsElapsedUs=3000000, mediaOriginUs=1000000,
                               timeBasis='gs-bootstrap-elapsed-vs-first-common-media-pts')
        self.path = self.root / 'latest.json'
        self.write()
        self.reader = PersonPresentationReader(self.context_path)

    def write(self):
        encoded = json.dumps(self.result)
        self.projection.update(resultJson=encoded, resultDigest=digest(encoded.encode()))
        self.path.write_text(json.dumps(self.projection))

    def test_three_frames_preserve_native_ids_bytes_and_global_colors(self):
        frames, failure = self.reader.load(self.path)
        self.assertIsNone(failure)
        self.assertEqual(len(frames), 3)
        self.assertEqual([frame.camera_id for frame in frames], list(CAMERAS))
        self.assertEqual([frame.image_jpeg for frame in frames], [self.jpeg] * 3)
        self.assertEqual([frame.tracks[0]['localId'] for frame in frames], [10, 11, 12])
        self.assertEqual(len({_track_color(frame.tracks[0]) for frame in frames}), 1)
        self.assertEqual(frames[0].person_display['replayLagUs'], 3000000)
        label = _track_metric_label(frames[0].tracks[0])
        self.assertIn('G42 / L10', label)
        self.assertIn('cosine=0.125', label)
        self.assertNotIn('km/h', label)
        self.assertEqual(_track_status_label(dict(frames[0].tracks[0], globalId=None)), 'UNASSOCIATED')
        self.assertEqual(_track_color(dict(frames[0].tracks[0], globalId=None)), (150, 150, 150))

    def test_frozen_scope_and_all_source_fields_reject_mismatches(self):
        original = copy.deepcopy(self.result)
        for key in self.scope:
            with self.subTest(key=key):
                self.result = copy.deepcopy(original)
                self.result[key] += '-other'
                self.write()
                with self.assertRaises(ValueError):
                    self.reader.load(self.path)
        for key, value in [('sourceId', 'other'), ('sourceEpoch', 8), ('sequence', 2),
                           ('cameraId', 'UAV1'), ('width', 15)]:
            with self.subTest(frame_key=key):
                self.result = copy.deepcopy(original)
                self.result['frames'][2][key] = value
                self.write()
                with self.assertRaises(ValueError):
                    self.reader.load(self.path)
        self.context_path.write_text('{}')
        self.result = original
        self.write()
        self.assertEqual(len(self.reader.load(self.path)[0]), 3)

    def test_empty_person_window_accepts_native_not_run_reid_contract(self):
        self.result['reidInferenceBatches'] = 0
        self.result['actualReidDevice'] = self.result['reidInputDevice'] = 'not-run'
        for frame in self.result['frames']:
            frame['tracks'] = []
        self.write()
        frames, _ = self.reader.load(self.path)
        self.assertTrue(all(not frame.tracks for frame in frames))
        self.assertEqual(frames[0].person_display['reidDevice'], 'not-run')
        self.result['actualReidDevice'] = 'cpu'
        self.write()
        with self.assertRaises(ValueError):
            self.reader.load(self.path)

    def test_image_tampering_escape_symlink_and_missing_third_frame_fail_closed(self):
        third = self.root / 'window-0/UAV3.jpg'
        third.write_bytes(self.jpeg + b'tamper')
        with self.assertRaises(ValueError):
            self.reader.load(self.path)
        third.unlink()
        with self.assertRaises(FileNotFoundError):
            self.reader.load(self.path)
        third.symlink_to(self.root / 'window-0/UAV1.jpg')
        with self.assertRaises(ValueError):
            self.reader.load(self.path)
        third.unlink()
        third.write_bytes(self.jpeg)
        self.projection['images'][2]['path'] = '../source.jpg'
        self.write()
        with self.assertRaises(ValueError):
            self.reader.load(self.path)

    def test_result_digest_duplicate_json_and_bounds_are_enforced(self):
        self.projection['resultDigest'] = 'sha256:' + '0' * 64
        self.path.write_text(json.dumps(self.projection))
        with self.assertRaises(ValueError):
            self.reader.load(self.path)
        self.path.write_text('{"schema":"one","schema":"two"}')
        with self.assertRaises(ValueError):
            self.reader.load(self.path)
        self.result['frames'][0]['tracks'] *= 65
        self.write()
        with self.assertRaises(ValueError):
            self.reader.load(self.path)

    def test_failure_keeps_last_good_distinct_and_first_failure_has_no_fake_frames(self):
        (self.root / 'window-0/presentation.json').write_bytes(self.path.read_bytes())
        failure = dict(self.scope, schema='uav-person-display-v1', state='failure',
                       windowId='window-1', reason='authenticated source timeout',
                       lastGoodPresentation='window-0/presentation.json')
        self.path.write_text(json.dumps(failure))
        frames, status = self.reader.load(self.path)
        self.assertEqual([f.window for f in frames], [0, 0, 0])
        self.assertEqual(status, dict(window=1, reason='authenticated source timeout',
            runId=self.scope['runId'], missionId=self.scope['missionId'],
            projectionDigest=digest(self.path.read_bytes())))
        # A failed retry retains the earlier successful attempt of this window.
        failure['windowId'] = 'window-0'
        self.path.write_text(json.dumps(failure))
        same_frames, status = self.reader.load(self.path)
        self.assertEqual(same_frames, frames)
        self.assertEqual(status['window'], 0)
        self.assertEqual(status['projectionDigest'], digest(self.path.read_bytes()))
        failure['lastGoodPresentation'] = 'window-1/presentation.json'
        self.path.write_text(json.dumps(failure))
        with self.assertRaisesRegex(ValueError, 'invalid last-good'):
            self.reader.load(self.path)
        failure.update(windowId='window-2', lastGoodPresentation='window-1/presentation.json')
        (self.root / 'window-1').mkdir()
        (self.root / 'window-1/presentation.json').write_bytes(
            (self.root / 'window-0/presentation.json').read_bytes())
        self.path.write_text(json.dumps(failure))
        with self.assertRaisesRegex(ValueError, 'last-good result window mismatch'):
            self.reader.load(self.path)
        failure.update(windowId='window-0', lastGoodPresentation='')
        self.path.write_text(json.dumps(failure))
        self.assertEqual(self.reader.load(self.path), ((), dict(window=0, reason=failure['reason'],
            runId=self.scope['runId'], missionId=self.scope['missionId'],
            projectionDigest=digest(self.path.read_bytes()))))

    def test_replay_lag_increases_and_stale_failure_state_remains_explicit(self):
        frame = self.reader.load(self.path)[0][0]
        now = [100]
        renderer = ComputeRenderer('headless', hold_seconds=2, steady_now_us=lambda: now[0])
        renderer.start()
        self.addCleanup(renderer.close)
        renderer.submit(frame)
        renderer.pump_once()
        now[0] += 2000000
        renderer.pump_once()
        self.assertEqual(renderer.person_state(frame, now[0])['replayLagUs'], 5000000)
        self.assertEqual(renderer.person_state(frame, now[0])['freshness'], 'stale')
        failure = dict(window=1, reason='source timeout')
        renderer.set_person_failure(failure)
        self.assertEqual(renderer.person_state(frame, now[0])['failure'], failure)

    def test_actual_opencv_canvas_uses_global_badges_and_failure_overlay(self):
        # Exercise real decode/resize/drawing, replacing only the desktop calls.
        # This is a canvas fixture test, not a real GUI or experiment screenshot.
        from contextlib import ExitStack
        import cv2
        frames, _ = self.reader.load(self.path)
        canvases = {}
        labels = []
        original_put_text = cv2.putText
        def record_text(image, text, *args, **kwargs):
            labels.append(text)
            return original_put_text(image, text, *args, **kwargs)
        with ExitStack() as stack:
            stack.enter_context(patch.dict('os.environ', {'DISPLAY': ':fixture-only'}))
            for name in ('namedWindow', 'resizeWindow', 'moveWindow', 'destroyWindow'):
                stack.enter_context(patch.object(cv2, name))
            wait_key = stack.enter_context(patch.object(cv2, 'waitKey', return_value=-1))
            output = stack.enter_context(patch('sys.stdout', new_callable=io.StringIO))
            stack.enter_context(patch.object(cv2, 'imshow',
                                             side_effect=lambda name, image: canvases.update({name: image.copy()})))
            stack.enter_context(patch.object(cv2, 'putText', side_effect=record_text))
            renderer = ComputeRenderer('windows', steady_now_us=lambda: 100)
            renderer.start()
            try:
                for frame in frames:
                    renderer.submit(frame)
                self.assertTrue(renderer.pump_once())
                self.assertEqual(len(canvases), 3)
                self.assertTrue(all(image.shape == (700, 600, 3) for image in canvases.values()))
                for local_id in (10, 11, 12):
                    self.assertIn(f'G42 / L{local_id}', labels)
                self.assertIn('Geometry UNKNOWN: motion metrics unavailable', labels)
                self.assertIn('YOLO: cpu | OSNet: cpu', labels)
                labels.clear()
                failure = dict(window=1, reason='source timeout', runId='display-run',
                    missionId='display-mission', projectionDigest='sha256:' + 'a' * 64)
                renderer.set_person_failure(failure)
                output.truncate(0)
                output.seek(0)
                def flush(_wait_ms):
                    self.assertNotIn('DISPLAY_PERSON_FAILURE_SHOWN', output.getvalue())
                    self.assertEqual(len(canvases), 3)
                    return -1
                wait_key.side_effect = flush
                renderer.pump_once()
                self.assertIn('FAILED window 1; LAST GOOD window 0', labels)
                self.assertIn('source timeout', labels)
                events = [json.loads(line) for line in output.getvalue().splitlines()]
                shown = [item for item in events if item['event'] == 'DISPLAY_PERSON_FAILURE_SHOWN']
                self.assertEqual({item['cameraId'] for item in shown}, set(CAMERAS))
                self.assertEqual(len(shown), 3)
                self.assertTrue(all(item['mode'] == 'windows' and
                    item['projectionDigest'] == failure['projectionDigest'] and
                    item['clockDomain'] == 'CLOCK_MONOTONIC_NS' and item['monotonicNs'] > 0
                    for item in shown))
                output.truncate(0)
                output.seek(0)
                renderer.pump_once()
                self.assertNotIn('DISPLAY_PERSON_FAILURE_SHOWN', output.getvalue())
                # First failure has no source frame; all three blank canvases still flush.
                self.assertEqual(renderer.status['shownFrames'], 3)
                renderer.close()
                renderer = ComputeRenderer('windows', steady_now_us=lambda: 100)
                renderer.start()
                renderer.set_person_failure(dict(failure, window=2))
                output.truncate(0)
                output.seek(0)
                canvases.clear()
                renderer.pump_once()
                self.assertEqual(len(canvases), 3)
                self.assertEqual(output.getvalue().count('DISPLAY_PERSON_FAILURE_SHOWN'), 3)
                self.assertEqual(renderer.status['shownFrames'], 0)
                # A failed GUI flush cannot acknowledge presentation.
                renderer.set_person_failure(dict(failure, window=3))
                output.truncate(0)
                output.seek(0)
                wait_key.side_effect = RuntimeError('fixture flush failure')
                with self.assertRaisesRegex(RuntimeError, 'fixture flush failure'):
                    renderer.pump_once()
                self.assertNotIn('DISPLAY_PERSON_FAILURE_SHOWN', output.getvalue())
            finally:
                renderer.close()

    def test_headless_failure_is_observed_once_without_claiming_gui_or_frames(self):
        renderer = ComputeRenderer('headless')
        renderer.start()
        self.addCleanup(renderer.close)
        failure = dict(window=0, reason='source failure', runId='display-run',
            missionId='display-mission', projectionDigest='sha256:' + 'b' * 64)
        with patch('sys.stdout', new_callable=io.StringIO) as output:
            renderer.set_person_failure(failure)
            renderer.pump_once()
            renderer.pump_once()
        records = [json.loads(line) for line in output.getvalue().splitlines()]
        observed = [r for r in records if r['event'] == 'DISPLAY_PERSON_FAILURE_OBSERVED']
        self.assertEqual({r['cameraId'] for r in observed}, set(CAMERAS))
        self.assertEqual(len(observed), 3)
        self.assertTrue(all(r['mode'] == 'headless' for r in observed))
        self.assertNotIn('DISPLAY_PERSON_FAILURE_SHOWN', output.getvalue())
        self.assertEqual(renderer.status['shownFrames'], 0)

    def test_person_mode_ignores_socket_frames_but_retains_stop_control(self):
        errors = []
        socket_path = self.root / 'display.sock'
        def run():
            try:
                self.assertEqual(serve(socket_path, 'headless', 1, self.path,
                                       person_display_context=self.context_path), 0)
            except BaseException as error:
                errors.append(error)
        with patch.object(ComputeRenderer, 'submit', autospec=True) as submit:
            thread = threading.Thread(target=run)
            thread.start()
            try:
                deadline = time.monotonic() + 5
                while not socket_path.exists() and thread.is_alive() and time.monotonic() < deadline:
                    time.sleep(0.01)
                with socket.socket(socket.AF_UNIX) as client:
                    client.connect(str(socket_path))
                    client.sendall(b'{"cameraId":"UAV1","imageJpeg":"fake"}\n{"event":"STOP"}\n')
                thread.join(5)
                self.assertFalse(thread.is_alive())
                self.assertEqual(len(submit.call_args_list), 3)
                self.assertTrue(all(call.args[1].person_display is not None for call in submit.call_args_list))
            finally:
                if thread.is_alive():
                    with socket.socket(socket.AF_UNIX) as client:
                        client.connect(str(socket_path))
                        client.sendall(b'{"event":"STOP"}\n')
                    thread.join(5)
        if errors:
            raise errors[0]


if __name__ == '__main__':
    unittest.main()
