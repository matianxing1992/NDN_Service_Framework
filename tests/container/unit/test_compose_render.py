from __future__ import annotations

import unittest
import yaml

from contract._support import FIXTURES, REPO, load_impl

profile_impl = load_impl("profile")
release_impl = load_impl("release")
compose_impl = load_impl("adapters/docker_compose")


class ComposeRenderTest(unittest.TestCase):
    def setUp(self) -> None:
        self.profile = profile_impl.load_profile(FIXTURES / "profiles" / "cloud-cpu-valid.yaml")
        self.release = release_impl.load_release_manifest(FIXTURES / "releases" / "release-valid.json")
        self.compose_path = REPO / "packaging" / "ndnsf-di-container" / "adapters" / "docker-compose" / "compose.yaml"

    def test_environment_uses_digest_image_and_one_host_nfd(self) -> None:
        environment = compose_impl.render_environment(self.profile, self.release)
        self.assertIn("@sha256:", environment["NDNSF_OCI_IMAGE"])
        self.assertEqual(environment["COMPOSE_PROJECT_NAME"], "ndnsf-cloud-a")
        document = yaml.safe_load(self.compose_path.read_text(encoding="utf-8"))
        self.assertEqual([name for name in document["services"] if name == "nfd"], ["nfd"])
        self.assertIn("nfd", document["services"]["provider"]["depends_on"])

    def test_only_nfd_publishes_network_ports(self) -> None:
        document = yaml.safe_load(self.compose_path.read_text(encoding="utf-8"))
        self.assertIn("ports", document["services"]["nfd"])
        for name, service in document["services"].items():
            if name != "nfd":
                self.assertNotIn("ports", service)


if __name__ == "__main__":
    unittest.main()
