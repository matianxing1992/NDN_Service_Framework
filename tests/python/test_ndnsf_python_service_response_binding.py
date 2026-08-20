from __future__ import annotations

from pathlib import Path
import sys
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "pythonWrapper"))

from ndnsf import _ndnsf  # noqa: E402
from ndnsf.service import _from_native_response, _to_native_response, ServiceResponse  # noqa: E402


class NativeServiceResponseBindingTest(unittest.TestCase):
    def test_request_id_round_trips_across_pybind_boundary(self) -> None:
        native = _to_native_response(ServiceResponse(
            status=True, payload=b"ok", error="", request_id="/request/42"))
        self.assertTrue(hasattr(native, "request_id"))
        self.assertEqual(native.request_id, "/request/42")
        restored = _from_native_response(native)
        self.assertEqual(restored.request_id, "/request/42")
        self.assertEqual(restored.payload, b"ok")

    def test_native_binding_exports_request_id(self) -> None:
        self.assertIn("request_id", dir(_ndnsf.ServiceResponse))

    def test_authenticated_transport_evidence_round_trips_across_pybind_boundary(self) -> None:
        native = _ndnsf.ServiceResponse()
        native.status = True
        native.payload = b"lease"
        native.data_name = "/provider/NDNSF/RESPONSE/request"
        native.signer_certificate = "/provider/KEY/key/issuer/version"
        native.wire_digest = "sha256:abc"
        response = _from_native_response(native)
        self.assertEqual(response.data_name, native.data_name)
        self.assertEqual(response.signer_certificate, native.signer_certificate)
        self.assertEqual(response.wire_digest, native.wire_digest)
        rebuilt = _to_native_response(response)
        self.assertEqual(rebuilt.data_name, native.data_name)
        self.assertEqual(rebuilt.signer_certificate, native.signer_certificate)
        self.assertEqual(rebuilt.wire_digest, native.wire_digest)

    def test_collaboration_callbacks_export_authenticated_transport_evidence(self) -> None:
        """Keep sync and async collaboration paths aligned with normal responses.

        The end-to-end MiniNDN gate proves that these values originate from a
        validated Data packet.  This focused source contract prevents the
        pybind adapters from silently dropping that evidence again.
        """
        source = (REPO / "pythonWrapper/src/ndnsf/_ndnsf.cpp").read_text()
        collaboration = source[source.index("requestCollaboration("):
                               source.index("requestServiceSelect(")]
        for field, getter in (
            ("output.dataName", "response.getDataName()"),
            ("output.signerCertificate", "response.getSignerCertificate()"),
            ("output.wireDigest", "response.getWireDigest()"),
        ):
            self.assertGreaterEqual(
                collaboration.count(f"{field} = {getter};"),
                2,
                f"sync and async collaboration callbacks must copy {field}",
            )

    def test_multi_provider_role_selection_carries_complete_provider_mapping(self) -> None:
        """Per-provider Selection projections must preserve dependency routing."""
        source = (REPO / "pythonWrapper/src/ndnsf/_ndnsf.cpp").read_text()
        selector = source[source.index("class RoleAssignmentSelectionPolicy"):
                          source.index("class NativeServiceUser")]
        self.assertIn("selectedRoleProviders", selector)
        self.assertIn('"roleProvider."', selector)
        self.assertIn("participant.assignmentPayload.insert", selector)

    def test_sync_request_does_not_capture_submit_local_callbacks_by_reference(self) -> None:
        """The native runtime retains callbacks after the submit lambda returns."""
        source = (REPO / "pythonWrapper/src/ndnsf/_ndnsf.cpp").read_text()
        request_service = source[source.index("PyServiceResponse\n  requestService("):
                                 source.index("PyServiceResponse\n  requestServiceTargeted(")]
        self.assertIn(
            "timeoutMs, onResponse, onTimeout,",
            " ".join(request_service.split()),
        )
        self.assertNotIn(
            "[&](const nsf::ResponseMessage& response) {\n"
            "          onResponse(response);",
            request_service,
        )
        self.assertNotIn(
            "[&](const ndn::Name& requestId) {\n"
            "          onTimeout(requestId);",
            request_service,
        )


if __name__ == "__main__":
    unittest.main()
