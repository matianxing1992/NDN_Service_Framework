"""SDK caller wiring with mock metadata; native name checks use Core oracle."""
from types import SimpleNamespace
import hashlib
from concurrent.futures import Future
import tempfile
import unittest
from unittest import mock

from ndnsf import parse_large_data_reference_payload
from ndnsf_distributed_inference.artifact_deployment import (
    ExecutionArtifactSpec, publish_execution_artifact_spec,
    _artifact_from_repo_spec, _fetch_repo_manifest_payload)
from ndnsf_distributed_inference.client import DistributedInferenceClient, _artifact_fingerprint
from ndnsf_distributed_inference.app_sdk.facades import APPClient, InferencePlanBuilder
from ndnsf_distributed_inference.app_sdk.placement import AutomaticPlanningCoordinator
from spec201_publication_fixture import publication_result
from ndnsf_distributed_inference.naming import canonical_name_uri, parse_producer_data_name
from ndnsf_distributed_inference.plan import ArtifactSpec
from ndnsf_distributed_inference.app_sdk.client import APPClient as DurableAPPClient
from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal


def model_artifact(payload, filename, model_name="model"):
    return ArtifactSpec("model", payload, filename, content_name={
        "kind": "model-file", "label": "canonical-source",
        "model_name": model_name, "model_identity": "a" * 64,
    }).to_ndnsf_artifact()


class User:
    def __init__(self, legacy=False):
        self.calls = []
        self.options = []
        self.legacy = legacy

    def publish_encrypted_large_data(self, service, payload, **kwargs):
        self.options.append(kwargs)
        result = publication_result(payload, service, label=kwargs["object_label"],
                                    content_name=kwargs.get("content_name"))
        if self.legacy:
            result.manifest_name = ""
        self.calls.append((bytes(payload), result))
        return result


