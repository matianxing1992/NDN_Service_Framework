"""Versioned Spec 129 Core deployment-control wire contracts."""

from __future__ import annotations

from . import _ndnsf

DeploymentIntent = _ndnsf.NativeDeploymentIntent
ProviderCapabilityOffer = _ndnsf.NativeProviderCapabilityOffer
DeploymentPlan = _ndnsf.NativeDeploymentPlan
ProviderReadyMessage = _ndnsf.NativeProviderReadyMessage
ReadyAcknowledgement = _ndnsf.NativeReadyAcknowledgement
ExecutionActivateMessage = _ndnsf.NativeExecutionActivateMessage
SecureStatusQuery = _ndnsf.NativeSecureStatusQuery
SecureStatusSnapshot = _ndnsf.NativeSecureStatusSnapshot
EncryptedRequestInput = _ndnsf.NativeEncryptedRequestInput
SelectionInputKeyOffer = _ndnsf.NativeSelectionInputKeyOffer
SelectionInputKeyGrant = _ndnsf.NativeSelectionInputKeyGrant
ReservationLease = _ndnsf.NativeReservationLease
SelectionDecision = _ndnsf.NativeSelectionDecision
SelectionDecisionReceipt = _ndnsf.NativeSelectionDecisionReceipt
RecipientEncryptedAssignment = _ndnsf.NativeRecipientEncryptedAssignment
StageInputEvidence = _ndnsf.NativeStageInputEvidence
StageAbort = _ndnsf.NativeStageAbort
SelectionDecisionTombstone = _ndnsf.NativeSelectionDecisionTombstone


def make_opaque_control_handle(random_bytes: int = 24) -> str:
    return str(_ndnsf.make_opaque_control_handle(int(random_bytes)))


def is_valid_opaque_control_handle(handle: str) -> bool:
    return bool(_ndnsf.is_valid_opaque_control_handle(str(handle)))


__all__ = [
    "DeploymentIntent", "ProviderCapabilityOffer", "DeploymentPlan",
    "ProviderReadyMessage", "ReadyAcknowledgement",
    "ExecutionActivateMessage", "SecureStatusQuery", "SecureStatusSnapshot",
    "EncryptedRequestInput", "SelectionInputKeyOffer", "SelectionInputKeyGrant",
    "ReservationLease", "SelectionDecision", "SelectionDecisionReceipt",
    "RecipientEncryptedAssignment", "StageInputEvidence", "StageAbort",
    "SelectionDecisionTombstone",
    "make_opaque_control_handle", "is_valid_opaque_control_handle",
]
