from __future__ import annotations

from datetime import datetime, timezone
import unittest

from _support import load_tool


cluster = load_tool("spec110_cluster")


OBSERVED = "2026-07-13T18:00:00Z"


def live_values() -> dict[str, object]:
    return {
        "account": "research",
        "qos": "normal",
        "partition": "bigTiger",
        "gres": {
            "requested": "gpu:rtx5000:3",
            "nodes": [{"name": "itiger07", "label": "gpu:rtx5000", "count": 8}],
        },
        "nodes": ["itiger07"],
        "apptainerVersions": {"login": "1.3.3", "compute": "1.3.3"},
        "driverCuda": {"driver": "550.54.15", "cuda": "12.4", "observedOn": "itiger07"},
        "storage": {
            "projectRoot": "/project/tester/ndnsf-di",
            "quota": {"command": "quota", "status": "AVAILABLE", "availableBytes": 1000000000},
        },
        "addresses": [
            {"node": "itiger07", "address": "10.0.0.7", "scope": "allocation"}
        ],
    }


class ClusterSnapshotTests(unittest.TestCase):
    def make(self, stale_fact: str | None = None):
        ttls = {name: 300 for name in cluster.MUTABLE_FACTS}
        if stale_fact:
            ttls[stale_fact] = 1
        return cluster.build_cluster_snapshot(
            live_values(), observed_at=OBSERVED, fact_ttl_seconds=ttls,
        )

    def test_fresh_snapshot_validates_and_is_digest_bound(self):
        value = self.make()
        result = cluster.validate_cluster_snapshot(
            value, now=datetime(2026, 7, 13, 18, 4, 0, tzinfo=timezone.utc)
        )
        self.assertEqual(result["snapshotId"], value["snapshotId"])

    def test_stale_mutable_facts_fail_independently(self):
        now = datetime(2026, 7, 13, 18, 0, 2, tzinfo=timezone.utc)
        for fact in ("partition", "gres", "quota", "versions", "addresses"):
            with self.subTest(fact=fact):
                with self.assertRaisesRegex(cluster.ClusterSnapshotError, f"CLUSTER_FACT_STALE:{fact}"):
                    cluster.validate_cluster_snapshot(self.make(fact), now=now)

    def test_non_allocation_address_is_rejected(self):
        values = live_values()
        values["addresses"] = [{"node": "itiger07", "address": "127.0.0.1", "scope": "login"}]
        with self.assertRaisesRegex(cluster.ClusterSnapshotError, "CLUSTER_ADDRESS_SCOPE_INVALID"):
            cluster.build_cluster_snapshot(values, observed_at=OBSERVED)

    def test_shared_df_is_not_accepted_as_quota(self):
        values = live_values()
        values["storage"]["quota"] = {"command": "df", "status": "AVAILABLE", "availableBytes": 1}
        with self.assertRaisesRegex(cluster.ClusterSnapshotError, "CLUSTER_QUOTA_SIGNAL_INVALID"):
            cluster.build_cluster_snapshot(values, observed_at=OBSERVED)

    def test_mutation_breaks_snapshot_digest(self):
        value = self.make()
        value["partition"] = "other"
        with self.assertRaisesRegex(cluster.ClusterSnapshotError, "CLUSTER_SNAPSHOT_DIGEST_MISMATCH"):
            cluster.validate_cluster_snapshot(
                value, now=datetime(2026, 7, 13, 18, 0, 1, tzinfo=timezone.utc)
            )


if __name__ == "__main__":
    unittest.main()
