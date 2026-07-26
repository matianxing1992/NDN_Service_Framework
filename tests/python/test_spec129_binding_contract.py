from __future__ import annotations

import unittest
from ndnsf import _ndnsf

from ndnsf import (
    DeploymentIntent, DeploymentPlan, ExecutionActivateMessage,
    ProviderCapabilityOffer, ProviderReadyMessage, ReadyAcknowledgement,
    SecureStatusQuery, SecureStatusSnapshot,
    EncryptedRequestInput, SelectionInputKeyOffer, SelectionInputKeyGrant,
    ReservationLease, SelectionDecision, SelectionDecisionReceipt,
    RecipientEncryptedAssignment, StageInputEvidence, StageAbort,
    SelectionDecisionTombstone,
    is_valid_opaque_control_handle, make_opaque_control_handle,
)
from ndnsf.service import _native_deployment_intent


class Spec129BindingContractTest(unittest.TestCase):
    def test_all_native_control_messages_have_equivalent_round_trip_surface(self):
        classes = (
            DeploymentIntent, ProviderCapabilityOffer, DeploymentPlan,
            ProviderReadyMessage, ReadyAcknowledgement,
            ExecutionActivateMessage, SecureStatusQuery, SecureStatusSnapshot,
            EncryptedRequestInput, SelectionInputKeyOffer, SelectionInputKeyGrant,
            ReservationLease, SelectionDecision, SelectionDecisionReceipt,
            RecipientEncryptedAssignment, StageInputEvidence, StageAbort,
            SelectionDecisionTombstone,
        )
        for cls in classes:
            with self.subTest(message=cls.__name__):
                value = cls()
                value.set_field("requestId", "request-1")
                value.set_field("attempt", "1")
                decoded = cls.decode(value.wire_encode())
                self.assertEqual(decoded.version, 1)
                self.assertEqual(decoded.get_field("requestId"), "request-1")
                self.assertEqual(decoded.fields["attempt"], "1")
                self.assertEqual(decoded.digest(), value.digest())

    def test_handle_is_high_entropy_opaque_and_native_validated(self):
        first = make_opaque_control_handle()
        second = make_opaque_control_handle()
        self.assertEqual(len(first), 48)
        self.assertNotEqual(first, second)
        self.assertTrue(is_valid_opaque_control_handle(first))
        self.assertFalse(is_valid_opaque_control_handle("request-1"))

    def test_unknown_version_fails_before_wire_output(self):
        value = DeploymentIntent()
        value.version = 2
        with self.assertRaises(ValueError):
            value.wire_encode()

    def test_public_service_mapping_normalizes_to_native_intent(self):
        value = _native_deployment_intent({
            "artifactDigest": "sha256:model", "requiredRoles": "worker"})
        self.assertIsInstance(value, DeploymentIntent)
        self.assertEqual(value.get_field("artifactDigest"), "sha256:model")
        self.assertIs(_native_deployment_intent(value), value)
        self.assertIsNone(_native_deployment_intent(None))
        self.assertTrue(hasattr(_ndnsf.NativeServiceProvider,
                                "set_deployment_prepare_handler"))
        self.assertTrue(hasattr(_ndnsf.NativeServiceProvider,
                                "set_r1_selection_decision_handler"))
        self.assertTrue(hasattr(_ndnsf.NativeServiceProvider,
                                "set_r1_reservation_terminal_handler"))


if __name__ == "__main__":
    unittest.main()
