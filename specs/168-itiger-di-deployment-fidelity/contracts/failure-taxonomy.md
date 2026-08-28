# Failure Taxonomy and Repair Contract

Each admitted invocation has exactly one primary terminal class. Contributing
conditions may be listed separately but do not replace the primary boundary.

| Code family | Boundary | Examples |
|---|---|---|
| `ENV_*` | Cluster/environment | allocation, VPN, node, GPU hardware, scratch, quota |
| `BOOT_*` | Process/security bootstrap | NFD unavailable, permission fetch, certificate/NAC-ABE setup |
| `ROUTE_*` | NDN forwarding | missing face/route/strategy, wrong namespace, unreachable producer |
| `ACK_*` | Capability collection | no valid ACK, late/wrong request, invalid signature, stale boot epoch |
| `PLAN_*` | Graph/placement | infeasible cut, capacity overflow, invalid dependency, catalog mismatch |
| `REPO_PUBLISH_*` | Artifact publication | manifest/data unavailable, digest/catalog activation failure |
| `REPO_FETCH_*` | Artifact delivery | stalled range, retry/window exhaustion, corruption, incomplete bytes |
| `PREP_*` | Provider preparation | verify, disk, RAM, adapter, CUDA load/warmup, false residency |
| `DEPENDENCY_*` | Dataflow readiness | missing/wrong/replayed predecessor data, local queue exhaustion |
| `EXEC_*` | Model stage | shape/backend/CUDA/model execution, CPU fallback |
| `TOKEN_*` | Generation loop | ordering, tokenizer, EOS/max-token accounting, per-token recollaboration |
| `RESPONSE_*` | Terminal delivery | incomplete answer, auth failure, wrong request, response timeout |
| `CLEANUP_*` | Terminal cleanup | leaked request resources, accidental reusable-cache eviction |
| `ANALYZER_*` | Evidence tooling | schedule mismatch, parser/schema error, post-hoc analyzer defect |

## Classification rules

1. Classify at the earliest lifecycle boundary whose independently retained
   evidence proves the failure.
2. A generic timeout is not sufficient. Include the last authenticated progress
   checkpoint and the operation/range/stage that stopped.
3. Do not classify a Repository fetch failure as model inference or CUDA failure
   because a later stage never ran.
4. Do not classify scheduler/VPN/allocation failure as NDNSF-DI correctness.
5. Do not classify an analyzer-only false negative as a runtime failure; retain
   both the original wrapper verdict and corrected immutable post-hoc analysis.
6. Unknown evidence gaps use `UNRESOLVED_EVIDENCE_GAP`, not a guessed component.

## Repair chain

For an NDNSF-DI or lower-layer logic defect, the record must contain:

```text
original experiment/job and immutable evidence
minimal reproducer
primary owner and root cause
repair source identity
focused regression
real MiniNDN result
exact-container result
new remote candidate identity
TigerCluster requalification result
```

A new remote submission is prohibited until the reproducer fails before and
passes after the repair under the local admission path.

## Retry semantics

Bounded wire retransmissions inside one operation retain the same request and
operation identity and are counted. Exhaustion produces one terminal failure.
A formal campaign rerun or changed candidate is a new experiment identity and
cannot replace the original row.
