from __future__ import annotations

import os
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest

from contract._support import REPO


OCI = REPO / "packaging" / "ndnsf-di-container" / "oci"


class OciRuntimeScriptsTest(unittest.TestCase):
    def test_dockerfile_is_multistage_digest_argument_driven_and_non_root(self) -> None:
        text = (OCI / "Dockerfile.cpu").read_text(encoding="utf-8")
        self.assertGreaterEqual(text.count("FROM ${"), 2)
        self.assertIn("sha256sum --check", text)
        self.assertIn("USER 65532:65532", text)
        self.assertNotIn("systemctl", text)
        self.assertNotRegex(text, r"FROM\s+[^$\s]+:latest")

    def test_entrypoint_executes_only_declared_role_or_explicit_exec(self) -> None:
        entrypoint = OCI / "scripts" / "entrypoint.sh"
        with tempfile.TemporaryDirectory() as directory:
            release = Path(directory)
            (release / "bin").mkdir()
            provider = release / "bin/di-native-provider"
            provider.write_text("#!/bin/sh\nprintf 'provider:%s\\n' \"$*\"\n", encoding="utf-8")
            provider.chmod(0o755)
            environment = {**os.environ, "NDNSF_RELEASE_ROOT": str(release)}
            result = subprocess.run([str(entrypoint), "provider", "--check-only"], env=environment,
                                    text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "provider:--check-only")
            invalid = subprocess.run([str(entrypoint), "systemd"], env=environment,
                                     text=True, capture_output=True, check=False)
            self.assertEqual(invalid.returncode, 2)
            explicit = subprocess.run([str(entrypoint), "exec", "/bin/true"], env=environment,
                                      text=True, capture_output=True, check=False)
            self.assertEqual(explicit.returncode, 0, explicit.stderr)

    def test_entrypoint_copies_read_only_identity_source_to_ephemeral_home(self) -> None:
        entrypoint = OCI / "scripts" / "entrypoint.sh"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            release = root / "release"
            source = root / "identity-source"
            home = root / "ephemeral-home"
            (release / "bin").mkdir(parents=True)
            source.mkdir()
            (source / "pib.db").write_text("synthetic identity database", encoding="utf-8")
            provider = release / "bin/di-native-provider"
            provider.write_text("#!/bin/sh\ntest -f \"$HOME/pib.db\"\n", encoding="utf-8")
            provider.chmod(0o755)
            environment = {**os.environ, "NDNSF_RELEASE_ROOT": str(release),
                           "NDNSF_IDENTITY_SOURCE": str(source), "HOME": str(home)}
            result = subprocess.run([str(entrypoint), "provider"], env=environment,
                                    text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((home / "pib.db").is_file())

    def test_healthcheck_is_layered_and_missing_socket_fails(self) -> None:
        health = OCI / "scripts" / "healthcheck.sh"
        text = health.read_text(encoding="utf-8")
        self.assertIn("nfdc status", text)
        self.assertIn("kill -0 1", text)
        self.assertNotIn("systemctl", text)
        result = subprocess.run([str(health), "application"],
                                env={**os.environ, "NDNSF_NFD_SOCKET": "/tmp/spec108-does-not-exist.sock"},
                                text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("NFD_SOCKET_NOT_READY", result.stderr)

    def test_prepare_rootfs_verifies_release_and_captures_runtime_closure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            release = root / "release"
            output = root / "rootfs"
            (release / "bin").mkdir(parents=True)
            for name in ("App_ServiceController", "di-native-provider", "ndn-repo-ng", "nfd", "nfdc"):
                shutil.copy2("/bin/true", release / "bin" / name)
            (release / "release.json").write_text(json.dumps({"releaseId": "unit-r1"}) + "\n", encoding="utf-8")
            lines = []
            for path in sorted(item for item in release.rglob("*") if item.is_file()):
                lines.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(release)}")
            (release / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")
            result = subprocess.run([str(OCI / "scripts/prepare-rootfs.sh"), "--release", str(release),
                                     "--output", str(output)], text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((output / "opt/ndnsf-di/current").is_symlink())
            self.assertTrue((output / "manifest/SHA256SUMS").is_file())
            self.assertTrue(any((output / "lib").rglob("libc.so*")))

    def test_build_image_check_only_requires_digest_pinned_bases(self) -> None:
        root = REPO / "dist" / "ndnsf-di-container" / f"unit-{os.getpid()}"
        try:
            (root / "manifest").mkdir(parents=True)
            (root / "manifest/SHA256SUMS").write_text("", encoding="utf-8")
            sha = "a" * 64
            command = [str(OCI / "scripts/build-image.sh"), "--rootfs", str(root), "--tag", "unit:test",
                       "--build-base", f"ubuntu@sha256:{sha}", "--runtime-base", f"ubuntu@sha256:{sha}",
                       "--check-only"]
            valid = subprocess.run(command, text=True, capture_output=True, check=False)
            self.assertEqual(valid.returncode, 0, valid.stderr)
            invalid_command = list(command)
            invalid_command[invalid_command.index("--runtime-base") + 1] = "ubuntu:latest"
            invalid = subprocess.run(invalid_command, text=True, capture_output=True, check=False)
            self.assertEqual(invalid.returncode, 4)
        finally:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
