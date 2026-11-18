#!/usr/bin/env python3
"""Spec 240: the unified RepoClient facade is usable from Python."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

# Prefer the installed package layout; fall back to the build tree, where the
# extension is a top-level module. Loading both copies in one process registers the
# same pybind types twice, so this module deliberately picks exactly one.
try:
    from py_repoclient import _py_repoclient as native
except ImportError:
    import _py_repoclient as native

PAYLOAD = b"spec240 unified client facade v1"
OWNER = "/example/spec240/owner"
OBJECT = "/example/spec240/owner/object"


def reference() -> dict:
    return {
        "logical_name": OBJECT,
        "content_digest": hashlib.sha256(PAYLOAD).hexdigest(),
        "size_bytes": len(PAYLOAD),
        "format_version": "raw-file-v1",
        "publisher_identity": OWNER,
        "policy_epoch": "1",
    }


class UnifiedRepoClientTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = tempfile.mkdtemp(prefix="spec240-py-")
        self.addCleanup(shutil.rmtree, self.root, True)
        self.source = os.path.join(self.root, "source.bin")
        with open(self.source, "wb") as handle:
            handle.write(PAYLOAD)
        self.repo_root = os.path.join(self.root, "repo")

    def test_publish_fetch_and_inspect_round_trip(self) -> None:
        published = native.unified_publish_file(
            self.repo_root, OWNER, self.source, reference(), 30000)
        self.assertEqual(published["state"], "COMMITTED")

        destination = os.path.join(self.root, "fetched.bin")
        fetched = native.unified_fetch_file(
            self.repo_root, OWNER, reference(), destination, 30000)
        self.assertEqual(fetched["bytes_written"], len(PAYLOAD))
        self.assertTrue(fetched["digest_verified"])
        with open(destination, "rb") as handle:
            self.assertEqual(handle.read(), PAYLOAD)

        inspected = native.unified_inspect_object(
            self.repo_root, OWNER, reference(), 30000)
        self.assertEqual(inspected["availability"], "AVAILABLE")
        self.assertEqual(inspected["stored_bytes"], len(PAYLOAD))

    def test_a_second_process_reads_the_published_object(self) -> None:
        published = native.unified_publish_file(
            self.repo_root, OWNER, self.source, reference(), 30000)
        self.assertEqual(published["state"], "COMMITTED")

        destination = os.path.join(self.root, "second-process.bin")
        script = (
            "import json, sys\n"
            "try:\n"
            "    from py_repoclient import _py_repoclient as native\n"
            "except ImportError:\n"
            "    import _py_repoclient as native\n"
            "root, owner, reference, destination = json.loads(sys.argv[1])\n"
            "result = native.unified_fetch_file(root, owner, reference, destination, 30000)\n"
            "print(json.dumps(result))\n"
        )
        # The child is a fresh process: it cannot see this process' in-memory
        # catalog or admission state, so a successful read is a restart oracle.
        # Give the child the same Python closure as this process. A bare child falls back to whatever
        # `_py_repoclient` happens to be installed globally, which can be older than the sources under test
        # and would make this restart oracle measure the installed copy instead of the built one.
        import py_repoclient as _repo_client
        wrapper_root = Path(_repo_client.__file__).resolve().parent.parent
        child_env = dict(os.environ)
        child_env["PYTHONPATH"] = os.pathsep.join(
            [str(wrapper_root)] + ([os.environ["PYTHONPATH"]] if os.environ.get("PYTHONPATH") else []))
        completed = subprocess.run(
            [sys.executable, "-c", script,
             json.dumps([self.repo_root, OWNER, reference(), destination])],
            capture_output=True, text=True, timeout=120, check=False, env=child_env)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertGreater(int(json.loads(completed.stdout)["bytes_written"]), 0)
        with open(destination, "rb") as handle:
            self.assertEqual(handle.read(), PAYLOAD)

    def test_discovery_requires_a_real_service_user_owner(self) -> None:
        # The persistent surface must not invent an identity; without the capsule
        # it has to fail before any request is issued.
        with self.assertRaises(ValueError):
            native.unified_discover(None, "/SVC/repo", "/provider", 1000)

    def test_publish_rejects_an_empty_reference_before_side_effects(self) -> None:
        # pybind maps the native invalid_argument rejection to ValueError.
        with self.assertRaises(ValueError):
            native.unified_publish_file(
                self.repo_root, OWNER, self.source, {}, 30000)


if __name__ == "__main__":
    unittest.main()