class ManifestCallerTest(unittest.TestCase):
    def test_replica_fetch_keeps_original_producer_name_and_only_changes_hint(self):
        payload = b"original payload"
        original = publication_result(payload, "/service", producer="/actual/publisher").data_prefix
        manifest = {"objectName": "/local/logical-object", "size": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                    "replicaNodes": ["/repo/one", "/repo/two"],
                    "replicaDataNames": [original, original]}
        with mock.patch("ndnsf_distributed_inference.artifact_deployment.fetch_segmented_object",
                        side_effect=[RuntimeError("first replica unavailable"), payload]) as fetch:
            self.assertEqual(_fetch_repo_manifest_payload(manifest), payload)
            self.assertEqual([call.args[0] for call in fetch.call_args_list], [original, original])
            self.assertEqual([call.kwargs["forwarding_hints"] for call in fetch.call_args_list],
                             [["/repo/one"], ["/repo/two"]])
        artifact = _artifact_from_repo_spec("model", {"filename": "model.bin", "repoManifest": manifest})
        self.assertEqual(artifact.large_data_reference["dataName"], original)
        self.assertEqual(artifact.repo_manifest["objectName"], "/local/logical-object")

    def test_missing_replica_names_fail_before_fetch_and_never_invent_alias(self):
        manifest = {"objectName": "/local/logical-object", "size": 1, "sha256": "a" * 64,
                    "replicaNodes": ["/repo/one", "/repo/two"]}
        for names in ([], ["/actual/data"], ["/actual/data", ""], ["/actual/data", None]):
            with self.subTest(names=names), \
                    mock.patch("ndnsf_distributed_inference.artifact_deployment.fetch_segmented_object") as fetch:
                with self.assertRaisesRegex(ValueError, "original Producer"):
                    _fetch_repo_manifest_payload(dict(manifest, replicaDataNames=names))
                fetch.assert_not_called()
        with self.assertRaisesRegex(ValueError, "original Producer"):
            _artifact_from_repo_spec("model", {"filename": "model.bin", "repoManifest": manifest})

    def test_segment_reference_preserves_exact_original_version_name(self):
        payload = b"original payload"
        original = publication_result(payload, "/service", producer="/actual/publisher").data_prefix
        manifest = {"objectName": "/local/logical-object", "size": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                    "segmentLocations": [{"dataName": "/actual/publisher/base",
                                          "versionedDataName": original}]}
        artifact = _artifact_from_repo_spec("model", {"filename": "model.bin", "repoManifest": manifest})
        self.assertEqual(artifact.large_data_reference["dataName"], original)

    def test_service_facade_requires_and_uses_explicit_local_model_context(self):
        artifact = SimpleNamespace(role="worker", path="/does-not-exist",
                                   artifact_name="model", filename="model.bin", kind="model",
                                   backend="onnx", metadata={})
        policy = SimpleNamespace(name="/inference", model_name="model",
                                 artifacts=(artifact,), dependencies=())
        facade = APPClient.__new__(APPClient)
        facade.deployment = SimpleNamespace(service_policy=lambda service: policy)
        facade._service_runtimes = {}
        with self.assertRaisesRegex(ValueError, "explicit ArtifactSpec"):
            facade.service_plan("/inference")
        owned = ArtifactSpec("model", b"abc", "model.bin",
                             content_name=model_artifact(b"x", "model.bin")["content_name"])
        local = {"worker": owned}
        plan = facade.service_plan("/inference", local_artifacts=local)
        self.assertEqual(plan.roles[0].model_artifact.payload, b"abc")
        self.assertEqual(dict(plan.roles[0].model_artifact.content_name), dict(owned.content_name))
        runner_only = facade.service_plan(
            "/inference", local_artifacts=local,
            artifact_references={"worker": {"runner": {}}})
        self.assertEqual(runner_only.roles[0].model_artifact.payload, b"abc")
        wrong = ArtifactSpec("model", b"abc", "model.bin",
                             content_name=model_artifact(b"x", "model.bin", "wrong-model")["content_name"])
        with self.assertRaisesRegex(ValueError, "incompatible model context"):
            facade.service_plan("/inference", local_artifacts={"worker": wrong})
        user = User()
        client = DistributedInferenceClient.__new__(DistributedInferenceClient)
        client.user = user
        client.publish_plan_artifacts(plan)
        self.assertEqual(user.options[0]["content_name"], dict(owned.content_name))
        for method, endpoint in (("distributed_inference", "infer"),
                                 ("async_distributed_inference", "infer_async")):
            with mock.patch.object(facade, "encode_input", return_value=b"input"), \
                    mock.patch.object(facade, endpoint, return_value="submitted") as invoke:
                self.assertEqual(getattr(facade, method)(
                    "/inference", b"input", local_artifacts=local), "submitted")
                self.assertEqual(invoke.call_args[0][0].roles[0].model_artifact.payload, b"abc")
        def submit_network(plan, payload, **options):
            self.assertEqual(plan.roles[0].model_artifact.payload, owned.payload)
            self.assertEqual(dict(plan.roles[0].model_artifact.content_name), dict(owned.content_name))
            result = SimpleNamespace(
                status=True, payload=b"result", error="", request_id=options["request_id"],
                data_name="/provider/NDNSF/RESPONSE/result", signer_certificate="/provider/KEY/cert",
                wire_digest="sha256:" + "b" * 64)
            options["on_result"](result)
            future = Future()
            future.set_result(result)
            return future
        with tempfile.TemporaryDirectory() as root, \
                mock.patch.object(facade, "encode_input", return_value=b"input"), \
                mock.patch.object(facade, "infer_async", side_effect=submit_network):
            public = DurableAPPClient(RuntimeJournal.for_test(root, "requester"),
                                      network_client=facade, requester_identity="/requester")
            for method in ("distributed_inference", "async_distributed_inference"):
                result = getattr(public, method)(
                    "/inference", b"input", deployment_revision="rev", local_artifacts=local)
                if isinstance(result, Future):
                    result = result.result(timeout=1)
                self.assertTrue(result.status)

    def test_application_builder_forwards_owned_model_metadata_to_publisher(self):
        for grid in (False, True):
            user = User()
            client = DistributedInferenceClient.__new__(DistributedInferenceClient)
            client.user = user
            fields = model_artifact(b"x", "model.bin")["content_name"]
            builder = InferencePlanBuilder(service="/inference", model_name="model")
            options = dict(model=b"abc", filename="model.bin", content_name=fields)
            if grid:
                builder.add_grid_part(stage=0, shard=0, **options)
            else:
                builder.add_part(role="worker", **options)
            fields["model_name"] = "changed-after-admission"
            plan = builder.build()
            self.assertEqual(plan.roles[0].model_artifact.content_name["model_name"], "model")
            client.publish_plan_artifacts(plan)
            self.assertEqual(user.calls[0][0], b"abc")
            self.assertEqual(user.options[0]["content_name"]["model_name"], "model")
            spec = ExecutionArtifactSpec.from_bytes(user.calls[-1][0])
            self.assertEqual(spec.artifacts[0].data_name, user.calls[0][1].manifest_name)

    def test_application_builder_rejects_bad_semantic_fields(self):
        builder = InferencePlanBuilder(service="/inference", model_name="model")
        builder.add_part(role="worker", model=b"abc",
                         content_name={"kind": "model-file", "label": "model"})
        with self.assertRaises(ValueError):
            builder.build()

    def test_service_manifest_passes_material_identity_to_core(self):
        user = User()
        payload = b"service deployment descriptor"
        facade = SimpleNamespace(
            _client=SimpleNamespace(user=user),
            deployment=SimpleNamespace(
                services=[SimpleNamespace(name="/inference")],
                service_manifest_payload=lambda: payload))
        result = APPClient.publish_service_manifest(facade)
        self.assertEqual(user.options[0]["content_name"],
                         {"kind": "material", "label": "service-manifest"})
        self.assertEqual(user.calls[0][0], payload)
        parsed = parse_producer_data_name(result.data_prefix, "/encrypted")
        self.assertIsNotNone(parsed)
        self.assertEqual((parsed.name_space, parsed.category), ("NDNSF-DI", "material"))

    def test_whole_files_use_core_segmentation_and_exact_manifests(self):
        user = User()
        result = publish_execution_artifact_spec(
            user, "/inference", role="worker", backend="onnx",
            artifacts={"large": model_artifact(b"abcdefg", "large.bin"),
                       "small": model_artifact(b"x", "small.bin")},
            max_artifact_chunk_size=3)
        spec = ExecutionArtifactSpec.from_bytes(user.calls[-1][0])
        self.assertIs(result, user.calls[-1][1])
        self.assertEqual(user.options[-1]["content_name"],
                         {"kind": "material", "label": "execution-worker-spec"})
        parsed = parse_producer_data_name(result.data_prefix, "/encrypted")
        self.assertEqual((parsed.name_space, parsed.category), ("NDNSF-DI", "material"))
        self.assertEqual(len(user.calls), 3)
        self.assertEqual(user.calls[0][0], b"abcdefg")
        self.assertEqual(spec.artifacts[0].chunks, [])
        self.assertEqual(spec.artifacts[0].data_name, user.calls[0][1].manifest_name)
        self.assertEqual(spec.artifacts[1].data_name, user.calls[1][1].manifest_name)
        self.assertEqual(spec.artifacts[1].large_data_reference["dataName"],
                         user.calls[1][1].manifest_name)
        self.assertEqual(user.options[0]["content_name"]["kind"], "model-file")

    def test_invalid_later_model_prevents_all_publication(self):
        for invalid in ((b"x", "model.bin", "model"),
                        {**model_artifact(b"x", "model.bin"),
                         "content_name": {"kind": "material", "label": "x"}}):
            user = User()
            with self.assertRaises(ValueError):
                publish_execution_artifact_spec(
                    user, "/inference", role="worker", backend="onnx",
                    artifacts={"config": (b"c", "config.json", "config"), "model": invalid})
            self.assertEqual(user.calls, [])
            self.assertEqual(user.options, [])

    def test_artifact_metadata_and_payload_are_owned(self):
        payload = bytearray(b"abc")
        fields = model_artifact(b"x", "model.bin")["content_name"]
        artifact = ArtifactSpec("model", payload, "model.bin", content_name=fields)
        payload[0] = 0
        fields["model_name"] = "other"
        self.assertEqual(artifact.payload, b"abc")
        self.assertEqual(artifact.content_name["model_name"], "model")
        with self.assertRaises(TypeError):
            artifact.content_name["model_name"] = "other"
        copied = artifact.to_ndnsf_artifact()
        copied["content_name"]["model_name"] = "other"
        self.assertEqual(artifact.content_name["model_name"], "model")

    def test_fingerprint_distinguishes_semantic_context_for_identical_bytes(self):
        fields = model_artifact(b"x", "model.bin")["content_name"]
        first = ArtifactSpec("model", b"same", "model.bin", content_name=fields)
        second = ArtifactSpec("model", b"same", "model.bin",
                              content_name={**fields, "model_identity": "b" * 64})
        self.assertNotEqual(_artifact_fingerprint(first), _artifact_fingerprint(second))
        layer = {**fields, "kind": "artifact-file", "artifact_kind": "stage",
                 "coordinate": "layers=0-1", "profile_identity": "c" * 64}
        first = ArtifactSpec("model", b"same", "model.bin", content_name=layer)
        for changes in ({"coordinate": "layers=2-3"}, {"profile_identity": "d" * 64}):
            second = ArtifactSpec("model", b"same", "model.bin", content_name={**layer, **changes})
            self.assertNotEqual(_artifact_fingerprint(first), _artifact_fingerprint(second))
    def test_client_and_placement_scope_keys_use_exact_manifests(self):
        user = User()
        client = DistributedInferenceClient.__new__(DistributedInferenceClient)
        client.user = user
        names = client.publish_scope_keys_for_scopes("/inference", {"one": ["worker"]})
        self.assertEqual(names["one"], user.calls[-1][1].manifest_name)
        plan = SimpleNamespace(service="/inference", key_scopes=lambda: ("two",))
        names = client.publish_scope_keys(plan)
        self.assertEqual(names["two"], user.calls[-1][1].manifest_name)
        coordinator = SimpleNamespace(service_user=user, service_name="/inference")
        names = AutomaticPlanningCoordinator._publish_scope_keys(
            coordinator, {"three": ("worker",)}, deadline_ms=2**62)
        self.assertEqual(names["three"], user.calls[-1][1].manifest_name)

    def test_client_plan_and_facade_input_use_exact_manifests(self):
        user = User()
        client = DistributedInferenceClient.__new__(DistributedInferenceClient)
        client.user = user
        role = SimpleNamespace(
            role="worker", backend="onnx", metadata={},
            runtime=SimpleNamespace(name="onnx", entrypoint="main"),
            artifacts=lambda: {"model": model_artifact(b"abc", "model.bin")})
        plan = SimpleNamespace(service="/inference", roles=(role,), model_name="model", metadata={})
        self.assertEqual(client.publish_plan_artifacts(plan)["worker"],
                         user.calls[-1][1].manifest_name)
        self.assertEqual(user.options[0]["content_name"]["model_name"], plan.model_name)
        user.calls.clear()
        user.options.clear()
        plan.model_name = "different-model"
        plan.metadata = {"model": "model"}
        role.metadata = {"model": "model"}
        with self.assertRaisesRegex(ValueError, "expected model"):
            client.publish_plan_artifacts(plan)
        self.assertEqual(user.calls, [])
        result = publication_result(b"input", "/inference")
        actual_facade = SimpleNamespace(_client=SimpleNamespace(user=user))
        APPClient.publish_large_payload(actual_facade, "/inference", b"input",
                                       object_label="image")
        self.assertEqual(user.options[-1]["content_name"], {
            "kind": "input", "label": "image", "input_kind": "application"})
        facade = SimpleNamespace(publish_large_payload=lambda *args, **kwargs: result)
        wire = APPClient.publish_large_payload_reference(facade, "/inference", b"input")
        self.assertEqual(canonical_name_uri(parse_large_data_reference_payload(wire).data_name),
                         canonical_name_uri(result.manifest_name))

    def test_incomplete_binding_metadata_does_not_fall_back_to_data_prefix(self):
        user = User(legacy=True)
        client = DistributedInferenceClient.__new__(DistributedInferenceClient)
        client.user = user
        with self.assertRaisesRegex(ValueError, "canonical manifest"):
            client.publish_scope_keys_for_scopes("/inference", {"one": ["worker"]})
        with self.assertRaisesRegex(ValueError, "canonical manifest"):
            publish_execution_artifact_spec(user, "/inference", role="worker", backend="onnx",
                                            artifacts={"model": model_artifact(b"x", "model.bin")})


if __name__ == "__main__":
    unittest.main()
