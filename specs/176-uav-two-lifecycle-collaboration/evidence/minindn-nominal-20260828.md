# Spec176 nominal MiniNDN evidence

Date: 2026-08-28
Branch: `UAV-Experimental`
Output: `/tmp/spec176-minindn-stream60b.jpROqE`

Command:

```text
timeout 210s unshare --user --map-root-user --mount --net \
  python3 examples/ndnsf/uav-collaboration/minindn_uav_collaboration.py \
  --run --window-seconds 60 --output /tmp/spec176-minindn-stream60b.jpROqE
```

The launcher completed with `RUN_RC=0` and wrote:

```text
elapsedSeconds=84.346
processReturnCodes=[-2, 0, 0, 0, 0]
status=nominal-collaboration-completed
```

The controller received the shared policy/trust-schema requests. The Ground
Station completed two independent finite jobs in one 60-second MissionSession:

```text
job=1 ok=true state=SUCCEEDED selected_provider=/example/uav/drone/C
job=2 ok=true state=SUCCEEDED selected_provider=/example/uav/drone/C
ok=true state=SUCCEEDED jobs=2 measured_window_seconds=60
```

The same process also exercised the real predictive stream rather than a
placeholder stream flag:

```text
GS_VIDEO_STREAM_READY ...
STREAM_API_ACTIVE role=consumer mode=predictive ...
SPEC176_STREAM_RESULT ok=true state=streaming chunks=3 decoded_frames=0
SPEC176_STREAM_FINAL ok=true state=streaming chunks_before_jobs=3 chunks_after_jobs=112
```

The final marker is the independence check: while both finite incident jobs
ran, the stream stayed active and its fetched Data-chunk count increased.

The trace contains, for both jobs:

```text
REQUEST_RECEIVED -> ACK_PUBLISHED -> COLLAB_ACK_CLOSED
-> SELECTION_PUBLISHED -> PROVIDER_SELECTED
-> producer-owned evidence Data publication
-> exact Data validation -> EXECUTION_DONE
-> RESPONSE_PUBLISHED -> RESPONSE_DECRYPT_DONE -> RESPONSE_VALIDATION_DONE
```

Evidence names are producer-owned and immutable, for example
`/example/uav/drone/A/UAV/MISSION/spec176-minindn-mission/INCIDENT/incident-001/EVIDENCE/frame-001/v=1`.
The detector log records `MessageValidator Data validated` for the exact
evidence name before execution. No application payload contains an IP, host,
port, or socket endpoint.

After job 1, the Ground Station ran a second-consumer path that issued a fresh
exact Interest for the same immutable evidence name.  It received the same
producer-owned Data and verified the producer certificate/signature and digest:

```text
SECOND_CONSUMER_INTEREST_ISSUED ...
SECOND_CONSUMER_DATA_RECEIVED requested=... returned=...
SECOND_CONSUMER_DATA_VERIFIED ... producer=/example/uav/drone/A bytes=414
SPEC176_SECOND_CONSUMER_RESULT ok=true name=...
```

The second consumer uses no endpoint or connection identifier; its result is
bound only to the returned Data name, producer, signature, and digest.

The new acceptance trace also records the explicit bounded retrieval pair for
both jobs, including the configured 500 ms Interest lifetime:

```text
COLLAB_DATA_INTEREST_ISSUED ... incident-001 ... attempt=1 lifetimeMs=500
COLLAB_DATA_RECEIVED ... incident-001 ... requestedName=... returnedName=...
COLLAB_DATA_VERIFIED ... incident-001 ... producer=/example/uav/drone/A bytes=414
COLLAB_DATA_INTEREST_ISSUED ... incident-002 ... attempt=1 lifetimeMs=500
COLLAB_DATA_RECEIVED ... incident-002 ... requestedName=... returnedName=...
COLLAB_DATA_VERIFIED ... incident-002 ... producer=/example/uav/drone/A bytes=414
```

Each exact fetch uses `CanBePrefix=false`, `MustBeFresh=true`, a finite
Interest lifetime, and at most one scheduled retry before the job deadline.

This is nominal multi-process evidence only. T019 is covered separately by
`evidence/minindn-failure-matrix-20260828.md`; multi-segment/large-data cases
and T020 PX4 SITL acceptance remain open. Reproducibility hashes for this run:

```text
e2d0bf2e41f000d1a82846b3fa74f9e964994ba670d5f09064ee0b839545add1  manifest.json
45d1b69cfa97467ab6f2756c346228bee106c80361014864140b4c26bd1d3739  summary.json
62239760a48dc3aaebb792931027819c03d7b95ee1c50877789cf6f2ffdf0822  ground-station.log
5f8bf53b9843746e69e60770457b61725ebf0953efe7534a624e8248e2a054ba  compute.log
```

## Post-selector candidate rerun

Date: 2026-08-28
Output: `/tmp/spec176-selector-nominal2.X0TNVv`

After the Ground Station selector was wired to ACK capability metadata and the
controller was rebuilt against the same candidate library, the registered
60-second profile completed again:

```text
elapsedSeconds=83.455
processReturnCodes=[-2, 0, 0, 0, 0]
status=nominal-collaboration-completed
SPEC176_CAPABILITY_SELECTION provider=/example/uav/drone/C fallback=false
SPEC176_STREAM_FINAL ok=true state=streaming chunks_before_jobs=11 chunks_after_jobs=126
SPEC176_INCIDENT_TEST_RESULT ok=true state=SUCCEEDED jobs=2 measured_window_seconds=60
GS_INCIDENT_COLLABORATION_EXIT ok=true
```

The two selector log entries for each job both chose the compute UAV from the
verified capability snapshot. The controller's `-2` return code is the expected
SIGINT used by the launcher during orderly teardown; all application processes
completed normally.

Candidate hashes:

```text
971fe81e87980a6bf64aa770d9a94d1d4805f84b3ee678d3ef2a33a8a2629e27  manifest.json
0c33cd84a684eae47f5e39d75e05eab82235caa63bb4b67b50638f36027697d6  summary.json
e01b7d57a78a0f5e4c363214a9f2b25ae3bc571764a568105c5550c9abcd294f  ground-station.log
bd7d2da796448eb8e0ec4fade340226a9bf7faa7438cd354d72a07270c744cb1  compute.log
```

This rerun closes the post-selector nominal MiniNDN gate. Multi-segment
adverse-data evidence and PX4 SITL remain separate gates.
