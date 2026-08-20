from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceBinding, DeviceBindingMode, ExecutionRole, ReadinessMode,
    ReadinessPredicate, RoleDataflowContract, TensorEndpoint,
    TensorEndpointSource, TensorObjectManifestV1,
    validate_role_dataflow_contracts,
)
from ndnsf_distributed_inference.core.decision_validation import (  # noqa: E402
    validate_one_to_one_role_provider,
)
from ndnsf_distributed_inference.core.hybrid_contracts import (  # noqa: E402
    validate_role_dataflow_contracts as validate_core_role_dataflow_contracts,
)


def digest(label: str) -> str:
    import hashlib
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


PLAN = digest("plan")


def endpoint(producer: str = "S0R0", consumer: str = "S1R0",
             *, plan: str = PLAN, tensor: str = "hidden",
             consumers: tuple[str, ...] = ()) -> TensorEndpoint:
    return TensorEndpoint(
        producer_namespace="/provider/a",
        requester="/requester/app",
        request_id="request-001",
        attempt=1,
        plan_digest=plan,
        group_id="pipeline-0-1",
        group_epoch="epoch-1",
        operation="PIPELINE",
        round=0,
        source_kind=TensorEndpointSource.ROLE,
        producer_role=producer,
        producer_rank=0,
        consumer_role=consumer,
        tensor_id=tensor,
        tensor_digest=digest(tensor),
        layout_digest=digest("layout:" + tensor),
        microbatch=0,
        segment_count=3,
        manifest_digest=digest("manifest:" + tensor),
        security_profile="NDNSF_DATA_V1",
        no_progress_deadline_ms=500,
        hard_deadline_ms=5000,
        consumer_roles=consumers,
    )


def roles() -> tuple[ExecutionRole, ...]:
    return (
        ExecutionRole("S0R0", "S0", 0, 0, 2, "onnxruntime-cpu"),
        ExecutionRole("S1R0", "S1", 0, 2, 4, "onnxruntime-cpu"),
    )


def contracts(edge: TensorEndpoint | None = None):
    edge = edge or endpoint()
    return (
        RoleDataflowContract(
            request_id="request-001", attempt=1, plan_digest=PLAN,
            role="S0R0", may_publish=(edge,), terminal_response_owner=False,
        ),
        RoleDataflowContract(
            request_id="request-001", attempt=1, plan_digest=PLAN,
            role="S1R0", must_fetch=(edge,),
            wait_for=(ReadinessPredicate(
                ReadinessMode.ALL, (edge.endpoint_digest,)),),
            terminal_response_owner=True,
        ),
    )


