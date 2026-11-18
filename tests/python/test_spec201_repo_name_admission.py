"""Actual source-linked C++ Name inspection and precommit Repo admission."""
import os
import sqlite3
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


class RepoNameAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="spec201-repo-name-")
        cls.addClassCleanup(cls.directory.cleanup)
        previous = {key: os.environ.get(key) for key in ("NDN_CLIENT_PIB", "NDN_CLIENT_TPM")}
        def restore():
            for key, value in previous.items():
                if value is None: os.environ.pop(key, None)
                else: os.environ[key] = value
        cls.addClassCleanup(restore)
        os.environ["NDN_CLIENT_PIB"] = "pib-sqlite3:" + cls.directory.name + "/pib"
        os.environ["NDN_CLIENT_TPM"] = "tpm-file:" + cls.directory.name + "/tpm"
        setup_env = dict(os.environ)
        setup_env.pop("LD_PRELOAD", None)
        subprocess.run(["ndnsec", "key-gen", "-t", "r", "/spec201/repo-name-owner"],
                       env=setup_env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                       check=True, timeout=15)
        from ndnsf import _ndnsf, inspect_data_name

        requested = os.environ.get("SPEC201_PYTHON_BINDING")
        if requested and Path(requested).resolve(strict=True) != Path(_ndnsf.__file__).resolve():
            raise RuntimeError("SPEC201_PYTHON_BINDING does not match the imported binding")
        cls.inspect = staticmethod(inspect_data_name)

    def test_uri_profiles_have_equal_wire_and_typed_segment_parent(self):
        alternate = self.inspect("/actual/producer/v=1/seg=1")
        canonical = self.inspect("/8=actual/8=producer/54=%01/50=%01")
        self.assertEqual(alternate, canonical)
        self.assertEqual(alternate["segment"], 1)
        self.assertEqual(alternate["segment_prefix"], "/8=actual/8=producer/54=%01")
        generic = self.inspect("/actual/producer/1")
        self.assertIsNone(generic["segment"])
        self.assertEqual(generic["segment_prefix"], generic["canonical_name"])
        with self.assertRaises(ValueError):
            self.inspect("/")



if __name__ == "__main__":
    unittest.main()
