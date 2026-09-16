# Spec186 local exact-SIF YOLO four-provider run — 2026-09-16

## Scope

This receipt records the Y-B normal MiniNDN run requested after the single-provider
smoke. It proves one local four-provider application execution on CPU. It does
not prove Tiger GPU, two-node execution, or clean harness teardown.

## Candidate and composition

| Field | Value |
| --- | --- |
| Run ID | `spec186-yolo-local-normal-r86-v5` |
| Candidate digest | `sha256:66afadee66e0805a269055ab98a325190af9707f33e138c5c27e21d9f297d8dc` |
| Source seal | `sha256:1cfafed9c78b8f256fe4d5bd881fe2603125f6c679ad8c66044dae6cba4c5ce9` |
| Apptainer | `/usr/local/bin/apptainer` 1.5.3 |
| Base SIF | `sha256:089a4bc942db5fcc9d01ba2bf62b01ae1836416a232f0806a61931f26d946547` |
| Application bundle | `sha256:92934b89e842483a12b6669cf935b62a1ec8b1750d1093f097ccc4cbcbc58f79` |
| Case | Y-B normal, one local MiniNDN topology |

Roles were `BackboneNeck`, `DetectShard0`, `DetectShard1` and `Merge`.

## Protocol and execution evidence

`evidence/lifecycle.jsonl` records `ACK_CLOSED` with `ackCount=4`,
`GRAPH_READY`, `SELECTION_COMMITTED` with `selectedRoleCount=4`,
`PROVIDER_EXECUTION_STARTED` with `providerCount=4`, and terminal
`TERMINAL_RESPONSE` with `status=true`.

Each provider log contains one READY marker and one completed execution update:

| Provider | Runner | `realCompute` | Completed |
| --- | --- | --- | --- |
| BackboneNeck | `onnxruntime-cpu` | `true` | `true` |
| DetectShard0 | `onnxruntime-cpu` | `true` | `true` |
| DetectShard1 | `onnxruntime-cpu` | `true` | `true` |
| Merge | `native-yolo-postprocess` | `false` | `true` |

The User log contains:

```text
YOLO_ACK_DRIVEN_RESULT status=true payload_bytes=1267
```

The numerical oracle reports `matched=true`, shape `[1,50,6]`, and
`maxAbsError=0.0005340576171875` (`rtol=0.0001`, `atol=0.001`).

## Cleanup boundary

The bounded submitter ended with `cleanup.forced=true` and `exitCode=-15` after
the terminal response. The protocol and numerical evidence are retained, but
this run is `FAILED` at the harness level because Controller/Repo/NFD child
processes were not reaped within the cleanup boundary. The leftover processes
were subsequently terminated by their exact run/SIF identity.

Therefore this is a **four-provider local application execution PASS with
cleanup failure**, not a clean qualification PASS. The next recipe-built
candidate must fix teardown and repeat Y-B before promotion or Tiger submission.
