from __future__ import annotations

import copy
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from contract._support import FIXTURES, load_impl

profile_impl = load_impl("profile")
compose_impl = load_impl("adapters/docker_compose")


class FakeRunner:
    def __init__(self) -> None:
        self.commands: list[list[str]] = []

    def __call__(self, command, **kwargs):
        self.commands.append(list(command))
        stdout = ""
        if command[:2] == ["docker", "version"]:
            stdout = '{"Client":{"Version":"26"},"Server":{"Version":"26"}}\n'
        elif command[:3] == ["docker", "compose", "version"]:
            stdout = "2.27.0\n"
        elif command[:3] == ["docker", "image", "inspect"]:
            if "{{json .RepoDigests}}" in command:
                stdout = '["registry.example/ndnsf-di@sha256:' + "a" * 64 + '"]\n'
            else:
                stdout = "sha256:" + "b" * 64 + "\n"
        elif "ps" in command:
            stdout = "[" + ",".join(
                '{"Service":"' + name + '","State":"running","Health":"healthy"}'
                for name in ("nfd", "controller", "provider")) + "]\n"
        elif "logs" in command:
            stdout = "provider ready\n"
        return subprocess.CompletedProcess(command, 0, stdout=stdout, stderr="")


class ComposeAdapterTest(unittest.TestCase):
    def prepared_profile(self, root: Path) -> dict:
        profile = copy.deepcopy(profile_impl.load_profile(FIXTURES / "profiles" / "cloud-cpu-valid.yaml"))
        identity = root / "identity"
        project = root / "project"
        nfd_run = root / "nfd-run"
        identity.mkdir()
        (project / "config/nfd").mkdir(parents=True)
        nfd_run.mkdir()
        for name in ("policies.conf", "trust-schema.conf", "native-execution-plan.json", "native-service-manifest.json"):
            (project / "config" / name).write_text("{}\n", encoding="utf-8")
        identity.chmod(0o700)
        nfd_run.chmod(0o770)
        profile["identity"]["reference"] = str(identity)
        profile["storage"]["projectRoot"] = str(project)
        profile["compose"]["nfdSocket"] = str(nfd_run / "nfd.sock")
        profile["network"]["localEndpoint"] = "unix://" + str(nfd_run / "nfd.sock")
        return profile

    def test_preflight_renders_config_without_mutating_runtime(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runner = FakeRunner()
            adapter = compose_impl.DockerComposeAdapter(runner=runner)
            result = adapter.preflight(self.prepared_profile(Path(directory)))
        self.assertEqual(result["status"], "PASS")
        flattened = [" ".join(command) for command in runner.commands]
        self.assertTrue(any("compose" in command and "config --quiet" in command for command in flattened))
        self.assertFalse(any(" up " in f" {command} " for command in flattened))

    def test_materialize_uses_digest_pull_and_records_image_id(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runner = FakeRunner()
            adapter = compose_impl.DockerComposeAdapter(runner=runner)
            value = adapter.materialize(self.prepared_profile(Path(directory)))
        self.assertEqual(value["type"], "docker-image")
        self.assertTrue(value["id"].startswith("sha256:"))
        self.assertIn("@sha256:", next(command[-1] for command in runner.commands if command[:2] == ["docker", "pull"]))

    def test_start_and_stop_use_wait_and_preserve_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runner = FakeRunner()
            adapter = compose_impl.DockerComposeAdapter(runner=runner)
            profile = self.prepared_profile(Path(directory))
            with mock.patch.object(compose_impl, "validate_mount_contract", return_value={}):
                adapter.start(profile)
                stopped = adapter.stop(profile)
        flattened = [" ".join(command) for command in runner.commands]
        self.assertTrue(any("up --detach --wait" in command for command in flattened))
        self.assertTrue(any(command.endswith("stop") for command in flattened))
        self.assertTrue(stopped["statePreserved"])

    def test_status_requires_complete_healthy_service_set(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runner = FakeRunner()
            adapter = compose_impl.DockerComposeAdapter(runner=runner)
            profile = self.prepared_profile(Path(directory))
            self.assertEqual(adapter.status(profile)["status"], "PASS")
            original = runner.__call__

            def unhealthy(command, **kwargs):
                if "ps" in command:
                    return subprocess.CompletedProcess(command, 0,
                        stdout='[{"Service":"nfd","State":"exited","Health":"unhealthy"}]\n', stderr="")
                return original(command, **kwargs)

            adapter = compose_impl.DockerComposeAdapter(runner=unhealthy)
            with self.assertRaisesRegex(compose_impl.ComposeAdapterError, "SERVICE_SET_INVALID"):
                adapter.status(profile)


if __name__ == "__main__":
    unittest.main()
