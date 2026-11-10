"""Literal native/adapter wire parity, rejection and immutable configuration tests."""

import importlib.util
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "NDNSF-UAV-APP/tracking/person_profile.py"
SPEC = importlib.util.spec_from_file_location("uav_person_profile", MODULE_PATH)
profile_module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = profile_module
SPEC.loader.exec_module(profile_module)
FIXTURE = (ROOT / "tests/fixtures/uav-person-profile-golden.hpp").read_text()
GOLDEN = re.search(r'R"profile\((.*)\)profile"', FIXTURE).group(1).encode("ascii")
DIGEST = re.search(r'PERSON_PROFILE_DIGEST = "([^"]+)"', FIXTURE).group(1)
DETECTOR_FIXTURE = (ROOT / "tests/fixtures/uav-person-detector-profile-golden.hpp").read_text()
DETECTOR_GOLDEN = re.search(r'R"detector\((.*)\)detector"', DETECTOR_FIXTURE).group(1).encode("ascii")
DETECTOR_DIGEST = re.search(r'PERSON_DETECTOR_PROFILE_DIGEST = "([^"]+)"', DETECTOR_FIXTURE).group(1)


class PersonProfileTests(unittest.TestCase):
    def test_bounded_slow_cpu_demo_deadline(self):
        wire = GOLDEN.replace(b'"runDeadlineMs":900000', b'"runDeadlineMs":1200000')
        parsed = profile_module.parse_person_profile(wire)
        self.assertEqual(parsed.limits["runDeadlineMs"], 1_200_000)
        self.assertEqual(parsed.canonical_bytes, wire)
        self.assertNotEqual(parsed.digest, DIGEST)
        self.assertEqual(profile_module.parse_person_profile(GOLDEN).digest, DIGEST)
        for invalid in (1_200_001, True):
            value = json.loads(wire)
            value["limits"]["runDeadlineMs"] = invalid
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                profile_module.parse_person_profile(json.dumps(value).encode())

    def test_reid_preprocess_identity_and_closed_mode(self):
        gap = profile_module.parse_person_profile(GOLDEN)
        self.assertEqual(gap.reid_preprocess_id,
                         "osnet-rgb-256x128-imagenet-l2-v1")

        value = json.loads(GOLDEN)
        value["preprocessId"] = "osnet-rgb-256x128-imagenet-gem-p3-l2-v1"
        gem_wire = json.dumps(value).encode("ascii")
        gem = profile_module.parse_person_profile(gem_wire)
        self.assertEqual(gem.reid_preprocess_id, value["preprocessId"])
        self.assertNotEqual(gem.digest, gap.digest)
        self.assertEqual(gem.canonical_bytes,
                         json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=True).encode("ascii"))

        for preprocess_id in (None, True, 128, "osnet-rgb-256x128-imagenet-unknown-v1"):
            invalid = dict(value, preprocessId=preprocess_id)
            with self.subTest(preprocess_id=preprocess_id), self.assertRaises(ValueError):
                profile_module.parse_person_profile(json.dumps(invalid).encode())

    def test_nms50_canonical_identity(self):
        wire = DETECTOR_GOLDEN.replace(b"raw-box-v1", b"raw-box-nms50-v1")
        profile = profile_module.parse_person_profile(wire)
        self.assertEqual(profile.canonical_bytes, wire)
        self.assertEqual(profile.digest,
                         "sha256:5b7013bc592de5ba46eb7e0c8261fd68f98892581f25c86e741e814cf362307e")
        self.assertNotEqual(profile.digest, DETECTOR_DIGEST)
        self.assertEqual(profile.detector_preprocess_id, "yolo-letterbox1280-raw-box-nms50-v1")
        self.assertEqual(profile_module.parse_person_profile(
            json.dumps(dict(reversed(list(json.loads(wire).items())))).encode()).digest,
                         profile.digest)

    def test_explicit_detector_literal_and_legacy_identity(self):
        parsed = profile_module.parse_person_profile(DETECTOR_GOLDEN)
        self.assertEqual(parsed.canonical_bytes, DETECTOR_GOLDEN)
        self.assertEqual(parsed.digest, DETECTOR_DIGEST)
        self.assertNotEqual(parsed.digest, DIGEST)
        self.assertEqual(parsed.detector_preprocess_id, "yolo-letterbox1280-raw-box-v1")
        self.assertEqual(profile_module.parse_person_profile(GOLDEN).detector_preprocess_id,
                         "yolo-default640-stock-box-v1")
        reordered = dict(reversed(list(json.loads(DETECTOR_GOLDEN).items())))
        self.assertEqual(profile_module.parse_person_profile(
            json.dumps(reordered).encode()).canonical_bytes, DETECTOR_GOLDEN)
        escaped = DETECTOR_GOLDEN.replace(b"raw-box", b"raw-\\u0062ox")
        self.assertEqual(profile_module.parse_person_profile(escaped).digest, DETECTOR_DIGEST)
        with self.assertRaises(AttributeError):
            parsed.detector_preprocess_id = "other"

    def test_explicit_detector_requires_exact_version_field_and_mode(self):
        value = json.loads(DETECTOR_GOLDEN)
        invalid = []
        missing = dict(value)
        del missing["detectorPreprocessId"]
        invalid.append(missing)
        for key, replacements in {
                "schema": ["uav-person-tracking-profile-v1", "uav-person-tracking-profile-v3", None],
                "detectorPreprocessId": [None, True, 1280, [], "yolo-default640-stock-box-v1",
                                         "yolo-letterbox640-raw-box-v1",
                                         "yolo-letterbox1280-raw-box-nms51-v1"]}.items():
            invalid.extend({**value, key: replacement} for replacement in replacements)
        for fields in invalid:
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                profile_module.parse_person_profile(json.dumps(fields).encode())
        duplicate = DETECTOR_GOLDEN.replace(b'"detectorPreprocessId":',
                                            b'"detectorPreprocessId":"x","detectorPreprocessId":')
        with self.assertRaises(ValueError):
            profile_module.parse_person_profile(duplicate)

    def test_literal_native_golden_and_escaped_equivalence(self):
        parsed = profile_module.parse_person_profile(b" \n" + GOLDEN + b"\t")
        self.assertEqual(parsed.canonical_bytes, GOLDEN)
        self.assertEqual(parsed.digest, DIGEST)
        self.assertFalse(parsed.overlapping_cameras["UAV1", "UAV3"])
        escaped = GOLDEN.replace(b"UAV1", b"UAV\\u0031", 1)
        self.assertEqual(profile_module.parse_person_profile(escaped).digest, DIGEST)
        value = json.loads(GOLDEN)
        reordered = dict(reversed(list(value.items())))
        reordered["limits"] = dict(reversed(list(value["limits"].items())))
        self.assertEqual(profile_module.parse_person_profile(
            json.dumps(reordered).encode()).canonical_bytes, GOLDEN)
        with self.assertRaises(TypeError):
            parsed.limits["sampleHz"] = 3

    def test_invalid_wire_types_bounds_and_duplicates(self):
        for payload in (b"", b"[]", b"null", b" " * 16385, GOLDEN + b"{}",
                        GOLDEN.replace(b'"sampleHz":2', b'"sampleHz":2,"sampleHz":2'),
                        GOLDEN.replace(b'"minTransitionUs":0', b'"minTransitionUs":-0'),
                        GOLDEN.replace(b'"maxLocalKeys":1024',
                                       b'"maxLocalKeys":18446744073709551616')):
            with self.subTest(payload=payload[:50]), self.assertRaises(ValueError):
                profile_module.parse_person_profile(payload)
        for key, invalid_values in {
                "sampleHz": [True, "2", 2.0, None, [], -1, 0, 3],
                "maxLocalKeys": [0, 1025], "galleryTtlUs": [1],
                "runDeadlineMs": [1], "maxBackendTracksPerCamera": [1]}.items():
            for number in invalid_values:
                value = json.loads(GOLDEN)
                value["limits"][key] = number
                with self.subTest(key=key, value=number), self.assertRaises(ValueError):
                    profile_module.parse_person_profile(json.dumps(value).encode())
        for key, value in (("requestedDevice", "cuda:1"), ("featureDimension", 512.0),
                           ("architecture", None), ("extra", 1)):
            fields = json.loads(GOLDEN)
            fields[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                profile_module.parse_person_profile(json.dumps(fields).encode())

    def test_changed_computation_inputs_and_bounded_file_loading(self):
        value = json.loads(GOLDEN)
        for key, replacement in (("requestedDevice", "cpu"),
                                 ("detectorDigest", "sha256:" + "c" * 64),
                                 ("reidDigest", "sha256:" + "d" * 64)):
            fields = dict(value)
            fields[key] = replacement
            self.assertNotEqual(profile_module.parse_person_profile(
                json.dumps(fields).encode()).digest, DIGEST)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            path.write_bytes(GOLDEN)
            self.assertEqual(profile_module.load_person_profile(path).digest, DIGEST)
            path.write_bytes(b" " * 16385)
            with self.assertRaises(ValueError):
                profile_module.load_person_profile(path)


    def test_model_preparation_and_mismatch_refusal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            template = root / "template.json"
            detector, reid = root / "detector.pt", root / "reid.pth"
            template.write_bytes(GOLDEN)
            detector.write_bytes(b"test weight identity only, not an inference fixture")
            reid.write_bytes(b"test ReID weight identity only")
            prepared = profile_module.prepare_person_profile(template, detector, reid)
            profile_module.verify_person_model_files(prepared, detector, reid)
            template.write_bytes(DETECTOR_GOLDEN)
            explicit = profile_module.prepare_person_profile(template, detector, reid)
            profile_module.verify_person_model_files(explicit, detector, reid)
            self.assertEqual(explicit.detector_preprocess_id, "yolo-letterbox1280-raw-box-v1")
            self.assertEqual({key: value for key, value in json.loads(explicit.canonical_bytes).items()
                              if key not in ("detectorDigest", "reidDigest")},
                             {key: value for key, value in json.loads(DETECTOR_GOLDEN).items()
                              if key not in ("detectorDigest", "reidDigest")})
            reid.write_bytes(b"changed model bytes")
            with self.assertRaises(ValueError):
                profile_module.verify_person_model_files(prepared, detector, reid)
            with self.assertRaises(ValueError):
                profile_module.prepare_person_profile(template, root / "missing", reid)
            detector.write_bytes(b"")
            with self.assertRaises(ValueError):
                profile_module.prepare_person_profile(template, detector, reid)


if __name__ == "__main__":
    unittest.main()
