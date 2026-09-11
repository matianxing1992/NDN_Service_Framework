# Spec183 closure — 2026-09-11

Spec183 is closed on branch `TigerClusterExperiments` with one immutable,
layered v56 composition. The base SIF is
`sha256:44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0`
(3,586,351,104 bytes); the external read-only APP manifest is
`sha256:4a6c3af0c2cc24620c28519404f1a472074ebf354608372934339ae241942db7`;
the sealed harness is
`sha256:09d8bb4d237453acf1a7a33048363712c5bfdde87e8623a2be359f7eff969ed`.
Dispatch, input and runtime identities are
`sha256:d18cf044576875ac2aebc6d6b67a98439ac42ee37dd68510abcf8ff1bdd65545`,
`sha256:acd62dd7c78323f89b2a47ac27348a4d672cb1907e3802816a4d650a31e00933`,
and `sha256:16cbf7db11888d43e5d7e1431bd30b03928776660f62869f0050e6eb78c8fed3`.

## Executed sequence

The production sequence was design/code audit → focused/unit/integration gates
→ MiniNDN → exact-SIF local → single-node GPU → first two-node GPU → registered
negative → second unchanged two-node GPU. `prepare` was run for every v56
candidate and intentionally returned `PREPARED/NOT_EVALUATED` (exit 78); the
dispatch check returned zero. Local execution used `submit.py local` and its
frozen bundle collector. The first CLI collection returned a transient
`LOCAL_EXECUTION_FAILED:OperatorError`; direct collection from that frozen
bundle with `/usr/bin/python3` produced the authoritative PASS and did not
change any input. Tiger runs used `submit.py submit`, Slurm completion, and
remote `collect --reconcile` with the pinned operator environment.

| Gate | Run / job | Authoritative verdict |
| --- | --- | --- |
| Local CPU | `tiger-local-cpu-v56-r1` | `sha256:2159969ea07e52265e3147f8c28b2afe3485a078648e21cfbd83a314b3c88e89`, `NORMAL_EXPERIMENT_PASS`, 2 requests |
| Single GPU | `210471`, `itiger02` | `sha256:43ad2d5729798ac4f03901dbab22e80c4db261e0d72e6d37457e2fb79036d032`, 1 warmup + 1 measured |
| Two-node normal | `210472`, `itiger02,itiger03` | `sha256:0972a0d69c4c866c45bf9c6228629746d3f0311b6fb74e8b5df61cfb95b4d03a`, 1 warmup + 3 measured |
| Negative dependency | `210473`, `itiger02,itiger03` | `sha256:15cbf0df6f48e685aab3c15fdf2f0f3be90402fccc5a1f3ed0e98e575fdbd76b`, `EXPECTED_REJECTION_PASS` |
| Two-node reuse | `210474`, `itiger02,itiger03` | `sha256:b8513ddf693b74a22d6214d19d91fe3d55ba81ff1a9454a4a3e5386f686039d6`, 1 warmup + 3 measured |

The normal verdicts contain four roles, nine dependency edges per request,
graph digest `sha256:d8b40347e4cb60e7a0f74b3503d04816ba897a4e8f8733e9d59a38a8c9a65ed1`,
CUDA execution for BackboneNeck/DetectShard0/DetectShard1, CPU Merge,
independent oracle shape `[1,50,6]`, `matched=true`, maximum absolute error
`0.00042724609375` on GPU, and controlled cleanup. The local CPU maximum error
was `0.0005340576171875`. The negative retains one exact
DetectShard0→Merge cutpoint, native missing-data failure, `OBSERVATION_ONLY`
User record, no response, no reselection and cleanup.

An offline verifier reran the semantic checks against all five retained verdict
files and recomputed their SHA-256 values; it returned
`OFFLINE_REANALYSIS_PASS 5 verdict hashes and semantic gates verified`.

## Requirement and success-criteria mapping

FR-001/002/003/004/012/016/018 are covered by the strict profile, five-command
entrypoint, immutable candidate manifest, transport journal, closure ledger and
operator guide. FR-005/006/017 are covered by the layered base+APP contract,
base reuse, read-only `/app`, capacity and cache checks. FR-007/008/009/010/011/
019 are covered by the real MiniNDN and v56 verdicts: signed ACK/Selection,
four-role graph, encrypted NDN dependency Data, independent oracle, terminal
agreement, CUDA execution and signed per-device `free_memory_mb` offers. FR-013
is covered by jobs 210472 and 210474; FR-014 by the recorded gate order; FR-015
by mutation suites and job 210473.

SC-001 is the zero-side-effect mutation suite; SC-002 is the host/unit,
integration, MiniNDN, and exact-SIF local evidence; SC-003 is the eight successful
normal requests across jobs 210472 and 210474; SC-004 is their unchanged profile,
base, APP, harness, model and oracle identities with new allocations and GPU
UUIDs; SC-005 is the bounded `EXPECTED_REJECTION_PASS`; SC-006 is this file plus
the linked guide, matrix, checkpoint and failure log, all suitable for offline
reanalysis without editing retained receipts.

## Reproduction and limits

Use a clean checkout, the active `.specify/feature.json`, and the profile under
`/project/tma1/ndnsf-di/candidates/spec183-v56-20260911`. Run `check`,
`prepare`, then `local` or `submit`; after Slurm reaches a terminal state run
`collect --reconcile`. Keep the base SIF and model in the content-addressed
project cache; synchronize only changed app/harness planes when their hashes
change. Do not inject host libraries, write into `/app`, or build on Tiger.
Remove only the run's private temporary directories after retaining verdicts;
leave project-storage evidence and journals until reconciliation is complete.

The qualification proves correctness and reproducibility for this fixed small
YOLO graph. It does not claim a performance advantage or generalize to another
model, GPU type, driver, or ABI without requalification. Repository-wide
`pytest -q` remains blocked by the unrelated legacy gRPC replacement guard;
the dedicated TigerCluster suite is the applicable regression gate.
