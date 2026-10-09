#!/usr/bin/env python3
"""Real legacy SQLite/payload fixture for retired Python entrypoint negatives."""

import hashlib
import os
from pathlib import Path
import sqlite3
import stat
import tempfile
import unittest

from py_repoclient import RepoCompatibilityError


class LegacySourceFixture(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.database = self.root / "legacy.sqlite3"
        self.payload = self.root / "legacy.payload"
        self.payload.write_bytes(b"legacy committed bytes")
        self.connection = sqlite3.connect(self.database)
        self.addCleanup(self.connection.close)
        self.connection.execute("PRAGMA journal_mode=WAL")
        self.connection.execute("PRAGMA user_version=11")
        self.connection.execute(
            "CREATE TABLE artifact_active_catalog (logical_name TEXT, generation INTEGER)")
        self.connection.execute(
            "INSERT INTO artifact_active_catalog VALUES ('/legacy/artifact', 1)")
        self.connection.commit()
        self.schema = self.connection.execute(
            "SELECT name, sql FROM sqlite_master ORDER BY name").fetchall()

    def snapshot(self):
        entries = {}
        for path in self.root.rglob("*"):
            metadata = path.lstat()
            content = (os.readlink(path) if stat.S_ISLNK(metadata.st_mode) else
                       hashlib.sha256(path.read_bytes()).hexdigest()
                       if stat.S_ISREG(metadata.st_mode) else None)
            entries[str(path.relative_to(self.root))] = (
                metadata.st_mode, metadata.st_uid, metadata.st_gid, content)
        return entries

    def assert_rejected(self, operation, replacement, call):
        before = self.snapshot()
        with self.assertRaises(RepoCompatibilityError) as error:
            call()
        self.assertEqual(error.exception.code, "SPEC192_COMPATIBILITY_V1")
        self.assertEqual(error.exception.operation, operation)
        self.assertEqual(error.exception.replacement, replacement)
        self.assertEqual(self.snapshot(), before)
        for name in ("new.sqlite3", "session", "destination"):
            self.assertFalse(os.path.lexists(self.root / name))
        self.assertEqual(self.connection.execute(
            "SELECT name, sql FROM sqlite_master ORDER BY name").fetchall(), self.schema)
