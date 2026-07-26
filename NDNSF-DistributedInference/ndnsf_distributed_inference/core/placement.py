"""Final validated placement application."""

from __future__ import annotations

from .decision_validation import validate_assignment
from .ports import AssignmentProposal
from .contracts import AssignmentContext


def create_assignment_context(
    proposal: AssignmentProposal, *, request_id: str, attempt_epoch: int,
    plan_digest: str, model_variant_id: str, original_deadline_ms: int,
    roles, eligible_providers, excluded_providers=(),
) -> AssignmentContext:
    validate_assignment(proposal, roles, eligible_providers)
    if proposal.model_variant_id != model_variant_id:
        raise ValueError("assignment/model variant binding mismatch")
    return AssignmentContext(
        request_id, attempt_epoch, plan_digest, model_variant_id,
        tuple(sorted(proposal.providers_by_role.items())), original_deadline_ms,
        tuple(sorted(set(excluded_providers))))


__all__ = ["create_assignment_context"]
