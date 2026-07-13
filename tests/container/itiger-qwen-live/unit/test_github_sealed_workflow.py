from __future__ import annotations

import json
from pathlib import Path
import unittest


REPO = Path(__file__).resolve().parents[4]


class GithubSealedWorkflowTests(unittest.TestCase):
    def test_dockerfile_consumes_only_verified_sealed_archives(self) -> None:
        text = (REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu").read_text()
        self.assertIn("ARG DEPENDENCY_SOURCE_MODE=sealed", text)
        self.assertIn(".spec110-build/archives", text)
        self.assertIn("archiveDigest", text)
        self.assertIn("SOURCE_SEAL_MANIFEST_TAMPERED", text)
        self.assertNotIn("git clone", text)

    def test_dependency_build_order_installs_svs_before_ndnsd(self) -> None:
        text = (REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu").read_text()
        ndn_cxx = text.index("cd /src/dependencies/ndn-cxx")
        ndn_svs = text.index("cd /src/dependencies/ndn-svs")
        ndnsd = text.index("cd /src/dependencies/NDNSD")
        self.assertLess(ndn_cxx, ndn_svs)
        self.assertLess(ndn_svs, ndnsd)

    def test_nfd_build_inputs_include_pcap_and_locked_websocketpp(self) -> None:
        lock = json.loads(
            (REPO / "packaging/ndnsf-di-container/oci/locks/gpu.lock").read_text()
        )
        self.assertIn("libpcap-dev", lock["systemPackages"])
        self.assertEqual(
            lock["sourceRepositories"]["websocketpp"],
            {
                "url": "https://github.com/cawka/websocketpp.git",
                "revision": "ac4e021333675fc80b96eb7be45d218581c897e2",
            },
        )
        dockerfile = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu"
        ).read_text()
        materialize = dockerfile.index("/src/dependencies/NFD/websocketpp")
        nfd_build = dockerfile.index("cd /src/dependencies/NFD")
        self.assertLess(materialize, nfd_build)
        self.assertIn("websocketpp/version.hpp", dockerfile)

    def test_qwen_weights_are_excluded_from_build_context(self) -> None:
        patterns = set((REPO / ".dockerignore").read_text().splitlines())
        for required in (
            "*.safetensors", "*.gguf", "*.ckpt", "pytorch_model*.bin",
            "*.onnx", "*.onnx_data", "RELEASE",
        ):
            self.assertIn(required, patterns)

    def test_workflow_seals_dependencies_and_uploads_evidence_only(self) -> None:
        text = (REPO / ".github/workflows/ndnsf-di-itiger-image.yml").read_text()
        self.assertIn("prepare-sealed-context.py", text)
        self.assertIn("DEPENDENCY_SOURCE_MODE=sealed", text)
        self.assertIn("push: true", text)
        self.assertIn("df -h", text)
        self.assertIn("path: results/spec110-itiger-qwen-live/release-build/", text)
        self.assertNotIn("path: .spec110-build", text)
        self.assertNotIn("runtime.oci", text)
        self.assertNotIn("runtime.sif", text)


if __name__ == "__main__":
    unittest.main()
