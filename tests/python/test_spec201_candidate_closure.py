"""Read-only candidate rejection tests; no MiniNDN or model staging."""
import hashlib
import ast
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Experiments"))
import native_install_candidate as candidate
import spec201_sdk_closure as sdk


class SdkClosureTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.wrapper = self.root / 'NDNSF-DistributedRepo/pythonWrapper'
        self.wrapper.mkdir(parents=True)
        self.source = self.wrapper / 'source.cpp'
        self.source.write_text('int fixture;')
        (self.wrapper / 'setup.py').write_text("Extension('fixture', sources=['source.cpp'])")
        (self.wrapper / 'pyproject.toml').write_text('[build-system]')
        self.header = self.wrapper / 'header.hpp'
        self.header.write_text('struct Fixture {};')
        (self.wrapper / 'fixture.o').write_bytes(b'object')
        (self.wrapper / 'fixture.d').write_text('fixture.o: source.cpp header.hpp\n')
        self.built = self.wrapper / 'py_repoclient/fixture.so'
        self.built.parent.mkdir()
        self.built.write_bytes(b'extension')
        (self.wrapper / 'linked.so').write_bytes(b'extension')
        self.installed = self.root / 'installed.so'
        self.installed.write_bytes(b'extension')
        self.log = self.root / 'compile.log'
        self.log.write_text('/usr/bin/gcc -c source.cpp -o fixture.o\n'
                            '/usr/bin/g++ -shared fixture.o -o linked.so\n')

    def test_failed_build_rejected_before_inputs(self):
        with self.assertRaisesRegex(RuntimeError, 'SDK_BUILD_FAILED'):
            sdk.collect_sdk_closure(wrapper=self.wrapper, log=self.log, built=self.built,
                                    installed=self.installed, environment={}, exit_status=1)

    def test_source_tree_cannot_be_installed(self):
        with patch.object(sdk, '__file__', str(self.root / 'Experiments/spec201_sdk_closure.py')):
            with self.assertRaisesRegex(RuntimeError, 'SDK_INSTALLED_PACKAGE_MISMATCH'):
                sdk.collect_sdk_closure(wrapper=self.wrapper, log=self.log, built=self.built,
                                        installed=self.built, environment={}, exit_status=0)

    def test_unmaintained_wrapper_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'SDK_MAINTAINED_WRAPPER_REQUIRED'):
            sdk.collect_sdk_closure(wrapper=self.wrapper, log=self.log, built=self.built,
                                    installed=self.installed, environment={}, exit_status=0)

    def test_legacy_setuptools_receives_explicit_package_metadata(self):
        tree = ast.parse((ROOT / 'pythonWrapper/setup.py').read_text())
        setup = next(node for node in ast.walk(tree) if isinstance(node, ast.Call)
                     and isinstance(node.func, ast.Name) and node.func.id == 'setup')
        fields = {keyword.arg: keyword.value for keyword in setup.keywords}
        self.assertEqual(ast.literal_eval(fields['name']), 'ndnsf')
        self.assertEqual(ast.literal_eval(fields['version']), '0.1.0')
        self.assertEqual(fields['packages'].func.id, 'find_packages')
        self.assertEqual(ast.literal_eval(fields['packages'].keywords[0].value),
                         ('ndnsf', 'ndnsf.*'))


class CandidateClosureTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.binary = self.root / "spec201-candidate-closure"
        self.binary.write_bytes(b"native oracle")
        self.binary.chmod(0o700)
        self.repo_binary = self.root / "spec201-transparent-repo"
        self.repo_binary.write_bytes(b"repo oracle")
        self.repo_binary.chmod(0o700)
        self.asset = self.root / "frozen-source"
        self.asset.write_bytes(b'trust-anchor { type file file-name "/owned/root.cert" }\n')
        self.manifest = self.root / "candidate.json"
        binary = self.binary_identity(self.binary)
        row = candidate.file_identity(self.asset)
        self.record = {"schema": candidate.SPEC201_CLOSURE_SCHEMA,
                       "artifacts": [{"installed": binary, "build": binary}],
                       "inventory": {key: [{"writer": "real writer",
                                            "consumer": "real consumer",
                                            "validator": "real validator",
                                            "referenceOwner": "real owner",
                                            "sourceFiles": [str(self.asset)],
                                            "target": self.binary.name}]
                                     for key in [f"C{i:02d}" for i in range(2, 11)] + ["C12"]},
                       "validation": {"backend": "original-packets",
                                      "nameBytesHardLimit": 1024,
                                      "packetBytesHardLimit": 8800,
                                      "uriModes": [0, 1], "status": "PASS",
                                      "nativeReceipts": []}}
        for group in ("sources", "headers", "contracts", "models", "configs",
                      "generatedSchemas", "topologies"):
            self.record[group] = [row]
        installed_header = self.root / "installed-header.hpp"
        installed_header.write_bytes(self.asset.read_bytes())
        installed_identity = candidate.file_identity(installed_header)
        self.record["headers"].append(installed_identity)
        self.record["installedHeaders"] = [{"source": row, "installed": installed_identity}]
        from native_prefix_roles import prefix_role_plan
        role_input = {"actors": ["/actual/producer"], "groups": ["/actual/group"],
                      "dataRoots": ["/actual"], "protocolVersion": 3}
        role_input["roles"] = list(prefix_role_plan(actors=role_input["actors"],
                                                   groups=role_input["groups"],
                                                   data_roots=role_input["dataRoots"]))
        self.record["prefixRolePlans"] = {"yolo": role_input, "qwen": role_input}
        self.record["discoveryEnvironment"] = candidate.SPEC201_DISCOVERY_ENVIRONMENT
        from native_probe_validation_links import render_anchored_probe_validation_schema
        schema = self.root / "frozen-schema.conf"
        schema.write_text(render_anchored_probe_validation_schema("/actual", [self.asset]))
        self.record["generatedSchemas"] = [candidate.file_identity(schema)]
        bootstrap = self.root / "bootstrap"
        for directory in ("pib", "tpm"):
            path = bootstrap / directory / "fixture"
            path.parent.mkdir(parents=True)
            path.write_bytes(b"private fixture")
            self.record["configs"].append(candidate.file_identity(path))
        common = {"schema": str(schema), "applicationRoot": "/actual",
                  "identities": ["/actual/producer"]}
        self.record["credentialBundles"] = {
            "yolo": {**common, "rootCert": str(self.asset),
                     "keys": {"/actual/producer": {"rsa": str(self.asset), "ec": None}}},
            "qwen": {**common, "bootstrapHome": str(bootstrap),
                     "certs": {"/actual/producer": str(self.asset)}}}
        repo_binary = self.binary_identity(self.repo_binary)
        self.record["artifacts"].append({"installed": repo_binary, "build": repo_binary})
        self.record["oracles"] = []
        graph = self.root / "build-map.json"
        graph.write_text(json.dumps({"schema": "ndnsf-spec201-build-closure-1",
                                    "rows": [{"storedSignature": "same", "currentSignature": "same",
                                              "source": row, "dependencies": [], "outputs": []}],
                                    "artifacts": [{"target": path.name, **candidate.file_identity(path)}
                                                  for path in (self.binary, self.repo_binary)]}))
        self.record["buildMaps"] = [candidate.file_identity(graph)]
        for native_binary, mode in ((path, mode) for path in (self.binary, self.repo_binary)
                                    for mode in ("0", "1")):
            log = self.root / f"{native_binary.name}-{mode}.log"
            log.write_text("Test module passed: 4 test cases out of 4 passed\n8 assertions out of 8 passed\n")
            receipt = self.root / f"{native_binary.name}-{mode}.json"
            receipt.write_text(json.dumps({"command": [str(native_binary), "--run_test=*"],
                                          "binary_sha256": candidate.sha256_file(native_binary),
                                          "exit_status": 0, "log": str(log),
                                          "environment": {"NDN_NAME_ALT_URI": mode}}))
            self.record["oracles"].extend(map(candidate.file_identity, (log, receipt)))
            self.record["validation"]["nativeReceipts"].append(str(receipt))
        self.expected_inventory = json.loads(json.dumps(self.record["inventory"]))

    def binary_identity(self, path):
        return {**candidate.file_identity(path), "status": "PASS", "sharedLibraries": []}

    def freeze(self):
        unsigned = {k: v for k, v in self.record.items() if k != "candidateId"}
        self.record["candidateId"] = hashlib.sha256(json.dumps(
            unsigned, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        self.manifest.write_text(json.dumps(self.record))
        return candidate.sha256_file(self.manifest)

    def validate(self):
        digest = self.freeze()
        with patch.object(candidate, "binary_check", side_effect=self.binary_identity), \
                patch.object(candidate, "naming_inventory", return_value=self.expected_inventory), \
                patch.object(candidate, "inspect_spec201_credentials"), \
                patch.object(candidate.subprocess, "run") as external:
            result = candidate.validate_spec201_candidate(self.manifest, digest, [self.binary])
            external.assert_not_called()
            return result

    def test_frozen_complete_bundle_accepted_without_launch(self):
        self.assertEqual(self.validate()["candidateId"], self.record["candidateId"])

    def test_wrong_schema_rejected(self):
        self.record["schema"] = "old naming"
        with self.assertRaisesRegex(RuntimeError, "SCHEMA_MISMATCH"):
            self.validate()

    def test_credentials_required_before_binary_or_launch(self):
        del self.record["credentialBundles"]
        with self.assertRaisesRegex(RuntimeError, "CREDENTIAL_BUNDLES_REQUIRED"):
            self.validate()

    def test_real_malformed_credentials_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "CREDENTIAL_IDENTITY_INVALID"):
            candidate.inspect_spec201_credentials(self.record["credentialBundles"])

    def test_actual_yolo_credential_fence_accepts_root_provider(self):
        source = ast.parse((ROOT / "Experiments/NDNSF_DI_Yolo2x2_Minindn.py").read_text())
        main = next(node for node in source.body
                    if isinstance(node, ast.FunctionDef) and node.name == "main")
        gate = next(node for node in main.body if isinstance(node, ast.If)
                    and any(isinstance(item, ast.Assign)
                            and any(isinstance(target, ast.Name) and target.id == "required_identities"
                                    for target in item.targets) for item in node.body))
        start = next(i for i, node in enumerate(gate.body) if isinstance(node, ast.Assign)
                     and any(isinstance(target, ast.Name) and target.id == "required_identities"
                             for target in node.targets))
        fragment = ast.Module(body=gate.body[start:start+3], type_ignores=[])
        root = "/NDNSF-DistributeInference/example"
        identities = [root + suffix for suffix in (
            "/controller", "/user", "/authority", "/provider", "/provider/D",
            "/provider/A", "/provider/B", "/provider/C")]
        environment = {"APP_ROOT": root, "CONTROLLER_IDENTITY": root + "/controller",
                       "USER_IDENTITY": root + "/user", "PROVIDER_PREFIX": root + "/provider",
                       "args_cli": SimpleNamespace(native_requester=True, single_rsa_certs=False),
                       "credentials": {"applicationRoot": root, "identities": identities,
                                       "keys": {identity: {"ec": "frozen"}
                                                for identity in identities}}}
        exec(compile(fragment, "actual-yolo-credential-fence", "exec"), environment)
        environment["credentials"]["identities"].append("/foreign/provider")
        with self.assertRaisesRegex(RuntimeError, "CREDENTIAL_IDENTITIES_MISMATCH"):
            exec(compile(fragment, "actual-yolo-credential-fence", "exec"), environment)

    def test_real_untrusted_actor_safebag_rejected(self):
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.backends import default_backend
        from ndn.app_support.security_v2 import new_cert, SELF_COMPONENT
        from ndn.security import Sha256WithRsaSigner
        from ndn.encoding import get_tl_num_size, write_tl_num
        from datetime import datetime, timedelta

        def credential(identity):
            private = rsa.generate_private_key(public_exponent=65537, key_size=1024,
                                              backend=default_backend())
            der = private.private_bytes(serialization.Encoding.DER,
                                        serialization.PrivateFormat.PKCS8,
                                        serialization.NoEncryption())
            public = private.public_key().public_bytes(
                serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
            now = datetime.utcnow()
            _, cert = new_cert(identity + "/KEY/test-key", SELF_COMPONENT, public,
                               Sha256WithRsaSigner(identity + "/KEY/test-key", der),
                               now - timedelta(days=1), now + timedelta(days=1))
            encrypted = private.private_bytes(serialization.Encoding.DER,
                                              serialization.PrivateFormat.PKCS8,
                                              serialization.BestAvailableEncryption(b"123456"))
            return bytes(cert), encrypted

        def tlv(typ, value):
            a, b = get_tl_num_size(typ), get_tl_num_size(len(value))
            wire = bytearray(a + b + len(value))
            write_tl_num(typ, wire)
            write_tl_num(len(value), wire, a)
            wire[a+b:] = value
            return bytes(wire)

        root, _ = credential("/actual")
        cert, encrypted = credential("/actual/producer")
        root_path, bag_path = self.root / "real-root.cert", self.root / "actor.safebag"
        root_path.write_bytes(root)
        bag_path.write_bytes(tlv(128, cert + tlv(129, encrypted)))
        bundles = self.record["credentialBundles"]
        bundles["yolo"]["rootCert"] = str(root_path)
        bundles["yolo"]["keys"]["/actual/producer"]["rsa"] = str(bag_path)
        with self.assertRaisesRegex(RuntimeError, "CREDENTIAL_IDENTITY_INVALID") as caught:
            candidate.inspect_spec201_credentials(bundles)
        self.assertIn("signer mismatch", str(caught.exception.__cause__))

    def test_unfrozen_private_key_rejected(self):
        path = self.root / "unfrozen-key"
        path.write_bytes(b"not frozen")
        self.record["credentialBundles"]["yolo"]["keys"]["/actual/producer"]["rsa"] = str(path)
        with self.assertRaisesRegex(RuntimeError, "CREDENTIAL_NOT_FROZEN"):
            self.validate()

    def test_changed_anchor_schema_rejected(self):
        schema = Path(self.record["credentialBundles"]["qwen"]["schema"])
        schema.write_text('trust-anchor { type file file-name "/another/root.cert" }\n')
        self.record["generatedSchemas"] = [candidate.file_identity(schema)]
        with self.assertRaisesRegex(RuntimeError, "CREDENTIAL_SCHEMA_MISMATCH"):
            self.validate()

    def test_declared_sync_prefix_requires_matching_frozen_schema(self):
        self.record["credentialBundles"]["yolo"]["syncPrefix"] = "/actual/group"
        with self.assertRaisesRegex(RuntimeError, "CREDENTIAL_SCHEMA_MISMATCH"):
            self.validate()

    def test_exact_member_signed_sync_schema_is_accepted(self):
        from native_probe_validation_links import render_anchored_probe_validation_schema
        bundle = self.record["credentialBundles"]["yolo"]
        bundle["syncPrefix"] = "/actual/group"
        schema = self.root / "sync-schema.conf"
        schema.write_text(render_anchored_probe_validation_schema(
            bundle["applicationRoot"], [bundle["rootCert"]], sync_prefix=bundle["syncPrefix"]))
        bundle["schema"] = str(schema)
        self.record["generatedSchemas"].append(candidate.file_identity(schema))
        self.validate()

    def test_changed_source_rejected_before_binary_or_launch(self):
        digest = self.freeze()
        self.asset.write_bytes(b"changed")
        with patch.object(candidate, "binary_check") as binary, \
                patch.object(candidate.subprocess, "run") as external:
            with self.assertRaisesRegex(RuntimeError, "FILE_CHANGED"):
                candidate.validate_spec201_candidate(self.manifest, digest, [])
            binary.assert_not_called()
            external.assert_not_called()

    def test_incomplete_family_rejected(self):
        del self.record["inventory"]["C04"]
        with self.assertRaisesRegex(RuntimeError, "FAMILY_COVERAGE_INCOMPLETE"):
            self.validate()

    def test_range_backend_rejected(self):
        self.record["validation"]["backend"] = "range-only"
        with self.assertRaisesRegex(RuntimeError, "VALIDATION_CONTRACT_MISMATCH"):
            self.validate()

    def test_native_failure_cannot_be_relabelled_pass(self):
        receipt = Path(self.record["validation"]["nativeReceipts"][0])
        value = json.loads(receipt.read_text())
        value["exit_status"] = 1
        receipt.write_text(json.dumps(value))
        self.record["oracles"] = [candidate.file_identity(Path(row["path"]))
                                  for row in self.record["oracles"]]
        with self.assertRaisesRegex(RuntimeError, "NATIVE_RECEIPT_FAILED"):
            self.validate()

    def test_zero_cases_and_assertions_rejected(self):
        log = Path(json.loads(Path(self.record["validation"]["nativeReceipts"][0]).read_text())["log"])
        log.write_text("0 test cases out of 0 passed\n0 assertions out of 0 passed\n")
        self.record["oracles"] = [candidate.file_identity(Path(row["path"]))
                                  for row in self.record["oracles"]]
        with self.assertRaisesRegex(RuntimeError, "NATIVE_LOG_FAILED"):
            self.validate()

    def test_partial_selector_rejected(self):
        receipt = Path(self.record["validation"]["nativeReceipts"][0])
        value = json.loads(receipt.read_text())
        value["command"][1] = "--run_test=unrelated"
        receipt.write_text(json.dumps(value))
        self.record["oracles"] = [candidate.file_identity(Path(row["path"]))
                                  for row in self.record["oracles"]]
        with self.assertRaisesRegex(RuntimeError, "NATIVE_SELECTOR_INCOMPLETE"):
            self.validate()

    def test_unlisted_launch_input_rejected_before_binary_or_launch(self):
        outside = self.root / "other-model.onnx"
        outside.write_bytes(b"different model")
        digest = self.freeze()
        with patch.object(candidate, "binary_check") as binary, \
                patch.object(candidate.subprocess, "run") as external:
            with self.assertRaisesRegex(RuntimeError, "LAUNCH_INPUT_NOT_FROZEN"):
                candidate.validate_spec201_candidate(self.manifest, digest, [], [outside])
            binary.assert_not_called()
            external.assert_not_called()

    def test_actual_launcher_preflight_fragments_reject_before_staging(self):
        self.record["schema"] = "wrong-schema"
        digest = self.freeze()
        for launcher, argument_name in (("NDNSF_DI_Yolo2x2_Minindn.py", "args_cli"),
                                         ("NDNSF_DI_Qwen06B_Native_Minindn.py", "args")):
            tree = ast.parse((ROOT / "Experiments" / launcher).read_text())
            main = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                        and node.name == "main")
            if argument_name == "args_cli":
                index = next(index for index, node in enumerate(main.body)
                             if isinstance(node, ast.Assign) and any(
                                 isinstance(target, ast.Name) and target.id == "naming_candidate"
                                 for target in node.targets))
                nodes = main.body[index:index + 2]
                staging = next(node for node in ast.walk(main) if isinstance(node, ast.Call)
                               and isinstance(node.func, ast.Attribute) and node.func.attr == "mkdir")
                self.assertLess(nodes[-1].end_lineno, staging.lineno)
            else:
                index = next(index for index, node in enumerate(main.body)
                             if isinstance(node, ast.Assign) and any(
                                 isinstance(target, ast.Name) and target.id == "args"
                                 for target in node.targets))
                nodes = main.body[index + 1:index + 3]
                guarded = next(node for node in ast.walk(main) if isinstance(node, ast.Call)
                               and isinstance(node.func, ast.Name) and node.func.id == "prepare_fixed_workspace")
                self.assertLess(nodes[-1].end_lineno, guarded.lineno)
            arguments = SimpleNamespace(native_providers=True, native_requester=False,
                native_library_path=str(self.root), spec201_candidate=self.manifest,
                spec201_candidate_sha256=digest, native_provider_binary=str(self.binary),
                native_assembly_worker_binary=str(self.binary),
                native_repo_binary=str(self.repo_binary), model=str(self.asset))
            import os
            namespace = {argument_name: arguments, "Path": Path, "os": os,
                         "validate_spec201_candidate": candidate.validate_spec201_candidate,
                         "REPO": ROOT, "PY_DIR": self.root, "TOPO": self.asset}
            with patch.dict(os.environ), patch.object(candidate.subprocess, "run") as external:
                with self.assertRaisesRegex(RuntimeError, "SCHEMA_MISMATCH"):
                    exec(compile(ast.Module(body=nodes, type_ignores=[]), launcher, "exec"), namespace)
                external.assert_not_called()


class RealPublicationInventoryTest(unittest.TestCase):
    def test_maintained_page_and_revocation_writers_have_real_source_anchors(self):
        from spec201_naming_inventory import naming_inventory
        inventory = naming_inventory(ROOT)
        for family, label, symbol in (
                ("C02", "Root/artifact/material manifest page, only if independently published", "FileSegmentedObjectProducer"),
                ("C08", "App revocation record", "revoke_activation")):
            rows = [row for row in inventory[family] if row["row"] == label]
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["publication"], "independent")
            self.assertIn(symbol, rows[0]["writer"])
            self.assertTrue(all(Path(path).is_file() for path in rows[0]["sourceFiles"]))
        self.assertTrue(any(row["row"] == "Inventory / capability / cache status / operation status"
                            for row in inventory["C09"]))


if __name__ == "__main__":
    unittest.main()