class Spec170RoleDataflowContractTest(unittest.TestCase):
    def test_core_owns_v3_graph_and_one_to_one_validation(self):
        self.assertIs(
            validate_role_dataflow_contracts,
            validate_core_role_dataflow_contracts,
        )
        validate_one_to_one_role_provider(
            {"S0R0": "/provider/a", "S1R0": "/provider/b"},
            expected_roles=("S0R0", "S1R0"),
        )
        with self.assertRaisesRegex(ValueError, "one-to-one"):
            validate_one_to_one_role_provider(
                {"S0R0": "/provider/a", "S1R0": "/provider/a"},
                expected_roles=("S0R0", "S1R0"),
            )
        with self.assertRaisesRegex(ValueError, "cover each role"):
            validate_one_to_one_role_provider(
                {"S0R0": "/provider/a"},
                expected_roles=("S0R0", "S1R0"),
            )

    def test_contract_round_trip_and_exact_segment_names(self):
        producer, consumer = contracts()
        validate_role_dataflow_contracts(roles(), (producer, consumer))

        decoded = RoleDataflowContract.from_bytes(consumer.to_bytes())
        self.assertEqual(decoded, consumer)
        edge = consumer.must_fetch[0]
        self.assertTrue(edge.name_template.startswith(
            "/provider/a/NDNSF-DI/TENSOR/v1/REQUESTER/"))
        self.assertTrue(edge.manifest_name.endswith("/MANIFEST"))
        self.assertTrue(edge.segment_name(0).endswith("/SEG/seg=0"))
        self.assertEqual(edge.segment_name(1), edge.segment_name(1))
        self.assertNotEqual(edge.segment_name(0), edge.segment_name(1))
        with self.assertRaisesRegex(ValueError, "segment"):
            edge.segment_name(3)

    def test_tensor_object_manifest_round_trip_binds_concrete_object(self):
        edge = endpoint()
        manifest = TensorObjectManifestV1(
            capability_digest=digest("capability"),
            epoch_key_id="epoch-key-1",
            requester=edge.requester,
            request_id=edge.request_id,
            attempt_id=str(edge.attempt),
            plan_digest=edge.plan_digest,
            group_id=edge.group_id,
            epoch=edge.group_epoch,
            operation_index=0,
            round=edge.round,
            operation_kind=edge.operation,
            producer_role=edge.producer_role,
            producer_rank=edge.producer_rank,
            consumer_roles=(edge.consumer_role,),
            microbatch=edge.microbatch,
            source_layout_digest=edge.layout_digest,
            target_layout_digest=edge.layout_digest,
            tensor_id=edge.tensor_id,
            tensor_digest=edge.tensor_digest,
            content_digest=digest("concrete-output"),
            total_bytes=21,
            segment_size=7,
            segment_count=edge.segment_count,
            ordered_segment_digests=tuple(
                digest(f"ciphertext-{index}")
                for index in range(edge.segment_count)),
            created_at_ms=1,
            no_progress_ms=edge.no_progress_deadline_ms,
            hard_deadline_ms=edge.hard_deadline_ms,
            endpoint_digest=edge.endpoint_digest,
            manifest_contract_digest=edge.manifest_digest,
            producer_signature="identity-signature",
        )
        decoded = TensorObjectManifestV1.from_bytes(manifest.to_bytes())
        self.assertEqual(decoded, manifest)
        decoded.validate_endpoint(edge)
        self.assertEqual(decoded.object_manifest_digest, manifest.object_manifest_digest)

        with self.assertRaisesRegex(ValueError, "endpoint mismatch"):
            replace(decoded, request_id="other", object_manifest_digest="").validate_endpoint(edge)

    def test_one_tensor_object_is_shared_by_all_declared_consumers(self):
        consumers = ("S1R0", "S1R1")
        first = endpoint(consumer="S1R0", consumers=consumers)
        second = endpoint(consumer="S1R1", consumers=consumers)
        self.assertEqual(first.endpoint_digest, second.endpoint_digest)
        self.assertEqual(first.manifest_name, second.manifest_name)
        self.assertEqual(first.segment_name(0), second.segment_name(0))
        self.assertEqual(first.consumer_roles, consumers)
        with self.assertRaisesRegex(ValueError, "invalid tensor endpoint"):
            replace(first, consumer_role="S2R0", endpoint_digest="")

    def test_device_binding_is_cpu_or_exactly_one_device(self):
        cpu = DeviceBinding(
            DeviceBindingMode.CPU, "/provider/a", "S0R0",
            digest("offer"), digest("topology"), digest("resources"), 1,
        )
        self.assertEqual(cpu.mode, DeviceBindingMode.CPU)
        device = replace(
            cpu, mode=DeviceBindingMode.SINGLE_DEVICE,
            offer_scoped_device_handle="gpu-uuid-0")
        self.assertEqual(device.offer_scoped_device_handle, "gpu-uuid-0")
        with self.assertRaisesRegex(ValueError, "accelerator handle"):
            replace(cpu, offer_scoped_device_handle="cuda:0")
        with self.assertRaisesRegex(ValueError, "requires one offer handle"):
            replace(cpu, mode=DeviceBindingMode.SINGLE_DEVICE)

    def test_wrong_plan_and_missing_producer_fail_closed(self):
        edge = endpoint(plan=digest("wrong-plan"))
        with self.assertRaisesRegex(ValueError, "plan/attempt"):
            RoleDataflowContract(
                request_id="request-001", attempt=1, plan_digest=PLAN,
                role="S1R0", must_fetch=(edge,), terminal_response_owner=True,
            )

        producer, consumer = contracts()
        with self.assertRaisesRegex(ValueError, "matching mayPublish"):
            validate_role_dataflow_contracts(roles(), (
                replace(producer, may_publish=(), dataflow_digest=""), consumer))

    def test_cycle_and_multiple_terminal_owners_fail_closed(self):
        forward = endpoint("S0R0", "S1R0", tensor="forward")
        reverse = endpoint("S1R0", "S0R0", tensor="reverse")
        first = RoleDataflowContract(
            request_id="request-001", attempt=1, plan_digest=PLAN,
            role="S0R0", may_publish=(forward,), must_fetch=(reverse,),
            wait_for=(ReadinessPredicate(
                ReadinessMode.ALL, (reverse.endpoint_digest,)),),
            terminal_response_owner=False,
        )
        second = RoleDataflowContract(
            request_id="request-001", attempt=1, plan_digest=PLAN,
            role="S1R0", may_publish=(reverse,), must_fetch=(forward,),
            wait_for=(ReadinessPredicate(
                ReadinessMode.ALL, (forward.endpoint_digest,)),),
            terminal_response_owner=True,
        )
        with self.assertRaisesRegex(ValueError, "cycle"):
            validate_role_dataflow_contracts(roles(), (first, second))

        producer, consumer = contracts()
        with self.assertRaisesRegex(ValueError, "exactly one"):
            validate_role_dataflow_contracts(
                roles(), (replace(
                    producer, terminal_response_owner=True,
                    dataflow_digest=""), consumer))


if __name__ == "__main__":
    unittest.main()
