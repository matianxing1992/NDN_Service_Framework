from __future__ import annotations

import ast
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
import sys
import sysconfig
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Experiments"))

import native_install_candidate as candidate  # noqa: E402
import native_yolo_requester  # noqa: E402
from native_probe_validation_links import render_anchored_probe_validation_schema  # noqa: E402


class InstalledCandidateTest(unittest.TestCase):
    def test_actual_runner_profile_serializes_candidate_paths(self):
        tree = ast.parse((ROOT / "Experiments/NDNSF_DI_Yolo2x2_Minindn.py").read_text())
        call = next(node for node in ast.walk(tree) if isinstance(node, ast.Call)
                    and isinstance(node.func, ast.Name)
                    and node.func.id == "build_yolo_native_candidate")
        profile = next(item.value for item in call.keywords if item.arg == "profile")
        expression = next(value for key, value in zip(profile.keys, profile.values)
                          if isinstance(key, ast.Constant) and key.value == "arguments")
        code = compile(ast.Expression(expression), "actual-yolo-profile", "eval")
        for candidate_path in (Path("/tmp/spec201-candidate.json"), None):
            with self.subTest(candidate_path=candidate_path):
                args = SimpleNamespace(spec201_candidate=candidate_path,
                    requester_publication_dir=Path("/tmp/spec201-publications"),
                    spec201_candidate_sha256="a" * 64, input_size=32)
                record = eval(code, {"args_cli": args})
                encoded = json.loads(json.dumps(record))
                self.assertEqual(encoded["spec201_candidate"],
                    str(candidate_path.resolve()) if candidate_path is not None else None)
                self.assertEqual(encoded["requester_publication_dir"], "/tmp/spec201-publications")
                self.assertEqual(encoded["spec201_candidate_sha256"], "a" * 64)
                self.assertEqual(encoded["input_size"], 32)

    def test_probe_schema_has_explicit_anchors_and_rooted_signature_rules(self):
        certificate = Path("/tmp/spec197-root.cert")
        schema = render_anchored_probe_validation_schema(
            "/NDNSF-DistributeInference/example", [certificate, certificate])
        self.assertNotIn("type any", schema)
        self.assertEqual(schema.count("trust-anchor"), 1)
        self.assertIn(json.dumps(str(certificate.resolve())), schema)
        for kind in ("data", "interest"):
            self.assertIn("for " + kind, schema)
        self.assertEqual(schema.count("sig-type rsa-sha256"), 2)
        self.assertEqual(schema.count("sig-type ecdsa-sha256"), 2)
        self.assertIn(json.dumps("^<NDNSF\\-DistributeInference><example>[^<KEY>]*<KEY><>{1,3}$"), schema)
        with self.assertRaises(ValueError):
            render_anchored_probe_validation_schema("/example", [])
        with self.assertRaises(ValueError):
            render_anchored_probe_validation_schema("/example/<.*>", [certificate])

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.installed = self.root / "usr/local"
        self.built = self.root / "build"
        self.source_package = self.root / "source/ndnsf"
        self.installed_package = self.installed / "python/ndnsf"
        self.source_app_package = self.root / "repo/ndnsf_distributed_inference"
        self.model = self.root / "model"
        self.model.mkdir()
        self.source_package.mkdir(parents=True)
        self.installed_package.mkdir(parents=True)
        (self.source_app_package / "app_sdk").mkdir(parents=True)
        (self.source_app_package / "__init__.py").write_text("", encoding="utf-8")
        (self.source_app_package / "app_sdk/client.py").write_text(
            "class APPClient: pass\n", encoding="utf-8")
        (self.source_app_package / "compatibility").mkdir()
        (self.source_app_package / "compatibility/manifest.json").write_text(
            '{"exports": []}\n', encoding="utf-8")
        self.topology = self.root / "topology.conf"
        self.topology.write_text("user\nprovider\n", encoding="utf-8")
        self.policy = self.root / "policy.yaml"
        self.write_policy(["/app/user"])
        self.runtime_input = self.root / "native/requester.json"
        self.runtime_input.parent.mkdir()
        self.runtime_input.write_text('{"run":"one"}\n', encoding="utf-8")
        self.model_file = self.model / "shard.onnx"
        self.model_file.write_bytes(b"model-artifact")
        for name in ("requester", "provider", "authority", "worker"):
            self._write_binary(self.installed / "bin" / name, name)
            self._write_binary(self.built / name, name)
        self.extension_installed = self.installed_package / "_ndnsf.cpython-38-x86_64-linux-gnu.so"
        self.extension_built = self.source_package / "_ndnsf.cpython-38-x86_64-linux-gnu.so"
        self._write_binary(self.extension_installed, "extension")
        self._write_binary(self.extension_built, "extension")
        for package in (self.source_package, self.installed_package):
            (package / "__init__.py").write_text("from .api import run\n", encoding="utf-8")
            (package / "api.py").write_text("def run(): return True\n", encoding="utf-8")
        self.components = {
            name: (self.installed / "bin" / name, self.built / name)
            for name in ("requester", "provider", "authority", "worker")
        }
        self.run_id = "0123456789abcdef0123456789abcdef"

    def _write_binary(self, path: Path, content: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        path.chmod(0o755)

    def write_policy(self, users: list[str]) -> None:
        self.policy.write_text(
            "services:\n"
            "  - name: /AI/YOLO/2x2Inference\n"
            f"    users: {json.dumps(users)}\n"
            "    roles: [/Stage/0/Shard/0]\n"
            "    providers: [{identity: /app/provider, roles: all}]\n",
            encoding="utf-8")

    def fake_binary_check(self, path: Path) -> dict:
        if not path.is_file() or not os.access(path, os.X_OK):
            return {"path": str(path), "status": "FAIL"}
        return {"path": str(path), "status": "PASS",
                "sha256": candidate.sha256_file(path), "sharedLibraries": []}

    def _binary_check_process(self, path: Path, ldd_output: str):
        def run(command, **_kwargs):
            output = "" if command[0].endswith("readelf") else ldd_output
            return candidate.subprocess.CompletedProcess(command, 0, output, "")
        return mock.patch.object(candidate.subprocess, "run", side_effect=run)

    def build(self) -> dict:
        with mock.patch.object(candidate, "binary_check", side_effect=self.fake_binary_check):
            return candidate.build_yolo_native_candidate(
                run_id=self.run_id, components=self.components,
                extension=(self.extension_installed, self.extension_built),
                source_package=self.source_package,
                installed_package=self.installed_package,
                source_app_package=self.source_app_package,
                topology=self.topology, policy=self.policy,
                model_directory=self.model, model_input="yolo26n.pt",
                input_size=32, layout="2x2",
                service_name="/AI/YOLO/2x2Inference",
                requester_identity="/app/user", provider_nodes=["a", "b", "c", "d"],
                profile={"id": "test-profile"},
                runtime_inputs=[self.runtime_input])

    def test_candidate_binds_installed_components_and_inputs(self) -> None:
        record = self.build()
        self.assertEqual(record["status"], "PASS")
        self.assertEqual(record["runId"], self.run_id)
        self.assertEqual(record["authorization"]["requester"], "/app/user")
        self.assertEqual(record["model"]["artifacts"][0]["sha256"],
                         candidate.sha256_file(self.model_file))
        self.assertEqual(record["runtimeAssets"][0]["sha256"],
                         candidate.sha256_file(self.runtime_input))
        self.assertEqual(
            record["pythonApplication"]["packageFiles"]["app_sdk/client.py"],
            candidate.sha256_file(self.source_app_package / "app_sdk/client.py"))
        self.assertEqual(
            record["pythonApplication"]["packageFiles"]
            ["compatibility/manifest.json"],
            candidate.sha256_file(
                self.source_app_package / "compatibility/manifest.json"))
        output = self.root / "candidate.json"
        candidate.write_candidate(output, record)
        self.assertEqual(json.loads(output.read_text())["candidateDigest"],
                         record["candidateDigest"])

    def test_run_scoped_assets_change_the_candidate_digest(self) -> None:
        first = self.build()
        self.runtime_input.write_text('{"run":"two"}\n', encoding="utf-8")
        second = self.build()
        self.assertNotEqual(first["candidateDigest"], second["candidateDigest"])
        self.assertNotEqual(first["runtimeAssets"], second["runtimeAssets"])

    def test_python_application_package_data_changes_the_candidate_digest(self) -> None:
        first = self.build()
        app_manifest = self.source_app_package / "compatibility/manifest.json"
        app_manifest.write_text('{"exports": ["changed"]}\n', encoding="utf-8")
        second = self.build()
        self.assertNotEqual(first["candidateDigest"], second["candidateDigest"])
        self.assertNotEqual(first["pythonApplication"], second["pythonApplication"])

    def test_stale_component_mutations_reject_without_launching_processes(self) -> None:
        for name, (installed, _built) in self.components.items():
            with self.subTest(component=name):
                installed.write_text("stale-" + name, encoding="utf-8")
                with mock.patch.object(candidate, "binary_check",
                                       side_effect=self.fake_binary_check), \
                     mock.patch.object(candidate.subprocess, "run") as run:
                    with self.assertRaisesRegex(RuntimeError,
                                                "CANDIDATE_COMPONENT_STALE"):
                        candidate.build_yolo_native_candidate(
                            run_id=self.run_id, components=self.components,
                            extension=(self.extension_installed, self.extension_built),
                            source_package=self.source_package,
                            installed_package=self.installed_package,
                            source_app_package=self.source_app_package,
                            topology=self.topology, policy=self.policy,
                            model_directory=self.model, model_input="yolo26n.pt",
                            input_size=32, layout="2x2",
                            service_name="/AI/YOLO/2x2Inference",
                            requester_identity="/app/user", provider_nodes=["a", "b"],
                            profile={"id": "test-profile"})
                    run.assert_not_called()
                installed.write_text(name, encoding="utf-8")

    def test_missing_existing_service_permission_rejects(self) -> None:
        self.write_policy(["/app/other-user"])
        with mock.patch.object(candidate, "binary_check",
                               side_effect=self.fake_binary_check), \
             mock.patch.object(candidate.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError,
                                        "CANDIDATE_TARGETED_REQUESTER_NOT_AUTHORIZED"):
                self.build_candidate_without_mock()
            run.assert_not_called()

    def build_candidate_without_mock(self) -> dict:
        with mock.patch.object(candidate, "binary_check", side_effect=self.fake_binary_check):
            return candidate.build_yolo_native_candidate(
                run_id=self.run_id, components=self.components,
                extension=(self.extension_installed, self.extension_built),
                source_package=self.source_package,
                installed_package=self.installed_package,
                source_app_package=self.source_app_package,
                topology=self.topology, policy=self.policy,
                model_directory=self.model, model_input="yolo26n.pt",
                input_size=32, layout="2x2",
                service_name="/AI/YOLO/2x2Inference",
                requester_identity="/app/user", provider_nodes=["a", "b"],
                profile={"id": "test-profile"})

    def test_runner_checks_candidate_before_mininet_cleanup_or_start(self) -> None:
        source = (ROOT / "Experiments/NDNSF_DI_Yolo2x2_Minindn.py").read_text()
        check = source.index("native_candidate = build_yolo_native_candidate(")
        cleanup = source.index("Minindn.cleanUp()")
        start = source.index("ndn.start()")
        self.assertLess(check, cleanup)
        self.assertLess(check, start)

    def test_native_child_imports_current_app_sdk_and_installed_extension(self) -> None:
        source = (ROOT / "Experiments/NDNSF_DI_Yolo2x2_Minindn.py").read_text()
        source_package = source.index('str(REPO / "NDNSF-DistributedInference")',
                                      source.index("python_path_parts = ["))
        inserted_installed_root = source.index(
            "python_path_parts.insert(1, str(installed_package.parent))")
        self.assertLess(source_package, inserted_installed_root)
        self.assertIn("source_app_package=(REPO / \"NDNSF-DistributedInference\" /", source)

    def test_model_generation_uses_source_adapter_and_installed_binding(self) -> None:
        source = (ROOT / "Experiments/NDNSF_DI_Yolo2x2_Minindn.py").read_text()
        main = next(node for node in ast.parse(source).body
                    if isinstance(node, ast.FunctionDef) and node.name == "main")
        start = next(i for i, node in enumerate(main.body)
                     if isinstance(node, ast.Assign)
                     and any(isinstance(t, ast.Name) and t.id == "py_path_parts"
                             for t in node.targets))
        block = ast.Module(body=main.body[start:start + 3], type_ignores=[])
        app_root = self.root / "NDNSF-DistributedInference"
        adapter = app_root / "ndnsf_distributed_inference/adapters/yolo"
        adapter.mkdir(parents=True)
        for directory in (adapter, adapter.parent, adapter.parent.parent):
            (directory / "__init__.py").write_text("", encoding="utf-8")
        global_app = self.installed_package.parent / "ndnsf_distributed_inference"
        global_app.mkdir()
        (global_app / "__init__.py").write_text("", encoding="utf-8")
        local_binding = self.root / "pythonWrapper/ndnsf"
        local_binding.mkdir(parents=True)
        (local_binding / "__init__.py").write_text("", encoding="utf-8")
        namespace = {"REPO": self.root, "PY_DIR": self.root / "scripts",
                     "os": os, "str": str,
                     "args_cli": SimpleNamespace(native_providers=True),
                     "installed_package": self.installed_package}
        exec(compile(block, "model-generation-path", "exec"), namespace)
        process = subprocess.run(
            [sys.executable, "-c",
             "import json, ndnsf; import ndnsf_distributed_inference.adapters.yolo as y; "
             "print(json.dumps([y.__file__, ndnsf.__file__]))"],
            env={**os.environ, "PYTHONPATH": namespace["py_path"]},
            cwd=self.root, capture_output=True, text=True, check=True)
        self.assertEqual(json.loads(process.stdout),
                         [str(adapter / "__init__.py"),
                          str(self.installed_package / "__init__.py")])

    def test_runner_supplies_closed_loop_mode_to_app_environment(self) -> None:
        source = (ROOT / "Experiments/NDNSF_DI_Yolo2x2_Minindn.py").read_text()
        tree = ast.parse(source)
        args_call = next(node for node in ast.walk(tree)
                         if isinstance(node, ast.Call)
                         and isinstance(node.func, ast.Name)
                         and node.func.id == "Args")
        keywords = {keyword.arg: keyword.value for keyword in args_call.keywords}
        self.assertEqual(ast.literal_eval(keywords["workload_mode"]), "closed-loop")

    def test_runner_wires_native_requester_inputs_and_authority_before_launch(self) -> None:
        source = (ROOT / "Experiments/NDNSF_DI_Yolo2x2_Minindn.py").read_text()
        self.assertIn('parser.add_argument("--native-requester"', source)
        self.assertIn('"--native-requester-config"', source)
        self.assertIn('"--canonical-package"', source)
        self.assertIn('"--catalogue-registry"', source)
        self.assertIn('candidate_components["authority"]', source)
        self.assertIn(
            "providers = provider_role_assignments(load_policy_roles(CONFIG))", source)
        self.assertNotIn('"--roles", "all"', source)
        self.assertIn("collect_native_yolo_evidence(", source)


class NativeRequesterEvidenceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifest_digest = "sha256:" + "a" * 64
        self.snapshot_digest = "sha256:" + "b" * 64
        self.goodput_digest = "sha256:" + "f" * 64
        self.roles = ["/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"]
        placement = {
            "/Backbone": "/provider/1",
            "/Head/Shard/0": "/provider/2",
            "/Head/Shard/1": "/provider/2",
            "/Merge": "/provider/3",
        }
        assignments = [
            {"role_key": role, "provider": placement[role]}
            for role in self.roles
        ]
        dispatch = {
            "requestId": "native-request-1",
            "attempt": "1",
            "observer": "/provider/2",
            "rttCount": "0",
            "goodputEdgeCount": "1",
            "rttPairs": "[]",
            "goodputEdges": json.dumps([
                {"src_provider": "/provider/1", "dst_provider": "/provider/2"}
            ], separators=(",", ":")),
            "invocation": "NDNSF_Targeted",
        }
        snapshot = {
            "requestId": "native-request-1",
            "status": "accepted",
            "snapshotDigest": self.snapshot_digest,
            "ageMs": "4",
            "expiresInMs": "599996",
            "activeProbeRttRows": "[]",
            "activeProbeGoodputRows": json.dumps([
                {"reported_by": "/provider/2", "src_provider": "/provider/1",
                 "dst_provider": "/provider/2", "sample_digest": self.goodput_digest}
            ], separators=(",", ":")),
        }
        decision = {
            "status": "SCORED",
            "score_mode": "network-only",
            "score_is_end_to_end_latency": False,
            "snapshot_digest": self.snapshot_digest,
            "fallback_reason": "",
            "selected_assignments": assignments,
            "edge_costs": [{
                "producer_role": "/Backbone",
                "consumer_role": "/Head/Shard/0",
                "src_peer": "/provider/1",
                "dst_peer": "/provider/2",
                "rate_method": "ACTIVE_PROBE_GOODPUT",
                "goodput_mbps": 14.5,
                "sample_digest": self.goodput_digest,
            }],
        }
        user_rows = [
            "YOLO_NATIVE_REQUEST_RESULT status=true request=native-request-1 payload_bytes=128",
            "NDNSF_DI_NATIVE_PLACEMENT_PROBE_DISPATCH " + " ".join(
                f"{key}={value}" for key, value in dispatch.items()),
            "NDNSF_DI_NATIVE_PLACEMENT_PROBE_RESPONSE requestId=native-request-1 "
            "observer=/provider/2 status=accepted payloadBytes=64",
            "NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT " + " ".join(
                f"{key}={value}" for key, value in snapshot.items()),
            "NDNSF_DI_NATIVE_PLACEMENT_DECISION requestId=native-request-1 "
            "strategy=native-observation-aware-v4 decisionJson=" +
            json.dumps(decision, separators=(",", ":")),
            "NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId=native-request-1 "
            "attemptEpoch=1 planDigest=sha256:" + "c" * 64,
        ]
        self.user_text = "\n".join(user_rows)
        self.provider_text = "\n".join(
            "NDNSF_DI_NATIVE_SELECTION_ACCEPTED requestId=native-request-1 "
            f"attemptEpoch=1 provider={placement[role]} role={role} "
            "planDigest=sha256:" + "c" * 64 + f" manifestDigest={self.manifest_digest}"
            for role in self.roles
        )

    _write_binary = InstalledCandidateTest._write_binary
    _binary_check_process = InstalledCandidateTest._binary_check_process

    def validate(self, user_text: str | None = None,
                 provider_text: str | None = None) -> dict:
        return native_yolo_requester.validate_native_yolo_evidence(
            self.user_text if user_text is None else user_text,
            self.provider_text if provider_text is None else provider_text,
            self.roles,
            expected_manifest_digest=self.manifest_digest,
        )

    def test_accepts_native_result_with_fresh_consumed_probe_and_scored_placement(self) -> None:
        evidence = self.validate()
        self.assertEqual(evidence["snapshotDigest"], self.snapshot_digest)
        self.assertEqual(set(evidence["placementByRole"]), set(self.roles))
        self.assertEqual(len(evidence["targetedProbeCosts"]), 1)

    def collect(self, user_text: str | None = None,
                provider_text: str | None = None) -> dict:
        return native_yolo_requester.collect_native_yolo_evidence(
            self.user_text if user_text is None else user_text,
            self.provider_text if provider_text is None else provider_text,
            self.roles, expected_manifest_digest=self.manifest_digest,
            expected_score_mode="network-only")

    def test_reports_all_three_success_boundaries(self) -> None:
        report = self.collect()
        self.assertEqual(report["status"], "PASS")
        for boundary in ("probe", "placement", "model"):
            self.assertEqual(report["boundaries"][boundary]["status"], "PASS")

    def cached_ack_text(self) -> str:
        text = "\n".join(line for line in self.user_text.splitlines()
                         if "NDNSF_DI_NATIVE_PLACEMENT_PROBE_DISPATCH " not in line and
                         "NDNSF_DI_NATIVE_PLACEMENT_PROBE_RESPONSE " not in line)
        offers = [{"provider": provider, "boot_epoch": "boot-" + provider,
                   "signer_identity": provider, "captured_at_ms": 1000,
                   "expires_at_ms": 6000, "offer_digest": self.snapshot_digest,
                   "ack_wire_digest": self.manifest_digest,
                   "topology_digest": self.snapshot_digest}
                  for provider in ("/provider/1", "/provider/2", "/provider/3")]
        row = {"reported_by": "/provider/2", "src_provider": "/provider/1",
               "dst_provider": "/provider/2", "sample_digest": self.goodput_digest,
               "source_kind": "ACK_CACHE", "captured_at_ms": 990, "expires_at_ms": 5990,
               "observer_peer": "/provider/2", "observer_boot_epoch": "boot-/provider/2",
               "src_boot_epoch": "boot-/provider/1", "dst_boot_epoch": "boot-/provider/2",
               "topology_digest": self.snapshot_digest,
               "probe_complete": True, "probe_signature_valid": True,
               "probe_source_timing_validated": True, "probe_segments_sent": 16,
               "probe_rate_status": "MEASURED_GOODPUT", "probe_train_goodput_mbps": 14.5}
        compact = lambda value: json.dumps(value, separators=(",", ":"))
        text = re.sub(r"activeProbeGoodputRows=\S+", "activeProbeGoodputRows=" + compact([row]), text)
        text = text.replace("activeProbeRttRows=[]", "activeProbeRttRows=[] checkedAtMs=1004 " +
                            "providerOfferBindings=" + compact(offers) + " providerRoster=" +
                            compact([offer["provider"] for offer in offers]) + " rttPairs=[] " +
                            "goodputEdges=" + compact([{"src_provider": "/provider/1",
                                                        "dst_provider": "/provider/2"}]))
        return text

    def test_accepts_fresh_ack_cached_sample_without_claiming_targeted_call(self) -> None:
        report = self.collect(self.cached_ack_text())
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["probeDispatches"], [])
        self.assertEqual(report["probeResponses"], [])
        self.assertEqual(report["requestedGoodputEdges"], [])
        self.assertEqual(report["sampleSources"], ["ACK_CACHE"])
        self.assertEqual(report["targetedProbeCosts"], [])
        self.assertEqual(len(report["cachedAckProbeCosts"]), 1)
        self.assertEqual(len(report["qualifiedProbeCosts"]), 1)
        self.assertEqual(report["cachedAckGoodputEdges"],
                         [("/provider/2", "/provider/1", "/provider/2")])

    def cached_service_text(self) -> str:
        text = self.cached_ack_text()
        match = re.search(r"activeProbeGoodputRows=(\S+)", text)
        rows = json.loads(match.group(1))
        rows[0]["source_kind"] = "TARGETED_RESPONSE"
        for field in ("observer_peer", "observer_boot_epoch", "topology_digest"):
            rows[0].pop(field)
        text = text.replace(match.group(0), "activeProbeGoodputRows=" +
                            json.dumps(rows, separators=(",", ":")))
        dispatch = next(line for line in self.user_text.splitlines()
                        if "NDNSF_DI_NATIVE_PLACEMENT_PROBE_DISPATCH " in line)
        return text + "\n" + dispatch.replace("PLACEMENT_PROBE_DISPATCH", "PLACEMENT_CACHE_DISPATCH") + (
            "\nNDNSF_DI_NATIVE_PLACEMENT_CACHE_RESPONSE requestId=native-request-1 "
            "observer=/provider/2 status=accepted payloadBytes=512"
            "\nNDNSF_DI_NATIVE_PLACEMENT_CACHE_RESULT requestId=native-request-1 "
            "observer=/provider/2 status=hit")

    def test_cache_service_hit_is_used_without_new_measurement_claim(self) -> None:
        report = self.collect(self.cached_service_text())
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["probeDispatches"], [])
        self.assertEqual(report["probeResponses"], [])
        self.assertEqual(report["requestedGoodputEdges"], [])
        self.assertEqual(report["cachedAckGoodputEdges"], [])
        self.assertEqual(report["cachedServiceGoodputEdges"],
                         [("/provider/2", "/provider/1", "/provider/2")])
        self.assertEqual(len(report["cacheQueryDispatches"]), 1)
        self.assertEqual(len(report["cachedServiceProbeCosts"]), 1)
        self.assertEqual(report["targetedProbeCosts"], [])
        self.assertEqual(report["cachedAckProbeCosts"], [])

    def test_cache_service_rejects_missing_duplicate_or_inconsistent_exchange(self) -> None:
        text = self.cached_service_text()
        query = next(line for line in text.splitlines() if "CACHE_DISPATCH " in line)
        result = next(line for line in text.splitlines() if "CACHE_RESULT " in line)
        for changed in (text.replace(result, ""), text + "\n" + result,
                        text.replace("status=hit", "status=miss"),
                        text.replace("CACHE_RESPONSE", "UNRELATED_RESPONSE"),
                        text + "\n" + query.replace("CACHE_DISPATCH", "PROBE_DISPATCH")):
            with self.subTest(text=changed[-150:]):
                self.assertEqual(self.collect(changed)["boundaries"]["probe"]["status"], "FAIL")

    def test_cache_service_rejects_boot_expiry_quality_and_unconsumed_edge(self) -> None:
        text = self.cached_service_text()
        for old, new in (('"src_boot_epoch":"boot-/provider/1"', '"src_boot_epoch":"foreign"'),
                         ('"expires_at_ms":5990', '"expires_at_ms":1004'),
                         ('"probe_rate_status":"MEASURED_GOODPUT"', '"probe_rate_status":"UNKNOWN"'),
                         ('"probe_source_timing_validated":true', '"probe_source_timing_validated":false'),
                         ('goodputEdges=[{"src_provider":"/provider/1","dst_provider":"/provider/2"}] invocation=',
                          'goodputEdges=[{"src_provider":"/provider/3","dst_provider":"/provider/2"}] invocation=')):
            with self.subTest(field=old):
                changed = text.replace(old, new)
                self.assertNotEqual(changed, text)
                self.assertEqual(self.collect(changed)["boundaries"]["probe"]["status"], "FAIL")

    def test_cache_miss_preserves_measured_cost_and_requires_matching_fallback(self) -> None:
        query = next(line for line in self.user_text.splitlines() if "PROBE_DISPATCH " in line)
        cache = query.replace("PLACEMENT_PROBE_DISPATCH", "PLACEMENT_CACHE_DISPATCH")
        text = self.user_text + "\n" + cache + (
            "\nNDNSF_DI_NATIVE_PLACEMENT_CACHE_RESPONSE requestId=native-request-1 "
            "observer=/provider/2 status=accepted payloadBytes=512"
            "\nNDNSF_DI_NATIVE_PLACEMENT_CACHE_RESULT requestId=native-request-1 "
            "observer=/provider/2 status=miss")
        report = self.collect(text)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(len(report["targetedProbeCosts"]), 1)
        self.assertEqual(report["cachedServiceProbeCosts"], [])
        changed = text.replace(cache, cache.replace("attempt=1", "attempt=2"))
        self.assertEqual(self.collect(changed)["boundaries"]["probe"]["status"], "FAIL")

    def test_rejects_cached_ack_boot_scope_expiry_topology_and_unknown_provenance(self) -> None:
        original = self.cached_ack_text()
        for old, new in (("\"src_boot_epoch\":\"boot-/provider/1\"", "\"src_boot_epoch\":\"old-boot\""),
                         ("expires_at_ms\":5990", "expires_at_ms\":1004"),
                         ("expires_at_ms\":5990", "expires_at_ms\":6000"),
                         ('goodputEdges=[{"src_provider":"/provider/1"',
                          'goodputEdges=[{"src_provider":"/provider/3"'),
                         ('"topology_digest":"' + self.snapshot_digest + '"',
                          '"topology_digest":"unknown"'),
                         ('"probe_source_timing_validated":true', '"probe_source_timing_validated":false'),
                         ("\"source_kind\":\"ACK_CACHE\"", "\"source_kind\":\"UNKNOWN\"")):
            with self.subTest(new=new):
                report = self.collect(original.replace(old, new))
                self.assertEqual(report["boundaries"]["probe"]["status"], "FAIL")

    def test_rejects_unproved_cache_in_legacy_targeted_log(self) -> None:
        text = "\n".join(line for line in self.user_text.splitlines()
                         if "NDNSF_DI_NATIVE_PLACEMENT_PROBE_DISPATCH " not in line and
                         "NDNSF_DI_NATIVE_PLACEMENT_PROBE_RESPONSE " not in line)
        self.assertEqual(self.collect(text)["boundaries"]["probe"]["status"], "FAIL")

    def test_model_failure_retains_successful_probe_and_placement(self) -> None:
        report = self.collect(self.user_text.replace(
            "YOLO_NATIVE_REQUEST_RESULT status=true", "YOLO_NATIVE_REQUEST_RESULT status=false"))
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["boundaries"]["model"]["status"], "FAIL")
        self.assertEqual(report["boundaries"]["probe"]["status"], "PASS")
        self.assertEqual(report["boundaries"]["placement"]["status"], "PASS")
        self.assertEqual(report["snapshotDigest"], self.snapshot_digest)

    def test_rtt_only_snapshot_cannot_qualify_requested_goodput(self) -> None:
        pair = json.dumps([{"src_provider": "/provider/2",
                            "dst_provider": "/provider/1"}], separators=(",", ":"))
        row = json.dumps([{"reported_by": "/provider/2",
                           "src_provider": "/provider/2",
                           "dst_provider": "/provider/1"}], separators=(",", ":"))
        text = self.user_text.replace("rttCount=0", "rttCount=1").replace(
            "rttPairs=[]", "rttPairs=" + pair).replace(
            "activeProbeRttRows=[]", "activeProbeRttRows=" + row)
        text = re.sub(r"activeProbeGoodputRows=\S+", "activeProbeGoodputRows=[]", text)
        text = text.replace('"status":"SCORED"', '"status":"BASELINE_FALLBACK"')
        report = self.collect(text)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["boundaries"]["probe"]["status"], "FAIL")
        self.assertEqual(report["boundaries"]["probe"]["missingGoodputRows"], 1)
        self.assertEqual(report["boundaries"]["probe"]["missingRttRows"], 0)
        self.assertEqual(len(report["activeProbeRttRows"]), 1)
        self.assertEqual(report["boundaries"]["model"]["status"], "PASS")
        self.assertEqual(report["boundaries"]["placement"]["reason"],
                         "SPEC196_NATIVE_OBSERVATION_STRATEGY_FELL_BACK")

    def test_fallback_retains_probe_and_model_success(self) -> None:
        report = self.collect(self.user_text.replace(
            '"status":"SCORED"', '"status":"BASELINE_FALLBACK"'))
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["boundaries"]["probe"]["status"], "PASS")
        self.assertEqual(report["boundaries"]["model"]["status"], "PASS")
        self.assertEqual(report["boundaries"]["placement"]["status"], "FAIL")
        self.assertEqual(report["placementDecision"]["status"], "BASELINE_FALLBACK")

    def test_missing_probe_evidence_does_not_fail_the_model_boundary(self) -> None:
        report = self.collect(self.user_text.splitlines()[0])
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["boundaries"]["probe"]["status"], "FAIL")
        self.assertEqual(report["boundaries"]["model"]["status"], "PASS")

    def test_missing_provider_evidence_preserves_probe_and_model_outcomes(self) -> None:
        report = self.collect(provider_text="")
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["boundaries"]["probe"]["status"], "PASS")
        self.assertEqual(report["boundaries"]["model"]["status"], "PASS")
        self.assertEqual(report["boundaries"]["placement"]["status"], "FAIL")

    def test_score_mode_mismatch_cannot_qualify_placement(self) -> None:
        report = self.collect(self.user_text.replace(
            '"score_mode":"network-only"', '"score_mode":"preparation-and-network"'))
        self.assertEqual(report["boundaries"]["probe"]["status"], "PASS")
        self.assertEqual(report["boundaries"]["placement"]["status"], "FAIL")
        self.assertIn("SCORE_MODE_MISMATCH", report["boundaries"]["placement"]["reason"])

    def test_malformed_decision_is_reported_without_erasing_other_boundaries(self) -> None:
        marker = next(line for line in self.user_text.splitlines()
                      if line.startswith("NDNSF_DI_NATIVE_PLACEMENT_DECISION "))
        prefix = marker.split(" decisionJson=", 1)[0]
        report = self.collect(self.user_text.replace(marker, prefix + " decisionJson=[]"))
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["boundaries"]["placement"]["status"], "FAIL")
        self.assertEqual(report["boundaries"]["probe"]["status"], "PASS")
        self.assertEqual(report["boundaries"]["model"]["status"], "PASS")

    def test_qwen_reports_network_success_independently_of_its_cpp_model_oracle(self) -> None:
        import NDNSF_DI_Qwen06B_LocalExperiment as qwen_local
        config_dir = self.root / "requester"
        config_dir.mkdir()
        (config_dir / "config.json").write_text(json.dumps({
            "request": {"placement_score_mode": "network-only"},
            "catalog": {"source": {"model_manifest_digest": self.manifest_digest}},
        }))
        (self.root / "native-qwen-execution-plan.json").write_text(json.dumps({
            "services": [{"roles": self.roles}]}))
        (self.root / "requester-0.log").write_text(self.user_text)
        (self.root / "provider-0.log").write_text(self.provider_text)
        for model_status in ("PASS", "FAIL"):
            report = qwen_local.collect_qwen_boundaries(
                self.root, 1, {"cppOracle": {"status": model_status}})
            self.assertEqual(report["model"]["status"], model_status)
            self.assertEqual(report["probe"]["status"], "PASS")
            self.assertEqual(report["placement"]["status"], "PASS")
        (self.root / "requester-0.log").rename(self.root / "requester.log")
        report = qwen_local.collect_qwen_boundaries(
            self.root, 1, {"cppOracle": {"status": "FAIL"}})
        self.assertEqual(report["model"]["status"], "FAIL")
        self.assertEqual(report["probe"]["status"], "PASS")
        self.assertEqual(report["placement"]["status"], "PASS")
        (self.root / "requester.log").write_text(
            self.user_text + "\n" + self.user_text.replace("native-request-1", "native-request-2"))
        (self.root / "provider-0.log").write_text(
            self.provider_text + "\n" +
            self.provider_text.replace("native-request-1", "native-request-2"))
        report = qwen_local.collect_qwen_boundaries(
            self.root, 2, {"cppOracle": {"status": "FAIL"}})
        self.assertEqual(report["probe"]["status"], "PASS")
        self.assertEqual(report["placement"]["status"], "PASS")
        self.assertEqual([row["requestId"] for row in report["probe"]["rounds"]],
                         ["native-request-1", "native-request-2"])

    def test_accepts_timestamped_runtime_evidence_records(self) -> None:
        prefix = "1790952712.445550  WARN: [ndnsf.di.RuntimeEvidence] "
        user_text = "\n".join(
            prefix + line if line.startswith("NDNSF_DI_") else line
            for line in self.user_text.splitlines()
        )
        provider_text = "\n".join(prefix + line for line in self.provider_text.splitlines())
        evidence = self.validate(user_text, provider_text)
        self.assertEqual(evidence["snapshotDigest"], self.snapshot_digest)

    def test_offer_key_id_loads_with_the_installed_cryptography_backend(self) -> None:
        _, public = native_yolo_requester._write_ed25519_keypair(self.root, "offer")
        self.assertRegex(native_yolo_requester._public_key_id(public), r"^sha256:[0-9a-f]{64}$")

    def test_rejects_probe_present_but_not_used_by_selected_v4_edge(self) -> None:
        marker = next(line for line in self.user_text.splitlines()
                      if line.startswith("NDNSF_DI_NATIVE_PLACEMENT_DECISION "))
        prefix, encoded = marker.split(" decisionJson=", 1)
        decision = json.loads(encoded)
        decision["edge_costs"][0]["sample_digest"] = "sha256:" + "e" * 64
        changed = self.user_text.replace(
            marker, prefix + " decisionJson=" + json.dumps(decision, separators=(",", ":")))
        with self.assertRaisesRegex(RuntimeError, "TARGETED_SAMPLE_NOT_USED"):
            self.validate(changed)

    def test_rejects_fallback_or_stale_snapshot(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "OBSERVATION_STRATEGY_FELL_BACK"):
            self.validate(self.user_text.replace('"status":"SCORED"',
                                                  '"status":"BASELINE_FALLBACK"'))
        with self.assertRaisesRegex(RuntimeError, "SNAPSHOT_FRESHNESS"):
            self.validate(self.user_text.replace("expiresInMs=599996", "expiresInMs=0"))

    def test_rejects_final_provider_assignment_or_manifest_mismatch(self) -> None:
        changed = self.provider_text.replace("provider=/provider/1", "provider=/provider/9", 1)
        with self.assertRaisesRegex(RuntimeError, "FINAL_PLACEMENT_INCOMPLETE"):
            self.validate(provider_text=changed)

        changed = self.provider_text.replace(self.manifest_digest, "sha256:" + "d" * 64, 1)
        with self.assertRaisesRegex(RuntimeError, "FINAL_PLACEMENT_INCOMPLETE"):
            self.validate(provider_text=changed)

    def test_committed_manifest_binds_package_and_provider_identity(self) -> None:
        manifest = {"schema": "ndnsf-di-canonical-model-manifest-v1",
                    "state": "ACTIVE",
                    "modelName": "model with spaces",
                    "metadata": {"packageManifestDigest": self.manifest_digest}}
        payload = json.dumps(manifest, separators=(",", ":"), sort_keys=True)
        digest = "sha256:" + hashlib.sha256(payload.encode()).hexdigest()
        user_text = self.user_text + f" manifestDigest={digest} manifestJson={payload}"
        provider_text = self.provider_text.replace(self.manifest_digest, digest)
        self.validate(user_text, provider_text=provider_text)
        with self.assertRaisesRegex(RuntimeError, "COMMITTED_MANIFEST_INVALID"):
            self.validate(user_text.replace(self.manifest_digest, "sha256:" + "d" * 64),
                          provider_text=provider_text)
        with self.assertRaisesRegex(RuntimeError, "FINAL_PLACEMENT_INCOMPLETE"):
            self.validate(user_text, provider_text=self.provider_text)

    def test_yolo_repo_grants_controller_only_probe_and_insert(self) -> None:
        policy_path = (ROOT / "examples/python/NDNSF-DistributedInference/"
                       "yolo_2x2/yolo_policy.yaml")
        policy = yaml.safe_load(policy_path.read_text(encoding="utf-8"))
        users = {service["name"]: set(service["users"])
                 for service in policy["services"]}
        controller = "/NDNSF-DistributeInference/example/controller"
        app_user = "/NDNSF-DistributeInference/example/user"
        for service_name in (
                "/NDNSF/DistributedRepo/Object/v1/STATUS",
                "/NDNSF/DistributedRepo/Object/v1/INSERT"):
            with self.subTest(service=service_name):
                self.assertEqual(users[service_name], {controller, app_user})
        for service_name in (
                "/NDNSF/DistributedRepo/Object/v1/FETCH",
                "/NDNSF/DistributedRepo/Object/v1/DELETE"):
            with self.subTest(service=service_name):
                self.assertEqual(users[service_name], {app_user})

    def test_generated_yolo_policy_includes_repo_operation_permissions(self) -> None:
        controller = "/NDNSF-DistributeInference/example/controller"
        app_user = "/NDNSF-DistributeInference/example/user"
        generated = self.root / "generated-policy.yaml"
        generated.write_text(yaml.safe_dump({
            "application": "yolo-2x2-demo",
            "controller": controller,
            "runtime": {"user_identity": app_user},
            "services": [{
                "name": "/AI/YOLO/2x2Inference",
                "users": [app_user],
                "providers": [{"identity": "/custom/compute", "roles": ["/Stage/0"]}],
            }],
        }, sort_keys=False), encoding="utf-8")
        template = (ROOT / "examples/python/NDNSF-DistributedInference/"
                    "yolo_2x2/yolo_policy.yaml")
        candidate.apply_yolo_repo_operation_policy(generated, template)
        policy = yaml.safe_load(generated.read_text(encoding="utf-8"))
        probe = next(service for service in policy["services"]
                     if service["name"] == "/NDNSF/Network/Probe/v1")
        self.assertEqual(probe["users"], [app_user])
        self.assertEqual(probe["providers"],
                         [{"identity": "/custom/compute", "roles": []}])
        provider_summary = next(entry for entry in policy["authorization_summary"]["providers"]
                                if entry["identity"] == "/custom/compute")
        self.assertIn({"service": "/NDNSF/Network/Probe/v1", "roles": []},
                      provider_summary["services"])
        users = {service["name"]: set(service["users"])
                 for service in policy["services"]}
        self.assertEqual(users["/NDNSF/DistributedRepo/Object/v1/STATUS"],
                         {controller, app_user})
        self.assertEqual(users["/NDNSF/DistributedRepo/Object/v1/INSERT"],
                         {controller, app_user})
        self.assertEqual(users["/NDNSF/DistributedRepo/Object/v1/DELETE"],
                         {app_user})

    def test_binary_check_allows_current_cpython_extension_api_symbols(self) -> None:
        suffix = sysconfig.get_config_var("EXT_SUFFIX")
        if not suffix:
            self.skipTest("current interpreter has no extension suffix")
        extension = self.root / f"_ndnsf{suffix}"
        self._write_binary(extension, "python extension")
        ldd_output = (
            f"undefined symbol: PyExc_ValueError ({extension})\n"
            f"undefined symbol: _Py_FalseStruct ({extension})\n")
        with self._binary_check_process(extension, ldd_output):
            report = candidate.binary_check(extension)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(len(report["pythonRuntimeSymbols"]), 2)
        self.assertEqual(report["unresolved"], [])

    def test_binary_check_still_rejects_non_python_extension_symbols(self) -> None:
        suffix = sysconfig.get_config_var("EXT_SUFFIX")
        if not suffix:
            self.skipTest("current interpreter has no extension suffix")
        extension = self.root / f"_ndnsf{suffix}"
        self._write_binary(extension, "python extension")
        ldd_output = (
            f"undefined symbol: PyExc_ValueError ({extension})\n"
            f"undefined symbol: ndnsf_missing ({extension})\n")
        with self._binary_check_process(extension, ldd_output):
            report = candidate.binary_check(extension)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(len(report["pythonRuntimeSymbols"]), 1)
        self.assertIn("ndnsf_missing", report["unresolved"][0])

    def test_installed_package_resolution_uses_active_python_import_path(self) -> None:
        package = self.root / "usr/local/lib/python3.8/dist-packages/ndnsf"
        package.mkdir(parents=True)
        spec = SimpleNamespace(submodule_search_locations=[str(package)])
        with mock.patch.object(candidate.importlib.util, "find_spec", return_value=spec):
            resolved = candidate.resolve_installed_python_package()
        self.assertEqual(resolved, package.resolve())

    def test_installed_package_resolution_rejects_missing_importable_package(self) -> None:
        with mock.patch.object(candidate.importlib.util, "find_spec", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "CANDIDATE_PYTHON_PACKAGE_NOT_IMPORTABLE"):
                candidate.resolve_installed_python_package()


if __name__ == "__main__":
    unittest.main()
