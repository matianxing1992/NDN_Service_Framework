"""Pure evidence aggregation for NDNSF-REPO failure/recovery campaigns."""

from __future__ import annotations

from typing import Iterable, Mapping


def parse_catalog_sync_metric(line: str) -> dict[str, object] | None:
    """Parse stable key=value sidecar metrics without consuming NDN logs."""

    text = str(line).strip()
    if text.startswith("catalog_sync repair_cycle "):
        kind = "repairCycle"
        payload = text[len("catalog_sync repair_cycle "):]
    elif text.startswith("catalog_sync merged "):
        kind = "catalogMerge"
        payload = text[len("catalog_sync merged "):]
    else:
        return None
    result: dict[str, object] = {"kind": kind}
    for item in payload.split():
        if "=" not in item:
            continue
        key, raw_value = item.split("=", 1)
        if not key:
            continue
        try:
            result[key] = float(raw_value) if "." in raw_value else int(raw_value)
        except ValueError:
            result[key] = raw_value
    return result


def correlate_recovered_repairs(
    lifecycle_rows: Iterable[Mapping[str, object]],
    repair_events: Iterable[Mapping[str, object]],
    *,
    recovered_repo: str,
    failure_epoch_ms: int,
    restart_epoch_ms: int,
) -> dict[str, object]:
    """Correlate writes completed during an outage with target repair events."""

    successful_write_objects: set[str] = set()
    failed_write_objects: set[str] = set()
    outage_objects: set[str] = set()
    for row in lifecycle_rows:
        if str(row.get("operation", "")) != "write":
            continue
        object_name = str(row.get("objectName", ""))
        if not object_name:
            continue
        if int(row.get("success", 0) or 0) != 1:
            failed_write_objects.add(object_name)
            continue
        successful_write_objects.add(object_name)
        started_ms = int(row.get("startedEpochMs", 0) or 0)
        completed_ms = int(row.get("completedEpochMs", 0) or 0)
        if failure_epoch_ms and started_ms < failure_epoch_ms:
            continue
        if restart_epoch_ms and completed_ms > restart_epoch_ms:
            continue
        outage_objects.add(object_name)

    failed_only_write_objects = failed_write_objects - successful_write_objects

    target_events = []
    repair_events_for_failed_writes = []
    repaired_objects: set[str] = set()
    repair_latencies_ms: list[int] = []
    for raw_event in repair_events:
        event = dict(raw_event)
        if str(event.get("repoNode", "")) != recovered_repo:
            continue
        timestamp_ms = int(event.get("timestampMs", 0) or 0)
        if restart_epoch_ms and timestamp_ms < restart_epoch_ms:
            continue
        target_events.append(event)
        object_name = str(event.get("objectName", ""))
        if object_name in failed_only_write_objects:
            repair_events_for_failed_writes.append(event)
        if object_name in outage_objects:
            repaired_objects.add(object_name)
        if restart_epoch_ms and timestamp_ms:
            repair_latencies_ms.append(max(0, timestamp_ms - restart_epoch_ms))

    unrepaired = sorted(outage_objects - repaired_objects)
    repaired = sorted(repaired_objects)
    return {
        "recoveredRepo": recovered_repo,
        "successfulWriteObjects": sorted(successful_write_objects),
        "failedOnlyWriteObjects": sorted(failed_only_write_objects),
        "repairEventsForFailedWrites": repair_events_for_failed_writes,
        "invalidRepairEventCount": len(repair_events_for_failed_writes),
        "outageSuccessfulWriteCount": len(outage_objects),
        "outageWriteObjects": sorted(outage_objects),
        "recoveredTargetRepairEventCount": len(target_events),
        "repairedOutageObjectCount": len(repaired),
        "repairedOutageObjects": repaired,
        "unrepairedOutageObjects": unrepaired,
        "repairCoverage": (
            len(repaired) / len(outage_objects) if outage_objects else 0.0
        ),
        "firstRepairAfterRestartMs": (
            min(repair_latencies_ms) if repair_latencies_ms else None
        ),
        "lastRepairAfterRestartMs": (
            max(repair_latencies_ms) if repair_latencies_ms else None
        ),
    }
