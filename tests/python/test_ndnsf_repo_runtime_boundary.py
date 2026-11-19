#!/usr/bin/env python3
"""One deployed Repo runtime and no public unversioned C++ network path."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class RepoRuntimeBoundaryTest(unittest.TestCase):
    def test_waf_installs_repo_native_binding_and_python_facade_together(self) -> None:
        root_wscript = (ROOT / "wscript").read_text()
        wscript = (ROOT / "NDNSF-DistributedRepo/wscript").read_text()
        self.assertIn("bld.recurse('NDNSF-DistributedRepo')", root_wscript)
        self.assertIn("target='../spec201-repo-python/_py_repoclient'", wscript)
        self.assertIn("install_path=python_site + '/py_repoclient'", wscript)
        self.assertIn("bld.path.ant_glob('pythonWrapper/py_repoclient/*.py')", wscript)

    def test_standalone_cpp_network_server_owns_persistent_services(self) -> None:
        wscript = (ROOT / "NDNSF-DistributedRepo/wscript").read_text()
        app = ROOT / "NDNSF-DistributedRepo/apps/DistributedRepoServer.cpp"
        self.assertIn("DistributedRepoServer", wscript)
        self.assertTrue(app.is_file())
        self.assertIn(
            "repo->registerServices(provider, publisherUser",
            app.read_text(),
        )

    def test_cpp_public_node_exposes_local_and_persistent_registration(self) -> None:
        header = (
            ROOT / "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp"
        ).read_text()
        public_section = header.split("private:", 1)[0]
        self.assertIn("registerLocalServices", public_section)
        self.assertIn("registerServices", public_section)
        self.assertNotIn("registerDeploymentServices", header)

    def test_native_service_names_use_only_versioned_families(self) -> None:
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
