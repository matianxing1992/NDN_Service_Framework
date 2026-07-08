#!/usr/bin/env python3
"""Deployment lifecycle dictionaries carry core ServiceOperationStatus."""

from __future__ import annotations

import unittest

from ndnsf import ServiceOperationState, ServiceOperationStatus
from ndnsf.service import (
    _deployment_operation_status,
    _deployment_sort_key,
    _with_deployment_operation_status,
)


class DeploymentOperationStatusTest(unittest.TestCase):
    def test_active_deployment_gets_done_operation_status(self) -> None:
        deployment = _with_deployment_operation_status({
            "deploymentId": "dep-active",
            "planId": "plan-1",
            "serviceName": "/Inference/NativeTracer",
            "status": "ACTIVE",
            "refCount": 2,
        })
        status = ServiceOperationStatus.from_dict(deployment["operationStatus"])

        self.assertEqual(deployment["status"], "ACTIVE")
        self.assertEqual(status.state, ServiceOperationState.DONE)
        self.assertEqual(status.progress, 1.0)
        self.assertEqual(status.metadata["deploymentStatus"], "ACTIVE")
        self.assertEqual(status.metadata["refCount"], 2)

    def test_provisioning_deployment_maps_to_running(self) -> None:
        status = ServiceOperationStatus.from_dict(_deployment_operation_status({
            "deploymentId": "dep-provisioning",
            "serviceName": "/Inference/NativeTracer",
            "status": "PROVISIONING",
        }))

        self.assertEqual(status.state, ServiceOperationState.RUNNING)
        self.assertEqual(status.progress, 0.5)

    def test_evicted_and_rejected_are_terminal_core_states(self) -> None:
        evicted = ServiceOperationStatus.from_dict(_deployment_operation_status({
            "deploymentId": "dep-evicted",
            "status": "EVICTED",
        }, operation="EVICT_DEPLOYMENT"))
        rejected = ServiceOperationStatus.from_dict(_deployment_operation_status({
            "deploymentId": "dep-rejected",
            "status": "REJECTED",
            "reason": "DEPLOYMENT_IN_USE",
        }, operation="EVICT_DEPLOYMENT"))

        self.assertEqual(evicted.state, ServiceOperationState.CANCELED)
        self.assertEqual(rejected.state, ServiceOperationState.FAILED)
        self.assertEqual(rejected.reason_code, "REJECTED")
        self.assertEqual(rejected.message, "DEPLOYMENT_IN_USE")

    def test_sort_prefers_operation_status_metadata_when_present(self) -> None:
        deployments = [
            {"deploymentId": "cold", "status": "ACTIVE", "operationStatus": {
                "operationId": "cold",
                "operation": "DEPLOYMENT",
                "state": "DONE",
                "metadata": {"deploymentStatus": "DISK_RESIDENT"},
            }},
            {"deploymentId": "hot", "status": "PROVISIONING", "operationStatus": {
                "operationId": "hot",
                "operation": "DEPLOYMENT",
                "state": "DONE",
                "metadata": {"deploymentStatus": "ACTIVE"},
            }},
            {"deploymentId": "legacy", "status": "IDLE"},
        ]

        deployments.sort(key=_deployment_sort_key)

        self.assertEqual([item["deploymentId"] for item in deployments],
                         ["hot", "legacy", "cold"])

    def test_legacy_only_sorting_remains_backward_compatible(self) -> None:
        deployments = [
            {"deploymentId": "provisioning", "status": "PROVISIONING"},
            {"deploymentId": "active", "status": "ACTIVE"},
            {"deploymentId": "evicted", "status": "EVICTED"},
        ]

        deployments.sort(key=_deployment_sort_key)

        self.assertEqual([item["deploymentId"] for item in deployments],
                         ["active", "provisioning", "evicted"])


if __name__ == "__main__":
    unittest.main()

