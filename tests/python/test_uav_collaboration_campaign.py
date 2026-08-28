#!/usr/bin/env python3
"""Analyzer contract for Spec 176 correlation-complete traces.

The analyzer is intentionally transport-neutral: it checks lineage, bounded
jobs, terminal ownership, and named-data fields without treating an IP endpoint
as a valid provider/object identifier.
"""

from __future__ import annotations

import json
import unittest
from collections import Counter, defaultdict
from typing import Iterable, Mapping


TERMINAL_STAGES = {"TERMINAL_ACCEPTED", "FAILURE"}
REQUIRED_FIELDS = {"mission_id", "incident_id", "request_id", "stage"}


def validate_events(events: Iterable[Mapping[str, object]], max_jobs: int = 64,
                    allow_fallback: bool = False) -> list[str]:
    errors: list[str] = []
    jobs: set[tuple[str, str]] = set()
    terminal_counts: Counter[tuple[str, str]] = Counter()
    stages: defaultdict[tuple[str, str], set[str]] = defaultdict(set)
    for index, event in enumerate(events):
        missing = sorted(REQUIRED_FIELDS - set(event))
        if missing:
            errors.append(f"event {index}: missing {','.join(missing)}")
            continue
        mission = str(event["mission_id"])
        incident = str(event["incident_id"])
        request = str(event["request_id"])
        stage = str(event["stage"])
        if not mission or not incident or not request:
            errors.append(f"event {index}: incomplete lineage")
        if any(token in json.dumps(event, sort_keys=True).lower()
               for token in ("://", "host=", "port=", "socket")):
            errors.append(f"event {index}: transport endpoint leaked into contract")
        key = (mission, incident)
        jobs.add((mission, request))
        stages[key].add(stage)
        if stage in TERMINAL_STAGES:
            terminal_counts[key] += 1
        if bool(event.get("fallback", False)) and not allow_fallback:
            errors.append(f"event {index}: implicit fallback")
    if len(jobs) > max_jobs:
        errors.append("job bound exceeded")
    for key, count in terminal_counts.items():
        if count > 1:
            errors.append(f"{key[1]}: duplicate terminal event")
    return errors


class UavCollaborationCampaignAnalyzerTest(unittest.TestCase):
    def test_accepts_complete_named_data_lineage(self) -> None:
        events = [
            {"mission_id": "m", "incident_id": "i", "request_id": "/r", "stage": "SELECTION"},
            {"mission_id": "m", "incident_id": "i", "request_id": "/r", "stage": "EVIDENCE_DATA_VERIFIED",
             "requested_data_name": "/drone/UAV/MISSION/m/INCIDENT/i/EVIDENCE/f/%FD%01"},
            {"mission_id": "m", "incident_id": "i", "request_id": "/r", "stage": "TERMINAL_ACCEPTED"},
        ]
        self.assertEqual(validate_events(events), [])

    def test_rejects_duplicate_terminal_and_endpoint(self) -> None:
        events = [
            {"mission_id": "m", "incident_id": "i", "request_id": "/r", "stage": "TERMINAL_ACCEPTED"},
            {"mission_id": "m", "incident_id": "i", "request_id": "/r", "stage": "TERMINAL_ACCEPTED",
             "detail": "host=10.0.0.2 port=6363"},
        ]
        errors = validate_events(events)
        self.assertTrue(any("duplicate terminal" in error for error in errors))
        self.assertTrue(any("endpoint leaked" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
