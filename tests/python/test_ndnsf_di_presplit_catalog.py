from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile
import unittest

from ndnsf_distributed_inference.app_sdk.contracts import (
    PreSplitArtifactInput,
)
from ndnsf_distributed_inference.app_sdk.presplit import PreSplitCatalog
from ndnsf_distributed_inference.splitter import (
    AdapterDescriptor,
    GraphNodeView,
    ModelDescriptor,
    ModelGraphSnapshot,
    RoleExecutionPlan,
    RoleResourceRequirement,
    SplitArtifact,
    SplitCandidate,
    SplitServiceSpec,
    SplitSource,
    SplitterDescriptor,
    SplitterOutput,
    TensorContract,
)


def _digest(payload: bytes | str) -> str:
    if isinstance(payload, str):
        return "sha256:" + payload * 64
    return "sha256:" + hashlib.sha256(payload).hexdigest()


class _Repository:
    def __init__(self):
        self.events = []
        self.fail_activation = False

    def publish_segment(self, name, payload, digest):
        self.events.append(("segment", name, digest, bytes(payload)))

    def activate_manifest(self, name, payload, digest):
        self.events.append(("activate", name, digest, bytes(payload)))
        if self.fail_activation:
            raise RuntimeError("activation failed")

    def publish_revocation(self, alias, digest, status):
        self.events.append(("revocation", alias, digest, status))

    def delete_staging(self, name):
        self.events.append(("delete-staging", name))


class PreSplitCatalogTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.repo = _Repository()
        self.catalog = PreSplitCatalog(
            repository=self.repo,
            repository_prefix="/repo/models",
            verify_artifact=lambda artifact, payload:
                artifact.signer_key_id == "/operator/KEY/1"
                and artifact.signature == "artifact-signature",
            sign_manifest=lambda payload:
                ("/operator/KEY/1", "manifest-signature"),
            verify_manifest=lambda key_id, payload, signature:
                key_id == "/operator/KEY/1"
                and signature == "manifest-signature",
        )

    def tearDown(self):
        self.temp.cleanup()

    def fixture(self, payload=b"model-shard-a"):
        artifact_path = Path(self.temp.name) / (
            hashlib.sha256(payload).hexdigest() + ".onnx")
        artifact_path.write_bytes(payload)
        artifact_digest = _digest(payload)
        adapter = AdapterDescriptor(
            name="generic-onnx",
            version="1",
            state_digest=_digest("a"),
            abi="ndnsf-di-adapter-v1",
            model_formats=("onnx",),
            tasks=("opaque",),
            backends=("onnxruntime",),
            precisions=("fp32",),
            input_schema_digest=_digest("1"),
            options_schema_digest=_digest("2"),
            result_schema_digest=_digest("3"),
            graph_schema_digest=_digest("4"),
            split_schema_digest=_digest("5"),
            state_schema_digest=_digest("6"),
            graph_inspectable=True,
            splittable=True,
        )
        graph = ModelGraphSnapshot(
            graph_digest=_digest("d"),
            adapter=adapter,
            nodes=(GraphNodeView("n0", "Opaque"),),
            edges=(),
            topological_order=("n0",),
            legal_cut_edges=(),
            model_inputs=(
                TensorContract("input", "bytes", ("N",), None),),
            model_outputs=(
                TensorContract("output", "bytes", ("N",), None),),
        )
        model = ModelDescriptor(
            model_name="example/opaque",
            content_digest=_digest("b"),
            semantics_digest=_digest("c"),
            graph_digest=graph.graph_digest,
            model_format="onnx",
            precision="fp32",
            adapter=adapter,
        )
        candidate = SplitCandidate(
            source=SplitSource.PRE_SPLIT,
            splitter=SplitterDescriptor(
                "operator-import", "1", _digest("e")),
            model=model,
            graph_digest=graph.graph_digest,
            execution_plan=RoleExecutionPlan(
                roles=("stage-0",),
                dependencies=(),
                node_roles={"n0": "stage-0"},
            ),
            fragments_by_role={"stage-0": _digest("f")},
            artifacts_by_role={"stage-0": (artifact_digest,)},
            requirements_by_role={
                "stage-0": RoleResourceRequirement(
                    ("onnxruntime",), len(payload), 0, 0, 0, 0),
            },
            cross_partition_tensors=(),
            estimated_costs={"repository_bytes": len(payload)},
        )
        split_artifact = SplitArtifact(
            role="stage-0",
            path=str(artifact_path),
            artifact_name="/model/stage-0",
            backend="onnxruntime",
        )
        output = SplitterOutput(
            application="/app",
            controller="/controller",
            group="/group",
            user="/user",
            provider_prefix="/provider",
            services=[SplitServiceSpec(
                name="/inference",
                model_name=model.model_name,
                roles=["stage-0"],
                dependencies=[],
                artifacts=[split_artifact],
            )],
        )
        artifact = PreSplitArtifactInput(
            role="stage-0",
            path=str(artifact_path),
            artifact_name="/model/stage-0",
            digest=artifact_digest,
            size_bytes=len(payload),
            signer_key_id="/operator/KEY/1",
            signature="artifact-signature",
        )
        return output, graph, candidate, artifact

    def register(self, alias="opaque-fp32", payload=b"model-shard-a", at_ms=100):
        output, graph, candidate, artifact = self.fixture(payload)
        return self.catalog.register(
            alias=alias,
            splitter_output=output,
            graph=graph,
            candidate=candidate,
            artifacts=(artifact,),
            backend="onnxruntime",
            precision="fp32",
            at_ms=at_ms,
        )

    def test_content_addressed_activation_snapshot_and_idempotency(self):
        first = self.register()
        self.assertEqual(first.status, "ACTIVE")
        self.assertEqual(len(self.repo.events), 2)
        self.assertEqual(self.catalog.snapshot(), (first,))
        self.assertNotIn("provider", repr(first).lower())
        self.assertNotIn("residency", repr(first).lower())
        with self.assertRaises(TypeError):
            first.artifact_data_names["stage-0"] = ("changed",)

        self.assertIs(self.register(), first)
        self.assertEqual(len(self.repo.events), 2)
        second_alias = self.register(alias="opaque-lab-alias")
        self.assertEqual(second_alias.manifest_digest, first.manifest_digest)
        self.assertEqual(len(self.repo.events), 2)

        with self.assertRaisesRegex(ValueError, "alias"):
            self.register(payload=b"different-shard")

    def test_trust_failure_and_partial_activation_fail_closed_then_cleanup(self):
        output, graph, candidate, artifact = self.fixture()
        forged = PreSplitArtifactInput(
            **{**artifact.__dict__, "signature": "forged"})
        with self.assertRaises(PermissionError):
            self.catalog.register(
                alias="forged",
                splitter_output=output,
                graph=graph,
                candidate=candidate,
                artifacts=(forged,),
                backend="onnxruntime",
                precision="fp32",
                at_ms=100,
            )
        self.assertEqual(self.catalog.snapshot(), ())

        self.repo.fail_activation = True
        with self.assertRaisesRegex(RuntimeError, "activation"):
            self.register(alias="partial", at_ms=100)
        self.assertEqual(self.catalog.snapshot(), ())
        self.assertEqual(
            self.catalog.cleanup_staging(now_ms=149, max_age_ms=50), ())
        cleaned = self.catalog.cleanup_staging(
            now_ms=150, max_age_ms=50)
        self.assertEqual(len(cleaned), 1)
        self.assertTrue(any(
            event[0] == "delete-staging" for event in self.repo.events))

    def test_retire_or_revoke_propagates_before_new_snapshot(self):
        active = self.register()
        retired = self.catalog.retire(
            active.alias, revoke=True, at_ms=200)
        self.assertEqual(retired.status, "REVOKED")
        self.assertEqual(self.repo.events[-1][0], "revocation")
        self.assertEqual(self.catalog.snapshot(), ())
        events = self.catalog.audit_evidence()
        self.assertEqual(
            [event.event for event in events], ["ACTIVATED", "REVOKED"])
        with self.assertRaises(TypeError):
            events[-1].evidence["changed"] = True


if __name__ == "__main__":
    unittest.main()
