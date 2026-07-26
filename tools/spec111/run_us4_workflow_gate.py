#!/usr/bin/env python3
"""Verify the real Spec 111 Controller/Provider/User MiniNDN workflow."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workflow-summary", required=True)
    parser.add_argument("--user-log", required=True)
    parser.add_argument("--controller-log", required=True)
    parser.add_argument("--provider-log", action="append", required=True)
    args = parser.parse_args()

    summary_path = Path(args.workflow_summary)
    user_log = Path(args.user_log)
    controller_log = Path(args.controller_log)
    provider_logs = tuple(Path(item) for item in args.provider_log)
    if len(provider_logs) != 3:
        raise SystemExit("workflow gate requires exactly three Provider logs")
    paths = (summary_path, user_log, controller_log, *provider_logs)
    if any(not path.is_file() for path in paths):
        raise SystemExit("workflow gate evidence file is missing")

    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    wire_ids = tuple(str(item) for item in summary.get("wireRequestIds", ()))
    if (summary.get("schema") != "ndnsf-di-spec111-minindn-deployment-workflow-v1"
            or summary.get("status") != "PASS"
            or summary.get("terminalState") != "DELETED"
            or int(summary.get("rollbackEpoch", 0)) != 2
            or int(summary.get("providerCount", 0)) != 3
            or not str(summary.get("initialRevision", "")).startswith("sha256:")
            or not str(summary.get("rollbackRevision", "")).startswith("sha256:")
            or len(wire_ids) < 7 or len(set(wire_ids)) != len(wire_ids)):
        raise SystemExit("workflow summary does not satisfy the T144 contract")

    user_text = user_log.read_text(errors="replace")
    if ("LLM_PIPELINE_DEPLOYMENT_ACTIVE" not in user_text
            or "LLM_PIPELINE_DURABLE_REQUEST" not in user_text
            or "LLM_PIPELINE_DEPLOYMENT_WORKFLOW_PASS" not in user_text):
        raise SystemExit("User log lacks lifecycle or durable-request proof")
    for action in ("PREPARE", "ACTIVATE", "DRAIN", "DELETE"):
        for provider_log in provider_logs:
            if f"action={action}" not in provider_log.read_text(errors="replace"):
                raise SystemExit(
                    f"Provider log lacks {action}: {provider_log}")
    if "controller ready" not in controller_log.read_text(errors="replace"):
        raise SystemExit("Controller log lacks startup proof")

    result = {
        "schema": "ndnsf-di-spec111-us4-workflow-gate-v2",
        "status": "PASS",
        "network": "MiniNDN",
        "controllerProviderUser": True,
        "initialRevision": summary["initialRevision"],
        "rollbackRevision": summary["rollbackRevision"],
        "rollbackEpoch": summary["rollbackEpoch"],
        "terminalDeploymentState": summary["terminalState"],
        "wireRequestCount": len(wire_ids),
        "artifacts": [
            {"path": str(path), "digest": digest(path), "bytes": path.stat().st_size}
            for path in paths
        ],
    }
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
