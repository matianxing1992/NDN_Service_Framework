"""Real process cleanup boundary; fixture launcher is not Apptainer."""
import sys
from pathlib import Path

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_result as result
from runtime.yolo_worker import NodeRuntime
from test_yolo_worker import prepared


def test_actual_worker_close_matches_launch_inventory(tmp_path):
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        worker.start_service('BackboneNeck', [sys.executable, '-c', 'import time; time.sleep(60)'])
    finally:
        rows = worker.close()
    evidence = result.validate_worker_cleanup(worker, rows)
    assert evidence['childCount'] == 1
    assert evidence['qualification'] == 'CLEANUP_COMPONENT_ONLY'


@pytest.mark.parametrize('fault', ['missing', 'duplicate', 'pid', 'forced', 'unreaped',
    'lease', 'early-exit', 'bad-exit', 'retained-owner', 'unclosed', 'failed-launch'])
def test_cleanup_cannot_hide_missing_failed_or_owned_processes(tmp_path, fault):
    worker = NodeRuntime(**prepared(tmp_path))
    try:
        worker.start_service('BackboneNeck', [sys.executable, '-c', 'import time; time.sleep(60)'])
    finally:
        rows = worker.close()
    if fault == 'missing': rows = []
    if fault == 'duplicate': rows.append(dict(rows[0]))
    if fault == 'pid': rows[0]['pid'] += 1
    if fault == 'forced': rows[0]['forced'] = True
    if fault == 'unreaped': rows[0]['reaped'] = False
    if fault == 'lease': rows[0]['leaseReleased'] = False
    if fault == 'early-exit': rows[0]['exitedBeforeCleanup'] = True
    if fault == 'bad-exit': rows[0]['exitCode'] = 1
    if fault == 'retained-owner': worker.leases['unexpected'] = object()
    if fault == 'unclosed': worker.closed = False
    if fault == 'failed-launch': worker.launches[0]['startError'] = 'failed'
    with pytest.raises(result.EvidenceError):
        result.validate_worker_cleanup(worker, rows)
