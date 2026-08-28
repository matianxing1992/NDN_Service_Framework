# T026 High-Concurrency Failure Diagnosis

## Conclusion

The retained high-concurrency failures are caused by concurrent NFD RIB
registrations made by several producer processes with the same management
identity, combined with an API that ignored registration completion and
failure. They are not caused by manifest signing, SQLite persistence, payload
verification, or the queued no-reservation control path.

`FileSegmentedObjectProducer.start()` submits `setInterestFilter()` and returns
before NFD invokes either callback and discards registration failure. Once
`start()` was changed to wait for the callback, r1/c4 exposed the previously
hidden error directly:

```text
failed to register file Data prefix ...: authorization rejected
```

A one-producer c1 control succeeds with the same `/localhost/operator`
management identity, while concurrent c4 registrations fail. The distinguishing
factor is therefore concurrent same-key NFD management commands, whose
timestamp/replay authorization can collide. In the old path, workers still
published `<worker>.store-ready.json`; cold Interests then reached an NFD with
no local producer route and received Nack reason 150 (`NO_ROUTE`).

The correction has two parts: give `FileSegmentedObjectProducer.start()` a
bounded, fail-closed readiness contract, and serialize only the short
management-plane registration handshake in the MiniNDN harness. Publication,
storage, cold transfer, and verification remain concurrent. This is not a
resource reservation or data-plane lock.

## Frozen-evidence symptom

Canonical input:

```text
results/spec164-artifact-remediation-campaign-20260730T0820Z
```

All 43 measured failures have the same outer form: the harness times out
waiting for one or more `consumer-*.cold.json` files. The corresponding cold
consumer logs report:

```text
RuntimeError: Data packet fetch failed for
/spec164/repo-cold/<worker>: Nack: 150
```

System header authority:

```text
/usr/local/include/ndn-cxx/lp/nack-header.hpp:39
NO_ROUTE = 150
```

The failure spans `raw-segmented-ndn`, `legacy-exact-packet`,
`digest-only`, and `signed-manifest`. It is concentrated in r1/c4, r1/c16,
and r3/c4. The c1 cells pass. This subject-independent concurrency pattern
falsifies the earlier narrow hypothesis that signed-manifest verification or
SQLite serialization alone caused SC-003.

## Direct reproduction

The diagnosis created a new disposable result; neither frozen campaign was
modified:

```bash
sudo -n env \
  PYTHONPATH="$PWD/pythonWrapper:$PWD/NDNSF-DistributedRepo/pythonWrapper" \
  LD_LIBRARY_PATH="$PWD/build:$PWD/build/lib:$PWD/build/NDNSF-DistributedRepo" \
  python3 Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py \
  --performance-subject signed-manifest \
  --payload-size 1048576 \
  --replicas 1 \
  --concurrency 4 \
  --timeout-seconds 15 \
  --quick-smoke \
  --output-dir \
    results/spec164-t026-preregistration-r1c4-before-20260730T0845Z
```

Observed:

```text
exit: 1
publication/store workers represented: 4
cold results: 3
missing: consumer-op0-replica0.cold.json
cold-consumer-op0-replica0.log: Nack: 150
```

This reproduces the same user-visible campaign failure at the real MiniNDN
seam in 18.5 seconds.

## Source chain

1. `NativeFileSegmentedObjectProducer.start()` calls
   `m_face.setInterestFilter(...)`.
2. Its success callback is empty.
3. Its failure callback only records `m_error`; callers cannot observe it
   before treating the producer as ready.
4. The face-processing thread starts, but `start()` immediately returns; it
   does not wait for either callback.
5. `_benchmark_consumer_role()` immediately writes
   `<worker>.store-ready.json` after `FileSegmentedObjectProducer(...).start()`.
6. `_run_repository_subject()` treats all store-ready files as sufficient,
   installs the consumer-to-repository route, and launches cold consumers.
7. Concurrent workers sign NFD management registrations with the same command
   identity; a rejected registration is ignored.
8. The route on the cold consumer is valid, but the repository NFD lacks that
   rejected producer prefix. The first Interest receives `NO_ROUTE`.

Relevant source locations:

```text
pythonWrapper/src/ndnsf/_ndnsf.cpp:634-665
Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py:1000-1014
Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py:1412-1433
```

## Ranked hypotheses and dispositions

| Rank | Hypothesis | Prediction | Result |
|---:|---|---|---|
| 1 | Concurrent same-identity NFD RIB commands are rejected, and producer readiness ignores the rejection | Waiting for callbacks exposes authorization rejection; c1 passes with the same identity while c4 fails | CONFIRMED |
| 2 | Producer prefix registration is merely still in flight when store-ready is published | Waiting for callbacks alone would make c4 pass | REJECTED as the complete cause; callback waiting exposed deterministic authorization rejection |
| 3 | Per-worker consumer routes are absent or point to the wrong repository | `nfdc` route installation would be missing for every run/worker | REJECTED as primary cause; routes are installed before each cold process, while failures are probabilistic |
| 4 | Repository producer exits before cold retrieval | Store worker logs would exit or all its cold fetches would fail | REJECTED; workers wait for their own cold result before stopping |
| 5 | Adaptive window/retry is too aggressive | Timeout/retransmission counters or congestion Nacks would dominate | REJECTED; the first failure is immediate NO_ROUTE, not congestion or retry exhaustion |
| 6 | Signing/SQLite/CPU contention | Failures would cluster in signed/persistent subjects and show verify/persist delay | REJECTED as root cause; raw and legacy subjects fail with the same NO_ROUTE signature |

## Required correction and regression

T027 must:

1. start the face-processing thread and wait, with a bounded deadline, for the
   exact prefix registration callback;
2. return only after success;
3. throw a bounded error and stop cleanly on registration failure or timeout;
4. preserve idempotent `start()`/`stop()` behavior;
5. keep the management-plane registrations serialized per NFD when workers
   share a command identity, without serializing publication or transfer;
6. add a failure-focused MiniNDN r1/c4 regression at the same real seam;
7. rerun the pre-fix command and require all four cold destinations to pass.

The fix must not increase the transfer retry budget, weaken timeouts, insert a
magic sleep, change campaign admission, or change SC-003.
