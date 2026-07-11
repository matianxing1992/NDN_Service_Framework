#!/usr/bin/env python3
"""One deployed Repo runtime and no public unversioned C++ network path."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class RepoRuntimeBoundaryTest(unittest.TestCase):
    def test_standalone_cpp_network_app_is_not_built_or_present(self) -> None:
        wscript = (ROOT / "NDNSF-DistributedRepo/wscript").read_text()
        self.assertNotIn("DistributedRepoNodeApp", wscript)
        self.assertFalse(
            (ROOT / "NDNSF-DistributedRepo/apps/DistributedRepoNodeApp.cpp").exists()
        )

    def test_cpp_public_header_exposes_only_local_registration(self) -> None:
        header = (
            ROOT / "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp"
        ).read_text()
        public_section = header.split("private:", 1)[0]
        self.assertIn("registerLocalServices", public_section)
        self.assertNotIn("registerServices", public_section)
        self.assertNotIn("registerDeploymentServices", header)

    def test_python_deployed_adapter_has_only_versioned_service_families(self) -> None:
        from py_repoclient.service_names import repo_versioned_services

        services = repo_versioned_services()
        self.assertTrue(services)
        self.assertTrue(all(
            "/Object/v1/" in service or "/Internal/v1/" in service
            for service in services
        ))
        self.assertNotIn("/NDNSF/DistributedRepo", services)


if __name__ == "__main__":
    unittest.main()
