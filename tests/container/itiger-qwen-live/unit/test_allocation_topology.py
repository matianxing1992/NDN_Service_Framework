from __future__ import annotations

import copy
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
LIB = REPO / "packaging" / "ndnsf-di-container" / "lib"
if str(LIB) not in sys.path:
    sys.path.insert(0, str(LIB))

import allocation_topology as topology


FIXTURES = REPO / "tests/container/itiger-qwen-live/fixtures/network"


def load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text())


class AllocationTopologyTest(unittest.TestCase):
    def test_single_and_multi_node_process_maps(self) -> None:
        single = topology.validate_process_map(load("single-node.json"))
        multi = topology.validate_process_map(load("multi-node-tcp.json"))
        self.assertEqual(1, len(single["nodes"]))
        self.assertEqual(2, len(multi["nodes"]))
        self.assertEqual(2, sum(row["kind"] == "nfd" for row in multi["processes"]))
        self.assertEqual(3, sum(row["kind"] == "provider" for row in multi["processes"]))

    def test_udp_is_an_independent_selected_transport_variant(self) -> None:
        value = load("multi-node-tcp.json")
        value.update(load("variants.json")["udp"])
        result = topology.validate_process_map(value)
        self.assertEqual("udp", result["selectedTransport"])
        self.assertTrue(all(route["transport"] == "udp" for route in result["routes"]))

    def test_duplicate_identity_fails_closed(self) -> None:
        value = load("single-node.json")
        value["processes"][3]["identityRef"] = value["processes"][2]["identityRef"]
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_DUPLICATE_IDENTITY"):
            topology.validate_process_map(value)

    def test_partial_readiness_fails_closed(self) -> None:
        value = load("single-node.json")
        value["processes"][-1]["readinessInputs"] = ["provider-not-ready"]
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_READINESS_DEPENDENCY_INVALID"):
            topology.validate_process_map(value)

    def test_shell_injection_fails_before_render(self) -> None:
        value = load("single-node.json")
        value["processes"][-1]["command"] = ["python3", "user.py;touch", "/tmp/escaped"]
        value["processes"][-1]["commandDigest"] = topology.command_digest(value["processes"][-1]["command"])
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_COMMAND_UNSAFE"):
            topology.render_multiprog(value)

    def test_duplicate_nfd_fails_closed(self) -> None:
        value = load("single-node.json")
        duplicate = copy.deepcopy(value["processes"][0])
        duplicate.update(processId="nfd-extra", taskRank=6, readinessOutput="nfd-extra-ready", shutdownOrder=7)
        value["processes"].append(duplicate)
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_DUPLICATE_NFD"):
            topology.validate_process_map(value)

    def test_teardown_signal_and_audit_are_mandatory(self) -> None:
        for field, changed in (("signals", ["TERM"]), ("zeroSurvivorAudit", False)):
            value = load("single-node.json")
            value[field] = changed
            with self.subTest(field=field), self.assertRaisesRegex(
                topology.TopologyError, "TOPOLOGY_TEARDOWN_POLICY_INVALID"
            ):
                topology.validate_process_map(value)

    def test_nfd_and_multiprog_rendering_is_complete(self) -> None:
        value = load("single-node.json")
        template = (REPO / "packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/nfd.conf.in").read_text()
        rendered = topology.render_nfd_config(template, value["nodes"][0], "/tmp/ndnsf-di-job/nfd/0")
        self.assertNotIn("@@", rendered)
        self.assertIn("port 16363", rendered)
        multiprog = topology.render_multiprog(value)
        self.assertEqual(len(value["processes"]), len(multiprog.splitlines()))
        self.assertIn("0 nfd --config /tmp/spec110/nfd-0.conf", multiprog)

    def test_process_launchers_isolate_identity_and_runtime_environment(self) -> None:
        value = load("multi-node-tcp.json")
        scratch = "/tmp/ndnsf-di-test-launcher"
        workdir = "/project/tma1/ndnsf-di/bundle"
        provider = next(row for row in value["processes"] if row["kind"] == "provider")
        rendered = topology.render_process_launcher(provider, "/tmp/ndnsf-di-job", workdir)
        self.assertIn("identity_source=/project/tma1/ndnsf-di/identities/c1/provider-0", rendered)
        self.assertIn('cp -a "$identity_source/." "$runtime_home/"', rendered)
        self.assertIn('export NDN_CLIENT_PIB="pib-sqlite3:$runtime_home/.ndn/pib.db"', rendered)
        self.assertIn('export NDN_CLIENT_TPM="tpm-file:$runtime_home/.ndn/ndnsec-key-file"', rendered)
        self.assertIn("unset NDN_CLIENT_PIB NDN_CLIENT_TPM", rendered)
        self.assertIn('cd "$runtime_workdir"', rendered)
        self.assertIn("export HOME=", rendered)
        self.assertEqual(
            subprocess.run(["bash", "-n"], input=rendered, text=True, check=False).returncode,
            0,
        )

        nfd = next(row for row in value["processes"] if row["kind"] == "nfd")
        nfd_rendered = topology.render_process_launcher(nfd, "/tmp/ndnsf-di-job", workdir)
        self.assertIn("kind=nfd", nfd_rendered)
        self.assertNotIn("identity_source=", nfd_rendered)
        self.assertIn('runtime_config=/tmp/ndnsf-di-job/generated/nfd-0.conf', nfd_rendered)
        self.assertIn('exec nfd --config "$runtime_config"', nfd_rendered)
        self.assertNotIn("/tmp/spec110/", nfd_rendered)
        self.assertEqual(
            subprocess.run(["bash", "-n"], input=nfd_rendered, text=True, check=False).returncode,
            0,
        )

    def test_nfd_config_is_rebound_to_job_scratch(self) -> None:
        value = load("single-node.json")
        nfd = next(row for row in value["processes"] if row["kind"] == "nfd")
        rendered = topology.render_process_launcher(
            nfd, "/tmp/ndnsf-di-job", "/project/tma1/ndnsf-di/bundle"
        )
        self.assertIn('exec nfd --config "$runtime_config"', rendered)
        nfd["command"] = ["nfd", "--config=/tmp/spec110/shared.conf"]
        rendered = topology.render_process_launcher(
            nfd, "/tmp/ndnsf-di-job", "/project/tma1/ndnsf-di/bundle"
        )
        self.assertIn('exec nfd --config="$runtime_config"', rendered)
        nfd["command"] = ["nfd", "--config"]
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_NFD_CONFIG_INVALID"):
            topology.render_process_launcher(
                nfd, "/tmp/ndnsf-di-job", "/project/tma1/ndnsf-di/bundle"
            )

    def test_process_launcher_rebinds_explicit_identity_argument(self) -> None:
        value = load("single-node.json")
        controller = copy.deepcopy(next(row for row in value["processes"] if row["kind"] == "controller"))
        controller["command"] = ["App_ServiceController", "--identity", controller["identityRef"]]
        rendered = topology.render_process_launcher(
            controller, "/tmp/ndnsf-di-job", "/project/tma1/ndnsf-di/bundle"
        )
        self.assertIn('exec App_ServiceController --identity "$runtime_home"', rendered)
        self.assertNotIn("exec App_ServiceController --identity /project/", rendered)
        controller["command"] = ["App_ServiceController", "--identity=" + controller["identityRef"]]
        rendered = topology.render_process_launcher(
            controller, "/tmp/ndnsf-di-job", "/project/tma1/ndnsf-di/bundle"
        )
        self.assertIn('exec App_ServiceController --identity="$runtime_home"', rendered)

    def test_process_map_rejects_host_bound_application_argument(self) -> None:
        value = load("single-node.json")
        controller = next(row for row in value["processes"] if row["kind"] == "controller")
        controller["command"] = ["App_ServiceController", "--model=/project/tma1/shared/model.onnx"]
        controller["commandDigest"] = topology.command_digest(controller["command"])
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_HOST_PATH_COMMAND_INVALID"):
            topology.validate_process_map(value)

        controller["command"] = ["App_ServiceController", "--identity", "/project/tma1/other-role"]
        controller["commandDigest"] = topology.command_digest(controller["command"])
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_HOST_PATH_COMMAND_INVALID"):
            topology.render_process_launcher(controller, "/tmp/ndnsf-di-job", "/project/tma1/ndnsf-di/bundle")

    def test_provider_launcher_rejects_wrong_visible_gpu_uuid(self) -> None:
        value = load("multi-node-tcp.json")
        provider = next(row for row in value["processes"] if row["kind"] == "provider")
        with tempfile.TemporaryDirectory(prefix="spec110-identity-") as source_dir, \
             tempfile.TemporaryDirectory(prefix="ndnsf-di-") as scratch_dir, \
             tempfile.TemporaryDirectory(prefix="spec110-bin-") as bin_dir:
            source = Path(source_dir)
            provider["nfdSocket"] = str(Path(scratch_dir) / "nfd/1/nfd.sock")
            (source / ".ndn").mkdir()
            (source / ".ndn/pib.db").write_text("source-pib")
            (source / ".ndn/ndnsec-key-file").mkdir()
            fake = Path(bin_dir) / "di-native-provider"
            sentinel = Path(scratch_dir) / "provider-ran"
            fake.write_text(f"#!/bin/sh\ntouch {shlex.quote(str(sentinel))}\n")
            fake.chmod(0o700)
            smi = Path(bin_dir) / "nvidia-smi"
            smi.write_text("#!/bin/sh\nprintf '%s\\n' GPU-WRONG\n")
            smi.chmod(0o700)
            rendered = topology.render_process_launcher(provider, scratch_dir, source_dir)
            rendered = rendered.replace(
                "identity_source=" + shlex.quote(provider["identityRef"]),
                "identity_source=" + shlex.quote(str(source)),
            )
            result = subprocess.run(
                ["bash"], input=rendered, text=True,
                env={"PATH": f"{bin_dir}:/usr/bin:/bin", "HOME": "/ambient-home",
                     "CUDA_VISIBLE_DEVICES": "0"},
                capture_output=True, check=False,
            )
            self.assertEqual(8, result.returncode)
            self.assertIn("SPEC110_GPU_UUID_MISMATCH", result.stderr)
            self.assertFalse(sentinel.exists())

    def test_process_launcher_rejects_socket_from_another_job(self) -> None:
        value = load("single-node.json")
        process = next(row for row in value["processes"] if row["kind"] == "nfd")
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_SOCKET_SCOPE_INVALID"):
            topology.render_process_launcher(
                process, "/tmp/ndnsf-di-current-job", "/project/tma1/ndnsf-di/bundle"
            )

    def test_process_launcher_rejects_socket_path_traversal(self) -> None:
        value = load("single-node.json")
        process = next(row for row in value["processes"] if row["kind"] == "nfd")
        process["nfdSocket"] = "/tmp/ndnsf-di-current-job/../other/nfd.sock"
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_NFD_SOCKET_INVALID"):
            topology.render_process_launcher(
                process, "/tmp/ndnsf-di-current-job", "/project/tma1/ndnsf-di/bundle"
            )

    def test_nfd_state_path_traversal_is_rejected(self) -> None:
        value = load("single-node.json")
        template = (REPO / "packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/nfd.conf.in").read_text()
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_NFD_TEMPLATE_INVALID"):
            topology.render_nfd_config(template, value["nodes"][0], "/tmp/ndnsf-di-job/../other")

    def test_process_launcher_copies_read_only_identity_before_exec(self) -> None:
        value = load("multi-node-tcp.json")
        provider = next(row for row in value["processes"] if row["kind"] == "provider")
        original_source = provider["identityRef"]
        with tempfile.TemporaryDirectory(prefix="spec110-identity-") as source_dir, \
             tempfile.TemporaryDirectory(prefix="ndnsf-di-") as scratch_dir, \
             tempfile.TemporaryDirectory(prefix="spec110-bin-") as bin_dir:
            source = Path(source_dir)
            provider["nfdSocket"] = str(Path(scratch_dir) / "nfd/0/nfd.sock")
            (source / ".ndn").mkdir()
            (source / ".ndn/pib.db").write_text("source-pib")
            (source / ".ndn/ndnsec-key-file").mkdir()
            fake = Path(bin_dir) / "di-native-provider"
            observation = Path(scratch_dir) / "observation.txt"
            fake.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n%s\\n%s\\n%s\\n' \"$HOME\" \"$NDN_CLIENT_PIB\" "
                "\"$NDN_CLIENT_TPM\" \"$NDN_CLIENT_TRANSPORT\" >\"$SPEC110_OBSERVATION\"\n"
            )
            fake.chmod(0o700)
            rendered = topology.render_process_launcher(provider, scratch_dir, source_dir)
            rendered = rendered.replace(
                "identity_source=" + shlex.quote(original_source),
                "identity_source=" + shlex.quote(str(source)),
            )
            environment = {
                "PATH": bin_dir + ":/usr/bin:/bin",
                "HOME": "/ambient-home",
                "CUDA_VISIBLE_DEVICES": "0",
                "NDN_CLIENT_PIB": "pib-sqlite3:/ambient/pib",
                "NDN_CLIENT_TPM": "tpm-file:/ambient/tpm",
                "SPEC110_OBSERVATION": str(observation),
            }
            smi = Path(bin_dir) / "nvidia-smi"
            smi.write_text(
                "#!/bin/sh\nprintf '%s\\n' " + shlex.quote(provider["gpuUuid"]) + "\n"
            )
            smi.chmod(0o700)
            result = subprocess.run(
                ["bash"], input=rendered, text=True, env=environment,
                capture_output=True, check=False,
            )
            self.assertEqual(0, result.returncode, result.stderr)
            home, pib, tpm, transport = observation.read_text().splitlines()
            expected_home = str(Path(scratch_dir) / "homes" / provider["processId"])
            self.assertEqual(expected_home, home)
            self.assertEqual("pib-sqlite3:" + expected_home + "/.ndn/pib.db", pib)
            self.assertEqual("tpm-file:" + expected_home + "/.ndn/ndnsec-key-file", tpm)
            self.assertEqual("unix://" + provider["nfdSocket"], transport)
            self.assertEqual("source-pib", (Path(home) / ".ndn/pib.db").read_text())

    def test_provider_launcher_rejects_missing_gpu_binding(self) -> None:
        value = load("multi-node-tcp.json")
        provider = next(row for row in value["processes"] if row["kind"] == "provider")
        with tempfile.TemporaryDirectory(prefix="spec110-identity-") as source_dir, \
             tempfile.TemporaryDirectory(prefix="ndnsf-di-") as scratch_dir, \
             tempfile.TemporaryDirectory(prefix="spec110-bin-") as bin_dir:
            source = Path(source_dir)
            provider["nfdSocket"] = str(Path(scratch_dir) / "nfd/1/nfd.sock")
            (source / ".ndn").mkdir()
            (source / ".ndn/pib.db").write_text("source-pib")
            (source / ".ndn/ndnsec-key-file").mkdir()
            fake = Path(bin_dir) / "di-native-provider"
            sentinel = Path(scratch_dir) / "provider-ran"
            fake.write_text(f"#!/bin/sh\ntouch {shlex.quote(str(sentinel))}\n")
            fake.chmod(0o700)
            smi = Path(bin_dir) / "nvidia-smi"
            smi.write_text("#!/bin/sh\nprintf '%s\\n' GPU-EXPECTED\n")
            smi.chmod(0o700)
            rendered = topology.render_process_launcher(provider, scratch_dir, source_dir)
            rendered = rendered.replace(
                "identity_source=" + shlex.quote(provider["identityRef"]),
                "identity_source=" + shlex.quote(str(source)),
            )
            result = subprocess.run(
                ["bash"], input=rendered, text=True,
                env={"PATH": f"{bin_dir}:/usr/bin:/bin", "HOME": "/ambient-home"},
                capture_output=True, check=False,
            )
            self.assertEqual(8, result.returncode)
            self.assertIn("SPEC110_GPU_BINDING_MISSING", result.stderr)
            self.assertFalse(sentinel.exists())

    def test_process_launcher_rejects_implicit_relative_workdir(self) -> None:
        value = load("single-node.json")
        process = next(row for row in value["processes"] if row["kind"] == "nfd")
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_WORKDIR_INVALID"):
            topology.render_process_launcher(process, "/tmp/ndnsf-di-test-launcher", "bundle")

    def test_process_map_rejects_path_traversal_process_and_identity(self) -> None:
        value = load("single-node.json")
        value["processes"][1]["processId"] = "../controller"
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_PROCESS_ID_INVALID"):
            topology.validate_process_map(value)

        value = load("single-node.json")
        value["processes"][1]["identityRef"] = "/project/tma1/../shared/controller"
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_IDENTITY_BINDING_INVALID"):
            topology.validate_process_map(value)

        value = load("single-node.json")
        value["nodes"][0]["nfdSocket"] = "/tmp/ndnsf-di-job/../other/nfd.sock"
        for process in value["processes"]:
            process["nfdSocket"] = value["nodes"][0]["nfdSocket"]
        with self.assertRaisesRegex(topology.TopologyError, "TOPOLOGY_NFD_SOCKET_INVALID"):
            topology.validate_process_map(value)


if __name__ == "__main__":
    unittest.main()
