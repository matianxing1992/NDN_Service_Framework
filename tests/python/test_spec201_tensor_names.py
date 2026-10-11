"""SDK configuration parity against the production C++ C04 wire oracle."""
import json
from dataclasses import replace
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from ndn.encoding import Name
from ndnsf_distributed_inference.sdk.placement import (
    DeviceBinding, DeviceBindingMode, ExecutionRole, ProviderSelectionProjectionV3,
    RoleAssemblySpec, RoleDataflowContract, TensorEndpoint, canonical_bytes,
)


class TensorNamingParity(unittest.TestCase):
    def test_actual_native_endpoint_and_typed_wire(self):
        root = Path(__file__).resolve().parents[2]
        binary = os.environ.get("SPEC201_TENSOR_NAMING_CPP", "/var/tmp/spec200-asan/spec189-epoch-projection")
        with tempfile.TemporaryDirectory(prefix="spec201-tensor-parity-") as directory:
            output = Path(directory) / "tensor.json"
            env = dict(os.environ, SPEC201_TENSOR_RECORDS=str(output))
            result = subprocess.run([binary, "--run_test=NativeTensorC04WireGoldensBindCompleteEndpointAndSignedStride"],
                cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout)
            record = json.loads(output.read_text())
        endpoint = TensorEndpoint(**record["endpoint"])
        self.assertEqual(endpoint.execution_object_digest, record["execution_object_digest"])
        for field, name in (("prefix_wire", endpoint.name_prefix),
                            ("manifest_wire", endpoint.manifest_name),
                            ("segment0_wire", endpoint.segment_name(0)),
                            ("segment256_wire", endpoint.segment_name(256))):
            self.assertEqual(bytes(Name.encode(Name.from_str(name))).hex(), record[field])
        # C++ also checks this complete independently frozen canonical commitment.
        self.assertEqual(endpoint.endpoint_digest,
            "sha256:1592bea674cefe6332119e31b6d04c42a2acd24512dbfeda48b2969c0f465a20")
        unsigned = dict(record["endpoint"])
        unsigned.pop("endpoint_digest")
        unsigned.pop("consumer_role")
        self.assertEqual(canonical_bytes(endpoint.unsigned_dict()), canonical_bytes(unsigned))
        self.assertEqual(json.loads(canonical_bytes(endpoint)), record["endpoint"])

        def assert_emitted_endpoint(value, expected):
            flow = RoleDataflowContract(value.request_id, value.attempt,
                value.plan_digest, value.producer_role, may_publish=(value,))
            assembly = RoleAssemblySpec("stage", value.producer_rank, 0, 1,
                value.layout_digest, value.tensor_digest, "onnxruntime-cpu")
            device = DeviceBinding(DeviceBindingMode.CPU, value.producer_namespace,
                value.producer_role, value.plan_digest, value.layout_digest,
                value.tensor_digest, 1)
            selection = ProviderSelectionProjectionV3(
                provider=value.producer_namespace, request_id=value.request_id,
                attempt=value.attempt, plan_core_digest=value.plan_digest,
                plan_digest=value.plan_digest, ack_closed_digest=value.plan_digest,
                offer_digest=device.offer_digest,
                security_policy_snapshot_digest=value.plan_digest,
                roles=(assembly,), dependencies=(), deadline_ms=1000,
                execution_role=ExecutionRole(value.producer_role, assembly.role,
                    assembly.rank, 0, 1, assembly.backend),
                assembly=assembly, dataflow=flow, device_binding=device)
            self.assertEqual(json.loads(flow.to_bytes())["may_publish"][0], expected)
            self.assertEqual(json.loads(selection.to_bytes())["dataflow"]["may_publish"][0], expected)

        assert_emitted_endpoint(endpoint, record["endpoint"])
        prefetched = replace(endpoint, prefetch_bundle_bytes=1,
            prefetch_segment_bytes=1, inline_first_segment=True, endpoint_digest="")
        expected = dict(record["endpoint"], prefetch_bundle_bytes=1,
            prefetch_segment_bytes=1, inline_first_segment=True,
            endpoint_digest=prefetched.endpoint_digest)
        assert_emitted_endpoint(prefetched, expected)


if __name__ == "__main__":
    unittest.main()
