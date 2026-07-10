#!/usr/bin/env python3
"""Bounded-concurrency contracts for the DistributedRepo repair sidecar."""

from __future__ import annotations

import importlib.util
import io
from pathlib import Path
import threading
import time
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = (
    ROOT / "examples/python/NDNSF-DistributedRepo/generic_object_store/catalog_sync.py"
)
SPEC = importlib.util.spec_from_file_location("ndnsf_repo_catalog_sync", MODULE_PATH)
catalog_sync = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(catalog_sync)


class FakeRepairRepo:
    def __init__(self, jobs: list[dict]) -> None:
        self._lock = threading.Lock()
        self.active = 0
        self.max_active = 0
        self.jobs = list(jobs)
        self.completions: list[str] = []
        self.control_threads: set[int] = set()

    def repair_scan(self, repo_node: str) -> dict:
        del repo_node
        self.control_threads.add(threading.get_ident())
        return {
            "createdCount": 0,
            "jobCount": len(self.jobs),
            "claimableCount": len(self.jobs),
        }

    def repair_claim(
        self, repo_node: str, *, lease_owner: str, lease_ms: int,
    ) -> dict:
        del repo_node, lease_owner, lease_ms
        self.control_threads.add(threading.get_ident())
        return {"job": self.jobs.pop(0) if self.jobs else None}

    def repair_complete(
        self, repo_node: str, *, repair_id: str, result: dict,
    ) -> dict:
        del repo_node, result
        self.control_threads.add(threading.get_ident())
        self.completions.append(repair_id)
        return {"state": "COMPLETED"}

    def repair_fail(
        self, repo_node: str, *, repair_id: str, error: str,
    ) -> dict:
        raise AssertionError(
            f"unexpected repair failure repo={repo_node} id={repair_id}: {error}")

    def catalog_repair(self, repo_node: str, action: dict) -> dict:
        with self._lock:
            self.active += 1
            self.max_active = max(self.max_active, self.active)
        try:
            time.sleep(0.05)
            return {
                "objectName": action["objectName"],
                "sourceRepo": action["sourceRepo"],
                "targetRepo": repo_node,
            }
        finally:
            with self._lock:
                self.active -= 1


class RepairSidecarConcurrencyTest(unittest.TestCase):
    def test_catalog_merge_selects_inline_pull_and_fallback(self) -> None:
        class FakeUser:
            user = "/repo/A"

        class FakeProducer:
            versioned_name = "/repo/A/catalog/v=1"
            segment_count = 3

            def __init__(self) -> None:
                self.stopped = False

            def start(self):
                return self

            def stop(self) -> None:
                self.stopped = True

        calls = []

        def request_ok(user, repo_node, payload, **kwargs):
            del user, kwargs
            request = catalog_sync.json.loads(payload.decode())
            calls.append((repo_node, request))
            return {"status": "merged"}

        with patch.object(catalog_sync, "request_repo", side_effect=request_ok):
            inline = catalog_sync.merge_catalog_delta(
                FakeUser(), "/repo/A", [], {"repoNode": "/repo/B"})
        self.assertEqual(inline["mode"], "inline")
        self.assertEqual(calls[-1][1]["operation"], "CATALOG_MERGE")

        producer = FakeProducer()
        calls.clear()
        with patch.object(
                catalog_sync, "request_repo", side_effect=request_ok), patch.object(
                catalog_sync, "SegmentedObjectProducer",
                return_value=producer), patch.object(catalog_sync.time, "sleep"):
            pulled = catalog_sync.merge_catalog_delta(
                FakeUser(), "/repo/A", [{"objectName": "/large"}],
                {"repoNode": "/repo/B"}, max_request_bytes=1)
        self.assertEqual(pulled["mode"], "pull")
        self.assertEqual(pulled["segments"], 3)
        self.assertEqual(calls[0][1]["operation"], "CATALOG_MERGE_PULL")
        self.assertTrue(producer.stopped)

        fallback_producer = FakeProducer()
        calls.clear()

        def request_fallback(user, repo_node, payload, **kwargs):
            del user, kwargs
            request = catalog_sync.json.loads(payload.decode())
            calls.append((repo_node, request))
            if request["operation"] == "CATALOG_MERGE_PULL":
                raise RuntimeError("unsupported")
            return {"status": "merged"}

        with patch.object(
                catalog_sync, "request_repo",
                side_effect=request_fallback), patch.object(
                catalog_sync, "SegmentedObjectProducer",
                return_value=fallback_producer), patch.object(
                catalog_sync.time, "sleep"):
            fallback = catalog_sync.merge_catalog_delta(
                FakeUser(), "/repo/A", [{"objectName": "/large"}],
                {"repoNode": "/repo/B"}, max_request_bytes=1)
        self.assertEqual(fallback["mode"], "fallback")
        self.assertEqual(
            [request["operation"] for _, request in calls],
            ["CATALOG_MERGE_PULL", "CATALOG_MERGE"],
        )
        self.assertTrue(fallback_producer.stopped)

    def test_transfer_workers_are_bounded_and_completion_is_serial(self) -> None:
        jobs = [
            {
                "repairId": f"repair-{index}",
                "objectName": f"/publisher/object-{index}",
                "sourceRepo": "/repo/B",
                "targetRepo": "/repo/A",
                "action": {
                    "objectName": f"/publisher/object-{index}",
                    "sourceRepo": "/repo/B",
                    "targetRepo": "/repo/A",
                },
            }
            for index in range(4)
        ]
        def fake_request_repo(user, repo_node, payload, **kwargs):
            del user, repo_node, payload, kwargs
            self.fail("auto repair control must use the Targeted repair client")

        repair_repo = FakeRepairRepo(jobs)
        started = time.monotonic()
        output = io.StringIO()
        with patch.object(
                catalog_sync, "request_repo",
                side_effect=fake_request_repo), redirect_stdout(output):
            metrics = catalog_sync.process_durable_repair_jobs(
                object(), "/repo/A", auto_repair=True,
                repair_repo=repair_repo, max_jobs=4, repair_workers=3,
                cycle_phase="post-merge")
        elapsed = time.monotonic() - started

        self.assertEqual(repair_repo.max_active, 3)
        self.assertEqual(sorted(repair_repo.completions),
                         [job["repairId"] for job in jobs])
        self.assertEqual(len(repair_repo.control_threads), 1)
        self.assertLess(elapsed, 0.17)
        self.assertEqual(metrics["phase"], "post-merge")
        self.assertEqual(metrics["claimable"], 4)
        self.assertEqual(metrics["claimed"], 4)
        self.assertEqual(metrics["completed"], 4)
        self.assertIn("catalog_sync repair_cycle", output.getvalue())
        self.assertIn("workers=3", output.getvalue())


if __name__ == "__main__":
    unittest.main()
