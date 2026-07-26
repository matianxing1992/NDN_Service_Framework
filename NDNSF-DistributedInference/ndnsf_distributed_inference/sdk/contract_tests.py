"""Reusable conformance checks for third-party optimizer packages."""

from __future__ import annotations

from .contracts import POLICY_KINDS


def validate_suite_contract(suite) -> None:
    if tuple(suite.policy_names()) != POLICY_KINDS:
        raise AssertionError("optimizer suite must resolve all ten policy ports")
    for kind in POLICY_KINDS:
        policy = suite.policy(kind)
        method = "dispatch" if kind == "scheduling" else (
            "admit" if kind == "admission" else (
                "transition" if kind == "recovery" else "propose"))
        if not callable(getattr(policy, method, None)):
            raise AssertionError(f"{kind} does not implement {method}")


__all__ = ["validate_suite_contract"]
