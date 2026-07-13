from __future__ import annotations

import copy
import os
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import yaml

from contract._support import FIXTURES, REPO, load_impl

profile_impl = load_impl("profile")
compose_impl = load_impl("adapters/docker_compose")


class ComposeStorageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.profile = profile_impl.load_profile(FIXTURES / "profiles" / "cloud-cpu-valid.yaml")

    def test_compose_mounts_identity_read_only_and_state_as_bind(self) -> None:
        path = REPO / "packaging" / "ndnsf-di-container" / "adapters" / "docker-compose" / "compose.yaml"
        provider = yaml.safe_load(path.read_text(encoding="utf-8"))["services"]["provider"]
        rendered = "\n".join(str(item) for item in provider["volumes"])
        self.assertIn("NDNSF_IDENTITY_ROOT", rendered)
        self.assertIn("read_only", rendered)
        self.assertIn("NDNSF_PROJECT_ROOT", rendered)
        self.assertIn("NDNSF_NFD_RUN_DIR", rendered)

    def test_mount_contract_and_socket_ownership(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            identity = root / "identity"
            project = root / "project"
            nfd_run = root / "nfd-run"
            for path in (identity, project, nfd_run):
                path.mkdir()
            (project / "config/nfd").mkdir(parents=True)
            for name in ("policies.conf", "trust-schema.conf", "native-execution-plan.json", "native-service-manifest.json"):
                (project / "config" / name).write_text("{}\n", encoding="utf-8")
            os.chmod(identity, 0o700)
            os.chmod(nfd_run, 0o770)
            profile = copy.deepcopy(self.profile)
            profile["identity"]["reference"] = str(identity)
            profile["storage"]["projectRoot"] = str(project)
            profile["compose"]["nfdSocket"] = str(nfd_run / "nfd.sock")
            profile["network"]["localEndpoint"] = "unix://" + str(nfd_run / "nfd.sock")
            compose_impl.validate_mount_contract(profile, require_socket=False)
            socket_path = nfd_run / "nfd.sock"
            original_stat = Path.stat

            def fake_stat(path: Path, *args, **kwargs):
                if path == socket_path:
                    return SimpleNamespace(st_mode=stat.S_IFSOCK | 0o660, st_uid=os.geteuid())
                return original_stat(path, *args, **kwargs)

            with mock.patch.object(Path, "stat", fake_stat):
                compose_impl.validate_mount_contract(profile, require_socket=True)

    def test_world_writable_identity_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            identity = Path(directory) / "identity"
            identity.mkdir()
            os.chmod(identity, 0o777)
            profile = copy.deepcopy(self.profile)
            profile["identity"]["reference"] = str(identity)
            with self.assertRaisesRegex(compose_impl.ComposeAdapterError, "IDENTITY_WORLD_WRITABLE"):
                compose_impl.validate_mount_contract(profile, require_socket=False)


if __name__ == "__main__":
    unittest.main()
