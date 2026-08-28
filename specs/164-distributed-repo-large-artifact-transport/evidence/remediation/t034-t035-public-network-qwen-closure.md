# T034–T035 Public Network and Qwen Integration Closure

## Result

The public `ArtifactRepositoryApi` now moves one immutable whole artifact
through the real NDNSF Collaboration and RepoNode network path. Qwen stage
registration and cold Provider retrieval use this API and no longer call the
legacy per-chunk `DistributedRepo.put_file()` / `get_file()` path.

## Control and execution semantics

The control path is:

```text
begin_collaboration
  -> ACK_CLOSED
  -> commit_plan(exact queued replica task)
  -> provider queue
  -> adaptive segmented receive
  -> verify
  -> atomic commit
  -> authenticated receipt
```

A successful ACK is an advisory statement that the Provider is willing and
apparently able to accept the task. It reserves no bytes, creates no lease, and
acquires no storage lock. `commit_plan` binds the ACK-closed candidate snapshot
to an exact task and enqueues it. Definitive capacity and concurrency checks
occur when the Provider executes that task.

## Implementation evidence

- the publisher exposes a bounded-memory segmented source;
- the Selection assignment binds signed-root, page, payload, receipt scope,
  packet geometry, artifact identity, task identity, and operation identity;
- the selected RepoNode verifies the signed root, page graph, every chunk, and
  the full SHA-256 before atomic CAS activation;
- the RepoNode returns a durable HMAC receipt through the collaboration
  response and starts a committed-file producer for later retrieval;
- Qwen registration records the exact `artifactReference`, committed Data
  name, operation ID, durability result, and receipts for each model stage;
- a selected Qwen Provider reconstructs the exact reference and performs
  receipt-directed public API retrieval;
- rank cleanup removes bootstrap tokens, selection-storage keys, local
  selection-offer keys, and shared selection-offer keys on normal exit,
  failure, cancellation, or signal.

## Verification

Syntax and shell checks passed for the backend, Qwen jobs, Provider, tests, and
rank scripts.

Focused Spec 162 tests:

```text
test_spec162_qwen36_repo_registration.py: 4/4 PASS
test_spec162_t009_live_source_bundle.py: 7/7 PASS
```

Spec 164 suite:

```text
104/104 PASS
```

New immutable real MiniNDN confirmation:

```text
results/spec164-public-network-api-20260730T130448Z
verdict: PASS
payloadBytes: 1048576
wholeArtifactCollaborationCount: 1
controlOperationCount: 2
legacyPerChunkPutFileUsed: false
realRepoNodeDataPlane: true
destinationVisible: true
source/destination SHA-256:
ea53eb498f9b7dc054aae73924bf25c569604d2c20aa9016f0b8714a1c180c7b
```

This MiniNDN run is a correctness/security confirmation, not a throughput
claim. Earlier diagnostic failures remain retained; this run does not replace
or reinterpret any frozen performance campaign.

## TigerCluster boundary

These results authorize rebuilding the content-addressed Spec 162 source
bundle and submitting a new TigerCluster candidate. They do not themselves
prove RTX 5000, Qwen3.6-27B, or multi-node generation correctness.
