#!/usr/bin/env python3
"""Capture independent stock ByteTrack expectations for the native tracker tests.

Use the personPreparation tool versions in Experiments/recipes/uav-multicam-tracking.json.
Only upstream BYTETracker updates produce the reference; native code is never imported.
"""

import argparse
import hashlib
from importlib.metadata import version
import inspect
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from ultralytics.engine.results import Boxes
from ultralytics.trackers.basetrack import BaseTrack
from ultralytics.trackers.byte_tracker import BYTETracker, merge_track_pools


def state(tracker):
    def track(item):
        return {"trackId": int(item.track_id), "isActivated": bool(item.is_activated),
                "state": int(item.state), "tlwh": item._tlwh.astype(float).tolist(),
                "mean": item.mean.astype(float).tolist(),
                "covariance": item.covariance.astype(float).tolist(),
                "score": float(item.score), "class": float(item.cls), "index": float(item.idx),
                "angle": None if item.angle is None else float(item.angle),
                "startFrame": int(item.start_frame), "frameId": int(item.frame_id),
                "timeSinceUpdate": int(getattr(item, "time_since_update",
                                               tracker.frame_id - item.frame_id)),
                "trackletLength": int(item.tracklet_len)}
    return {"frameId": int(tracker.frame_id), "idHighWatermark": int(BaseTrack._count),
            "tracked": [track(t) for t in tracker.tracked_stracks],
            "lost": [track(t) for t in tracker.lost_stracks],
            "removedIds": [int(t.track_id) for t in tracker.removed_stracks]}


def discrete(value):
    fields = ("trackId", "state", "isActivated", "frameId", "startFrame",
              "timeSinceUpdate", "trackletLength")
    return [value["frameId"], value["idHighWatermark"], value["removedIds"],
            [[item[k] for k in fields] for item in value["tracked"]],
            [[item[k] for k in fields] for item in value["lost"]]]


