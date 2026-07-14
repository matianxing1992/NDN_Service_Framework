from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
PREFLIGHT = (
    REPO / "packaging/ndnsf-di-container/oci/scripts/preflight-gpu-build.py"
)


class GithubSealedWorkflowTests(unittest.TestCase):
    def test_transient_local_seal_is_not_a_git_artifact(self) -> None:
        patterns = set((REPO / ".gitignore").read_text().splitlines())
        self.assertIn(".spec110-build/", patterns)
        self.assertIn(".spec110-build-context/", patterns)

    def test_local_foundation_consumes_only_verified_sealed_archives(self) -> None:
        foundation = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.foundation"
        ).read_text()
        gpu = (REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu").read_text()
        self.assertIn("ARG DEPENDENCY_SOURCE_MODE=sealed", foundation)
        self.assertIn(".spec110-build/archives", foundation)
        self.assertIn("archiveDigest", foundation)
        self.assertIn("SOURCE_SEAL_MANIFEST_TAMPERED", foundation)
        self.assertNotIn("git clone", foundation)
        self.assertIn("ARG FOUNDATION_IMAGE", gpu)
        self.assertIn("FROM ${FOUNDATION_IMAGE} AS local-foundation", gpu)
        self.assertNotIn(".spec110-build", gpu)
        self.assertNotIn("/src/dependencies/NFD", gpu)

    def test_dependency_build_order_installs_svs_before_ndnsd(self) -> None:
        text = (REPO / "packaging/ndnsf-di-container/oci/Dockerfile.foundation").read_text()
        ndn_cxx = text.index("cd /src/dependencies/ndn-cxx")
        ndn_svs = text.index("cd /src/dependencies/ndn-svs")
        ndnsd = text.index("cd /src/dependencies/NDNSD")
        self.assertLess(ndn_cxx, ndn_svs)
        self.assertLess(ndn_svs, ndnsd)

    def test_ndn_svs_retains_project_boost_171_compatibility(self) -> None:
        lock = json.loads(
            (REPO / "packaging/ndnsf-di-container/oci/locks/gpu.lock").read_text()
        )
        self.assertEqual(
            lock["sourceRepositories"]["ndn-svs"],
            {
                "url": "https://github.com/matianxing1992/ndn-svs.git",
                "revision": "7b616b08624a79617bb05f2d3553bbbacdc4c482",
            },
        )
        self.assertIn("libboost-all-dev", lock["systemPackages"])
        self.assertNotIn("sourceArchives", lock)

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
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.foundation"
        ).read_text()
        materialize = dockerfile.index("/src/dependencies/NFD/websocketpp")
        nfd_build = dockerfile.index("cd /src/dependencies/NFD")
        self.assertLess(materialize, nfd_build)
        self.assertIn("websocketpp/version.hpp", dockerfile)

    def test_openabe_uses_locked_relic_and_make_adapter(self) -> None:
        lock = json.loads(
            (REPO / "packaging/ndnsf-di-container/oci/locks/gpu.lock").read_text()
        )
        self.assertEqual(
            lock["sourceRepositories"]["NAC-ABE"]["revision"],
            "390e9001a8611e04c90f3a5866d09c3136c885d0",
        )
        self.assertEqual(
            lock["sourceRepositories"]["relic"]["revision"],
            "b984e901ba78c83ea4093ea96addd13628c8c2d0",
        )
        self.assertEqual(lock["distributionBase"], "ubuntu20.04-openssl1.1")
        self.assertEqual(
            lock["baseImages"]["foundation"],
            "ubuntu@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214",
        )
        self.assertEqual(lock["pythonRuntime"], "3.10.18-bullseye-glibc2.31")
        self.assertEqual(
            set(lock["pythonRuntimePackages"]),
            {
                "libgdbm6", "libreadline8", "libsqlite3-0", "libssl1.1",
            },
        )
        self.assertEqual(
            set(lock["pythonExcludedOptionalExtensions"]), {"nis", "_tkinter"}
        )
        dockerfile = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.foundation"
        ).read_text()
        openabe = dockerfile[dockerfile.index("cd /src/dependencies/openabe") :]
        self.assertIn('SHELL ["/bin/bash", "-o", "pipefail", "-c"]', dockerfile)
        self.assertIn("libgtest-dev", lock["systemPackages"])
        self.assertIn("NO_DEPS=1", openabe)
        self.assertIn("make INSTALL_PREFIX=$PREFIX install", openabe)
        self.assertNotIn("cmake -S . -B build", openabe.split("NAC-ABE", 1)[0])
        self.assertLess(
            dockerfile.index("/src/dependencies/relic"),
            dockerfile.index("cd /src/dependencies/openabe"),
        )
        self.assertIn("-DHAVE_TESTS=TRUE", dockerfile)
        self.assertIn("NAC-ABE/build/tests/unit-tests -l test_suite -x", dockerfile)

    def test_python_runtime_dependency_closure_is_explicit(self) -> None:
        lock = json.loads(
            (REPO / "packaging/ndnsf-di-container/oci/locks/gpu.lock").read_text()
        )
        packages = {name.lower() for name in lock["pythonPackages"]}
        for required in (
            "filelock", "fsspec", "pyyaml", "requests", "tqdm",
            "typing-extensions", "coloredlogs", "flatbuffers", "sympy",
            "nvidia-ml-py", "pillow", "regex", "jinja2", "networkx",
        ):
            self.assertIn(required, packages)

    def test_qwen_runtime_excludes_unused_torch_media_packages(self) -> None:
        lock = json.loads(
            (REPO / "packaging/ndnsf-di-container/oci/locks/gpu.lock").read_text()
        )
        packages = {name.lower() for name in lock["pythonPackages"]}
        self.assertIn("torch", packages)
        self.assertTrue(
            {"torchaudio", "torchvision"}.isdisjoint(packages),
            "Qwen text inference must not ship unused native media plugins",
        )
        dockerfile = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu"
        ).read_text()
        matrix = (
            REPO / "packaging/ndnsf-di-container/oci/compatibility/gpu-matrix.yaml"
        ).read_text()
        self.assertNotIn("torchaudio", dockerfile)
        self.assertNotIn("torchvision", dockerfile)
        self.assertNotIn("torchaudio:", matrix)
        self.assertNotIn("torchvision:", matrix)
        self.assertIn("derive-runtime-packages.py", dockerfile)

    def test_official_gpu_image_packages_qwen_entrypoints_without_overlay_lock(self) -> None:
        dockerfile = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu"
        ).read_text()
        script_root = (
            "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/"
        )
        for script in (
            "run-ndnsf-qwen.sh",
            "run-qwen-reference.py",
            "sample-qwen-resources.py",
        ):
            self.assertIn(
                f"COPY {script_root}{script} /opt/ndnsf/bin/{script}",
                dockerfile,
            )
            self.assertIn(f"test -x /opt/ndnsf/bin/{script}", dockerfile)
        self.assertFalse(
            (REPO / "packaging/ndnsf-di-container/oci/experiments/Dockerfile.qwen").exists(),
            "The official GPU image must not require a second overlay image",
        )
        self.assertFalse(
            (REPO / "packaging/ndnsf-di-container/oci/locks/qwen-reference.lock").exists(),
            "gpu.lock must remain the single Python/CUDA version contract",
        )

    def test_qwen_slurm_uses_canonical_least_privilege_runner(self) -> None:
        template = (
            REPO
            / "packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/ndnsf-qwen.sbatch.in"
        ).read_text()
        self.assertIn("@@RUN_CONTAINER@@", template)
        self.assertIn('--release-bind "@@PROJECT_ROOT@@/releases"', template)
        self.assertIn('--models "@@MODEL_PATH@@"', template)
        self.assertIn('--artifacts "@@ARTIFACT_ROOT@@"', template)
        self.assertIn('--identity "@@IDENTITY_ROOT@@"', template)
        self.assertIn('--evidence "@@EVIDENCE_ROOT@@"', template)
        self.assertIn("--nfd-config /artifacts/nfd.conf", template)
        self.assertIn("--provider-args-dir /artifacts/providers", template)
        self.assertNotIn("apptainer exec", template)
        self.assertNotIn("@@PROJECT_ROOT@@:/project:rw", template)

    def test_python_focal_fixture_rejects_foreign_optional_abis(self) -> None:
        fixture = (
            REPO
            / "tests/container/itiger-qwen-live/fixtures/python310-ubuntu2004.Dockerfile"
        ).read_text()
        self.assertIn('ssl.OPENSSL_VERSION.startswith("OpenSSL 1.1.1f")', fixture)
        self.assertIn("-name 'nis.*.so'", fixture)
        self.assertIn("-name '_tkinter.*.so'", fixture)
        self.assertIn('grep -q "not found"', fixture)

    def test_gpu_assembly_locks_onnx_cpp_and_installs_uninstalled_examples(self) -> None:
        lock = json.loads(
            (REPO / "packaging/ndnsf-di-container/oci/locks/gpu.lock").read_text()
        )
        cpp = lock["onnxRuntimeCpp"]
        self.assertEqual(cpp["version"], "1.20.1")
        self.assertEqual(
            cpp["sha256"],
            "6bfb87c6ebe55367a94509b8ef062239e188dccf8d5caac8d6909b2344893bf0",
        )
        dockerfile = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu"
        ).read_text()
        self.assertIn("sha256sum -c -", dockerfile)
        self.assertIn("FROM ${PYTHON_BASE_IMAGE} AS python-runtime", dockerfile)
        self.assertIn("python3.10 -m venv", dockerfile)
        self.assertIn(
            "--root /usr/local/bin --root /usr/local/lib/python3.10",
            dockerfile,
        )
        self.assertIn(
            "install -m 0755 build/examples/App_ServiceController", dockerfile
        )
        self.assertIn(
            "install -m 0755 build/examples/di-native-provider", dockerfile
        )

    def test_gpu_assembler_installs_foundation_runtime_before_closure_scan(self) -> None:
        dockerfile = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu"
        ).read_text()
        install = "$(cat /opt/ndnsf-di/manifest/runtime-system-packages)"
        derive = "python3 /build-contract/derive-runtime-packages.py"
        self.assertIn(install, dockerfile)
        self.assertLess(
            dockerfile.index(install),
            dockerfile.index(derive),
            "Foundation runtime DSOs must be installed before ldd closure scanning",
        )

    def test_cuda_only_runtime_removes_locked_optional_tensorrt_provider(self) -> None:
        lock = json.loads(
            (REPO / "packaging/ndnsf-di-container/oci/locks/gpu.lock").read_text()
        )
        self.assertEqual(
            lock["onnxRuntimeExcludedOptionalProviders"],
            ["TensorrtExecutionProvider"],
        )
        dockerfile = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu"
        ).read_text()
        optional_dsos = (
            "/opt/onnxruntime/lib/libonnxruntime_providers_tensorrt.so",
            (
                "/opt/venv/lib/python3.10/site-packages/onnxruntime/capi/"
                "libonnxruntime_providers_tensorrt.so"
            ),
        )
        for optional_dso in optional_dsos:
            self.assertIn(optional_dso, dockerfile)
            self.assertRegex(dockerfile, rf"rm -f[^\n]*{re.escape(optional_dso)}")
            self.assertIn(f"test ! -e {optional_dso}", dockerfile)
        required_cuda_dsos = (
            "/opt/onnxruntime/lib/libonnxruntime_providers_cuda.so",
            (
                "/opt/venv/lib/python3.10/site-packages/onnxruntime/capi/"
                "libonnxruntime_providers_cuda.so"
            ),
        )
        for required_cuda_dso in required_cuda_dsos:
            self.assertIn(f"test -f {required_cuda_dso}", dockerfile)

    def test_final_source_and_immutable_foundation_source_are_bound_separately(self) -> None:
        dockerfile = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu"
        ).read_text()
        workflow = (REPO / ".github/workflows/ndnsf-di-itiger-image.yml").read_text()
        self.assertIn("ARG FOUNDATION_SOURCE_REVISION", dockerfile)
        self.assertIn(
            'test "$(cat /opt/ndnsf-di/manifest/source-revision)" = '
            '"$FOUNDATION_SOURCE_REVISION"',
            dockerfile,
        )
        self.assertIn(
            'org.ndnsf.di.foundation.revision="${FOUNDATION_SOURCE_REVISION}"',
            dockerfile,
        )
        self.assertIn("foundation_source_revision:", workflow)
        self.assertIn(
            "FOUNDATION_SOURCE_REVISION=${{ inputs.foundation_source_revision }}",
            workflow,
        )

    def test_runtime_image_has_dynamic_link_and_nfd_config_gates(self) -> None:
        dockerfile = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu"
        ).read_text()
        self.assertIn("runtime-system-packages", dockerfile)
        self.assertIn("/etc/ndn/nfd.conf", dockerfile)
        self.assertIn("/run/nfd", dockerfile)
        self.assertIn("verify-runtime-closure.py", dockerfile)
        self.assertIn("verify-python-environment.py", dockerfile)

    def test_runtime_image_does_not_upgrade_cuda_base_packages(self) -> None:
        dockerfile = (
            REPO / "packaging/ndnsf-di-container/oci/Dockerfile.gpu"
        ).read_text()
        runtime = dockerfile.split("AS runtime", 1)[1]
        self.assertIn("missing_packages", runtime)
        self.assertIn("dpkg-query -W -f='${db:Status-Abbrev}'", runtime)
        self.assertIn("grep -qx '.i '", runtime)
        self.assertNotIn(
            "apt-get install -y --no-install-recommends "
            "$(cat /tmp/runtime-system-packages)",
            runtime,
        )

    def test_preflight_accepts_the_repository_build_contract(self) -> None:
        result = subprocess.run(
            [
                "python3", str(PREFLIGHT),
                "--workspace", str(REPO),
                "--output", "/dev/null",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "PASS")

    def test_openabe_relic_materializer_applies_upstream_patch_contract(self) -> None:
        script = (
            REPO / "packaging/ndnsf-di-container/oci/scripts/prepare-openabe-relic.py"
        )
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            source = root / "relic"
            openabe = root / "openabe"
            (source / "include").mkdir(parents=True)
            (source / "CMakeLists.txt").write_text("project(relic)\n")
            (source / "include/relic_label.h").write_text(
                "#define ep2_mul value\n#define MIN MAX ALIGN rsa_t rsa_st\n"
            )
            (source / "src/md").mkdir(parents=True)
            (source / "src/md/blake2.h").write_text(
                "#pragma pack(push, 1)\n"
                "ALIGNME( 64 ) typedef struct first {} first;\n"
                "ALIGNME( 64 ) typedef struct second {} second;\n"
                "#pragma pack(pop)\n"
            )
            downloader = openabe / "deps/relic/download_relic.sh"
            downloader.parent.mkdir(parents=True)
            downloader.write_text(
                "COMMIT=b984e901ba78c83ea4093ea96addd13628c8c2d0\n"
            )
            (openabe / "deps/relic/Makefile").write_text(
                'one -DARCH="ARM" -DWSIZE=32\ntwo -DARCH="ARM" -DWSIZE=32\n'
            )
            (openabe / "src/keys").mkdir(parents=True)
            (openabe / "src/keys/zpkey.cpp").write_text(
                "EVP_PKEY_assign_EC_KEY(this->pkey, ec_key);\n"
            )
            result = subprocess.run(
                [
                    "python3", str(script),
                    "--source", str(source),
                    "--openabe", str(openabe),
                ],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            label = (
                openabe / "deps/relic/relic-toolkit-0.5.0/include/relic_label.h"
            ).read_text()
            self.assertNotIn("#define ep2_mul ", label)
            self.assertIn("RLC_MIN RLC_MAX RLC_ALIGN rlc_rsa_t rlc_rsa_st", label)
            self.assertTrue(
                (openabe / "deps/relic/relic-toolkit-0.5.0.tar.gz").is_file()
            )
            adapter = (openabe / "deps/relic/Makefile").read_text()
            self.assertEqual(adapter.count('-DARCH="X64" -DWSIZE=64'), 2)
            blake = (
                openabe / "deps/relic/relic-toolkit-0.5.0/src/md/blake2.h"
            ).read_text()
            self.assertNotIn("RLC_ALIGNME", blake)
            self.assertNotIn("#pragma pack", blake)

    def test_qwen_weights_are_excluded_from_build_context(self) -> None:
        patterns = set((REPO / ".dockerignore").read_text().splitlines())
        for required in (
            "*.safetensors", "*.gguf", "*.ckpt", "pytorch_model*.bin",
            "*.onnx", "*.onnx_data", "RELEASE",
        ):
            self.assertIn(required, patterns)

    def test_workflow_assembles_from_local_foundation_and_uploads_evidence_only(self) -> None:
        text = (REPO / ".github/workflows/ndnsf-di-itiger-image.yml").read_text()
        self.assertIn("foundation_image:", text)
        self.assertIn("FOUNDATION_IMAGE=${{ env.FOUNDATION_IMAGE }}", text)
        self.assertNotIn("prepare-sealed-context.py", text)
        self.assertNotRegex(text, r"(?m)^  push:")
        self.assertIn("push: true", text)
        self.assertNotIn("gh api --method PATCH", text)
        self.assertIn("Verify anonymous digest access", text)
        self.assertIn(
            'DOCKER_CONFIG="$anonymous_config" docker manifest inspect', text
        )
        self.assertIn("df -h", text)
        self.assertIn("preflight-gpu-build.py", text)
        self.assertRegex(text, r"(?s)Record runner disk after build.*?if: always\(\)")
        self.assertIn("path: results/spec110-itiger-qwen-live/release-build/", text)
        self.assertNotIn("path: .spec110-build", text)
        self.assertNotIn("runtime.oci", text)
        self.assertNotIn("runtime.sif", text)


if __name__ == "__main__":
    unittest.main()
