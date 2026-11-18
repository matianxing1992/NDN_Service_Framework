from pathlib import Path

import pytest

from tools.spec174_traceability import build


REPO = Path(__file__).resolve().parents[2]
# The traceability document is local-only Spec material under the ignored `specs/` tree.
pytestmark = pytest.mark.skipif(
    not (REPO / "specs/174-ndnsf-di-verified-delivery/traceability.md").is_file(),
    reason="requires specs/174-ndnsf-di-verified-delivery/traceability.md")


def test_spec174_traceability_has_exact_requirement_inventory():
    result = build(REPO, "not-executed", [])
    assert result["counts"] == {
        "functionalRequirements": 29,
        "successCriteria": 11,
    }
    assert len(result["rows"]) == 40
    assert all(row["owner"] and row["proof"] for row in result["rows"])


def test_spec174_traceability_is_deterministic():
    first = build(REPO, "not-executed", [])
    second = build(REPO, "not-executed", [])
    assert first == second


def test_spec174_traceability_rejects_unknown_executed_requirement():
    with pytest.raises(ValueError, match="UNKNOWN_EXECUTED"):
        build(REPO, "not-executed", ["FR-999"])
