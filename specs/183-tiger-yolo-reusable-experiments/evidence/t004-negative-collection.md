# T004.s — retained negative collection and two-rank wiring

2026-09-08; follows `af8022f7`. Status: IMPLEMENTED / component validation only.
T007 remains BLOCK pending complete production-path re-audit; T015 is NOT_RUN.

## Production path

The existing `submit → shared once-only sbatch → run → two srun ranks` path now
accepts `negative-dependency`. It consumes `twoNodeGpu` qualification, verifies
the frozen profile/candidate and uses the same provision/startup/completion,
GPU allocation/probe, node scratch, persistent cleanup and submission journal
owners. There is one request with no warmup or independent numerical reference.
The obsolete `NEGATIVE_RUNNER_NOT_WIRED` guard was removed after boundary tests.
No allocation was submitted by this work.

Provider startup enables `NDNSF_DI_RUNTIME_TIMING=1` for this case: source
`NativeProviderHandler::nativeTraceEnabled()` requires that switch (or the
assignment-fetch trace switch) to emit `NDNSF_DI_NATIVE_FAILURE`. Dependency
object tracing alone was insufficient. Only DetectShard0 receives the previously
implemented request-bound output withholding argument.

The shared collection handoff contains both node receipt/allocation/probe hashes
and the verified preparation/model placement bindings, with
`kind=expected-rejection` and `references=[]`. It cannot supply a rejection verdict.
`collect_negative_verdict` derives the outcome from:

- exact request/attempt/plan Selection lineage and post-shutdown User observation;
- verified public preparation and paired dependency projection;
- both closed node logs, clean children, independent allocation/GPU probes and
  distinct host/GPU identities;
- bound BackboneNeck and DetectShard0 native CUDA computation/profile evidence;
- exactly one native DetectShard0→Merge output cutpoint and Merge's failure to
  fetch that same exact MANIFEST, with no contradictory successful transfer;
- srun termination and node-storage cleanup checked by the public collector.

Cross-host wall-clock ordering is not inferred. The cutpoint's post-Selection
position follows the native request-bound V3 handler and pre-publication callback.
A plain timeout, caller-authored rejection, unrelated failure, missing node,
reselection, successful response or forced cleanup cannot supply this verdict.
Per V17, a protocol failure response is allowed only with the same independent
failure evidence; the previous helper incorrectly required absence of any response.

Public collection also normalizes JSON representation before immutable verdict
comparison. Rank dictionaries use integer keys in memory and string keys on disk;
without normalization, an unchanged retained verdict could fail its second read.

## Focused validation

Retained results: `Experiments/TigerCluster/results/spec183-negative-collection-20260908/`.

| File | Scope and outcome |
| --- | --- |
| readers-first.log | 22 passed, one fixture failed: rank1 inherited nfd0 instead of nfd1; corrected fixture only |
| wiring.log | 191 passed / 19.32s: retained negative readers/join, two-rank orchestration, handoff, CLI, node receipt, storage, application |
| public-entry.log | 52 passed / 3.12s: affected operator/shared-submit/allocated entry points; prerequisite failure prevents sbatch; same negative run submits once |
| final-boundaries.log | 12 passed / 0.93s: immutable JSON reanalysis and both-rank storage binding |
| response-contract.log | 21 passed / 1.58s: absent versus failed/successful response and full retained negative join boundaries |
| json-reader.log | 9 passed / 0.81s: bounded User/cutpoint JSON readers, including excessive nesting reported as collection rejection |

Groups overlap and must not be summed as unique tests. Tests read real retained
files and use the production validators; native computation, allocation and
process coordination fixtures are explicitly marked doubles. They are not
physical GPU, deployed native User, full SIF or YOLO qualification. Existing
native output-cutpoint tests were reused without rerunning them. No native build,
model reference execution, SIF refresh/transfer or GPU job occurred.

Workflow: Context Mode anomaly screen and repository authority health checked;
CodeGraph indexed/query-checked the collector/rank/public-entry closure; Spec Kit
contract/progress and GSD handoff updated. ARS is not applicable to this source
wiring change; no new statistical campaign was designed.

## Remaining

Perform T007's full design/code/field closure review before formal qualification.
The 28-file harness changed; final source seal/E/native consumers must reflect
current code. Base-SIF read-integrity investigation remains open. After the source
audit, follow host unit/integration → MiniNDN → exact-SIF local → single GPU →
normal two-node → this one negative case → unchanged normal reuse allocation.