def capture(rows, buffer=30, outputs=None):
    tracker = BYTETracker(SimpleNamespace(track_buffer=buffer, track_high_thresh=.25,
                          track_low_thresh=.1, new_track_thresh=.25,
                          match_thresh=.8, fuse_score=True))
    BaseTrack._count = 0
    result = []
    for data in rows:
        output = tracker.update(Boxes(np.asarray(data, dtype=np.float32).reshape(-1, 6), (480, 640)))
        if outputs is not None:
            outputs.append(output.astype(float).tolist())
        result.append(state(tracker))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    crossing = []
    for step in range(31):
        rows = [[40 + 4 * step, 50, 80 + 4 * step, 150, .9, 0],
                [160 - 4 * step, 74, 200 - 4 * step, 174, .9, 0]]
        crossing.append(rows if step % 2 == 0 else rows[::-1])
    occlusion = [[] if step in (2, 3) else
                 [[10 + step, 20, 50 + step, 120, .9, 0]] for step in range(5)]
    detected = [[10, 20, 50, 120, .9, 0]]
    low = [[[80, 110, 120, 210, .9, 0]],
           [[82, 110, 122, 210, .2, 0], [380, 110, 420, 210, .2, 0],
            [480, 110, 520, 210, .05, 0]]]
    mixed = [[[10, 20, 50, 120, .9, 0], [110, 20, 150, 120, .9, 0]],
             [[11, 20, 51, 120, .9, 0]],
             [[12, 20, 52, 120, .9, 0], [112, 20, 152, 120, .9, 0],
              [230, 20, 270, 120, .9, 0]],
             [[13, 20, 53, 120, .9, 0], [113, 20, 153, 120, .9, 0],
              [231, 20, 271, 120, .9, 0]]]
    loss_order = [[[10, 20, 50, 120, .9, 0], [110, 20, 150, 120, .9, 0]],
                  [[11, 20, 51, 120, .9, 0]], [],
                  [[13, 20, 53, 120, .9, 0], [113, 20, 153, 120, .9, 0]]]
    unconfirmed = [[[10, 20, 50, 120, .9, 0]],
                   [[10, 20, 50, 120, .9, 0], [110, 20, 150, 120, .9, 0]],
                   [[10, 20, 50, 120, .9, 0], [131, 20, 171, 120, .9, 0]],
                   [[10, 20, 50, 120, .9, 0], [132, 20, 172, 120, .9, 0]]]
    cost_limit = [[[100, 20, 200, 120, .9, 0], [130, 20, 230, 120, .9, 0]],
                  [[114, 20, 214, 120, .9, 0], [25, 20, 125, 120, .9, 0]]]
    optional_match = [[[20, 20, 120, 120, .9, 0], [10, 20, 65, 120, .9, 0]],
                      [[10, 20, 120, 120, .9, 0], [70, 20, 120, 120, .9, 0]]]
    first_boundary = [[[0, 0, 100, 100, .9, 0]], [[0, 0, 40, 100, .5, 0]]]
    second_boundary = [[[0, 0, 100, 100, .9, 0]], [[0, 0, 50, 100, .2, 0]]]
    fifo = capture([[[100, 100, 140, 200, .9, 0]] if step % 5 < 2 else []
                    for step in range(1006 * 5)], buffer=1)
    trace = hashlib.sha256()
    for value in fifo:
        trace.update((json.dumps(discrete(value), separators=(",", ":")) + "\n").encode())
    rows = [[i * 10, 10, i * 10 + 8, 90, .9, 0] for i in range(512)]
    allocator = capture([rows, rows[:-1], rows[:-1], rows[:-1], [], [], [],
                         rows[:1], rows[:1]], buffer=1)
    record = {"fifoPrototypes": fifo[:10],
              "fifoFinal": fifo[-1], "fifoTraceSha256": trace.hexdigest(),
              "allocatorFinal": allocator[-1], "allocatorPredecessor": allocator[6],
              "versions": {key: version(key) for key in ("ultralytics", "numpy", "lap")},
              "stockMergeSourceSha256": hashlib.sha256(
                  Path(inspect.getfile(merge_track_pools)).read_bytes()).hexdigest(),
              "stockTrackerSourceSha256": hashlib.sha256(
                  Path(inspect.getfile(BYTETracker)).read_bytes()).hexdigest()}
    record["outputs"] = {}
    for name, inputs, buffer in (("crossing", crossing, 30), ("occlusion", occlusion, 30),
                                 ("expiry", [detected, [], [], [], detected, detected], 1),
                                 ("low", low, 30), ("mixed", mixed, 30),
                                 ("lossOrder", loss_order, 30),
                                 ("unconfirmed", unconfirmed, 30),
                                 ("costLimit", cost_limit, 30),
                                 ("optionalMatch", optional_match, 30),
                                 ("firstBoundary", first_boundary, 30),
                                 ("secondBoundary", second_boundary, 30)):
        outputs = []
        record[name] = capture(inputs, buffer, outputs)
        record["outputs"][name] = outputs
    payload = json.dumps(record, sort_keys=True, separators=(",", ":"), allow_nan=False)
    literals = "\n".join("  " + json.dumps(payload[i:i + 48])
                          for i in range(0, len(payload), 48))
    args.output.write_text("#ifndef NDNSF_TESTS_UAV_PERSON_TRACKER_GOLDEN_HPP\n"
        "#define NDNSF_TESTS_UAV_PERSON_TRACKER_GOLDEN_HPP\n\n"
        "namespace ndnsf::test::person_fixture {\n\n"
        "// Generated by tests/fixtures/generate-uav-person-tracker-golden.py.\n"
        "// Independent stock BYTETracker reference; no native tracker output is used.\n"
        "inline constexpr char STOCK_TRACKER_GOLDEN[] =\n" + literals + ";\n\n"
        "} // namespace ndnsf::test::person_fixture\n\n"
        "#endif // NDNSF_TESTS_UAV_PERSON_TRACKER_GOLDEN_HPP\n")
    print(json.dumps({"bytes": args.output.stat().st_size,
                      "sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
                      "fifoTraceSha256": trace.hexdigest(), "versions": record["versions"]}))


if __name__ == "__main__":
    main()
