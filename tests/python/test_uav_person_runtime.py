"""Local artifact inspection tests; these do not replace native admission tests."""

import base64
import hashlib
import importlib.metadata
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "NDNSF-UAV-APP/tracking"))
import person_runtime as runtime


class DistributionFixture:
    def __init__(self, root):
        self.root = root
        self.metadata = {"Name": "Example_Person"}
        self.version = "1.0"
        source = root / "example_person/module.py"
        source.parent.mkdir()
        source.write_bytes(b"abc")
        record = root / "example_person-1.0.dist-info/RECORD"
        record.parent.mkdir()
        record.write_bytes(b"fixture-record")
        module = importlib.metadata.PackagePath("example_person/module.py")
        module.hash = importlib.metadata.FileHash(
            "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(b"abc").digest()).rstrip(b"=").decode())
        metadata = importlib.metadata.PackagePath("example_person-1.0.dist-info/RECORD")
        metadata.hash = None
        self.files = [module, metadata]

    def locate_file(self, entry):
        return self.root / entry


class PersonRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.distribution = DistributionFixture(self.root)
        self.versions = {"example-person": "1.0"}

    def inspect(self, distributions=None):
        return runtime._inspect_packages(
            self.versions, [self.distribution] if distributions is None else distributions,
            self.root)

    def test_exact_lock_requires_complete_hashes_and_bounds(self):
        lock = self.root / "requirements.lock"
        good = "Example_Person==1.0 \\\n    --hash=sha256:" + "a" * 64 + "\n"
        lock.write_text(good)
        versions, digest = runtime._load_lock(lock)
        self.assertEqual(versions, self.versions)
        self.assertEqual(digest, "sha256:" + hashlib.sha256(good.encode()).hexdigest())
        for invalid in ("example-person==1.0\n", good + good,
                        good.replace("==1.0", ">=1.0"),
                        good.replace("sha256:", "md5:"), good.rstrip() + " \\\n"):
            with self.subTest(lock=invalid[:45]):
                lock.write_text(invalid)
                with self.assertRaises(ValueError):
                    runtime._load_lock(lock)
        lock.write_text(good)
        with mock.patch.object(runtime, "MAX_LOCK_BYTES", 2):
            with self.assertRaises(ValueError):
                runtime._load_lock(lock)

    def test_current_artifacts_are_deterministic_and_record_metadata_is_bound(self):
        first = self.inspect()
        self.assertEqual(first["example-person"]["fileCount"], 2)
        self.assertEqual(first["example-person"]["bytes"], 17)
        self.distribution.files.reverse()
        self.assertEqual(first, self.inspect())
        (self.root / "example_person-1.0.dist-info/RECORD").write_bytes(b"changed-record")
        self.assertNotEqual(first, self.inspect())
        self.assertNotIn("computationDigest", first)

    def test_exact_installed_set_versions_and_file_hashes_are_required(self):
        for distributions in ([], [self.distribution, self.distribution]):
            with self.assertRaises(ValueError):
                self.inspect(distributions)
        self.distribution.version = "2.0"
        with self.assertRaises(ValueError):
            self.inspect()
        self.distribution.version = "1.0"
        self.distribution.metadata["Name"] = "unexpected"
        with self.assertRaises(ValueError):
            self.inspect()
        self.distribution.metadata["Name"] = "Example_Person"
        (self.root / "example_person/module.py").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "differs from installed RECORD"):
            self.inspect()

    def test_path_and_resource_failures_do_not_return_an_inventory(self):
        for constant, limit in (("MAX_FILE_BYTES", 2), ("MAX_TOTAL_BYTES", 2),
                                ("MAX_RECORDS", 1), ("MAX_DISTRIBUTIONS", 0)):
            with self.subTest(limit=constant), mock.patch.object(runtime, constant, limit):
                with self.assertRaises(ValueError):
                    self.inspect()
        source = self.root / "example_person/module.py"
        source.unlink()
        source.symlink_to("/etc/hosts")
        with self.assertRaisesRegex(ValueError, "escapes isolated environment"):
            self.inspect()
        source.unlink()
        source.mkdir()
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            self.inspect()

    def test_metadata_is_bounded_before_stdlib_eager_readers(self):
        info = self.root / "example_person-1.0.dist-info"
        (info / "METADATA").write_text("Name: example-person\nVersion: 1.0\n")
        distribution = runtime._BoundedDistribution(info, [0])
        with mock.patch.object(runtime, "MAX_METADATA_BYTES", 2):
            with self.assertRaisesRegex(ValueError, "metadata exceeds byte limit"):
                _ = distribution.metadata
        with mock.patch.object(runtime, "MAX_RECORD_METADATA_BYTES", 2):
            with self.assertRaisesRegex(ValueError, "metadata exceeds byte limit"):
                _ = distribution.files
        (info / "RECORD").write_text("a,,\nb,,\n")
        with mock.patch.object(runtime, "MAX_RECORDS", 1):
            with self.assertRaisesRegex(ValueError, "file records exceed limit"):
                _ = distribution.files
        (info / "RECORD").write_text("malformed\n")
        with self.assertRaisesRegex(ValueError, "malformed"):
            _ = runtime._BoundedDistribution(info, [0]).files

    def test_opened_file_path_and_content_are_checked_together(self):
        source = self.root / "example_person/module.py"
        target = self.root / "inside.py"
        target.write_bytes(b"abc")
        expected = self.inspect()
        source.unlink()
        source.symlink_to(target)
        self.assertEqual(self.inspect(), expected)

        # A component-prefix lookalike must not satisfy directory containment.
        with tempfile.TemporaryDirectory(prefix=self.root.name + "-", dir=self.root.parent) as other:
            outside = Path(other) / "outside.py"
            outside.write_bytes(b"abc")
            source.unlink()
            source.symlink_to(outside)
            with self.assertRaisesRegex(ValueError, "escapes isolated environment"):
                self.inspect()

        source.unlink()
        source.write_bytes(b"abc")
        readlink = os.readlink
        moved = self.root / "held.py"

        def replace_after_open(link):
            source.rename(moved)
            source.write_bytes(b"changed-after-open")
            return readlink(link)

        with mock.patch.object(runtime.os, "readlink", side_effect=replace_after_open):
            self.assertEqual(runtime._hash_file(source, [0], prefix=self.root),
                             (hashlib.sha256(b"abc").hexdigest(), 3))
        with self.assertRaisesRegex(ValueError, "differs from installed RECORD"):
            self.inspect()

    def test_descriptor_is_released_on_all_admission_failures(self):
        source = self.root / "example_person/module.py"
        close = os.close
        cases = [("MAX_FILE_BYTES", 2), ("MAX_TOTAL_BYTES", 2)]
        for constant, limit in cases:
            with self.subTest(limit=constant), mock.patch.object(runtime, constant, limit), \
                    mock.patch.object(runtime.os, "close", wraps=close) as released:
                with self.assertRaisesRegex(ValueError, "byte limit"):
                    runtime._hash_file(source, [0], prefix=self.root)
                descriptor = released.call_args[0][0]
                self.assertEqual(released.call_count, 1)
                with self.assertRaises(OSError):
                    os.fstat(descriptor)

        with mock.patch.object(runtime.os, "readlink", side_effect=OSError("proc unavailable")), \
                mock.patch.object(runtime.os, "close", wraps=close) as released:
            with self.assertRaisesRegex(OSError, "proc unavailable"):
                runtime._hash_file(source, [0], prefix=self.root)
            self.assertEqual(released.call_count, 1)
            with self.assertRaises(OSError):
                os.fstat(released.call_args[0][0])

        fifo = self.root / "pipe"
        os.mkfifo(fifo)
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            runtime._hash_file(fifo, [0], prefix=self.root)
        missing = self.root / "missing"
        with self.assertRaises(FileNotFoundError):
            runtime._hash_file(missing, [0], prefix=self.root)


if __name__ == "__main__":
    unittest.main()
