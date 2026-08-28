# Spec 160 Completion Summary

## Verdict

`PASS` for the scoped three-node NDNSF-DI Qwen layer-pipeline capability.

Live Attempt 015 executed one secured collaboration request on three physical
iTiger nodes and three distinct RTX 5000 Ada GPU UUIDs. Both planned hidden
states crossed node boundaries, all three stages used CUDA without CPU
fallback, and the requester received the frozen-reference top token and shape.

Slurm job `174221` remains immutably recorded as `FAILED (1:0)`, elapsed
`00:07:07`. The workload had already completed successfully; the failure came
from a post-run analyzer comparing the correct indexed application request ID
`spec160-qwen-live-015-0` against the outer identity
`spec160-qwen-live-015`. The corrected analyzer passes read-only against the
same evidence. The user selected the no-rerun evidence-acceptance path, so this
summary does not relabel the job, promote its partial directory, or create
Attempt 016.

## Accepted runtime evidence

- Submission/run/job: `spec160-qwen-live-015` /
  `spec160-run-qwen-live-015` / `174221`.
- Candidate SIF SHA-256:
  `6638c813016e151fd6c19825db8dc01e3a326c23716084677eee3c4da935d52e`.
- Source bundle SHA-256:
  `ec053168605107dbb71a2bbb6e2ffb22eca6d4aadfde51f92ddc9ea4869ab3eb`.
- Placement:
  - Stage 0 `[0,8)` on `itiger07`,
    GPU `GPU-25e44831-503d-7227-29f7-d2346fbb1d80`.
  - Stage 1 `[8,16)` on `itiger08`,
    GPU `GPU-b76fa0fa-afae-8df6-a44c-158ccf9aa38b`.
  - Stage 2 `[16,24)` on `itiger09`,
    GPU `GPU-84ba2af4-da27-b890-5bbd-f3bd1d33e718`.
- All stages: `device=cuda:0`, `cpuFallback=0`.
- Stage 0 output / Stage 1 input SHA-256:
  `784e075ed8db8a28ac108ae0849353f1a990afb164b1938c1b0a73fce0a134bf`.
- Stage 1 output / Stage 2 input SHA-256:
  `d038b303945a672acadf6f7c04df9fc13c8c194817ed14e2b865cdceffa2170f`.
- Requester: `status=ok`, one measured request, `2794.666 ms`.
- Frozen and returned top token: `27024`.
- Returned logits shape: `[1, 5, 151936]`.
- Evidence integrity: all 58 original checksum rows pass. No bootstrap token,
  private-key file, mounted replacement native library/binding/vendor-site, CPU
  fallback, CUDA-unavailable marker, allocator corruption, or old
  `reportOperationStatus` pybind error is present.

Remote immutable evidence:
`/project/tma1/ndnsf-di/evidence/spec160/qwen-live/.spec160-qwen-live-015.partial/`.

Local mirror and post-hoc classification:
`results/spec160-itiger-multinode-qwen/qwen-live-015-analysis-gate-fail/`.

## Functional requirement audit

| Requirement | Verdict | Evidence |
|---|---|---|
| FR-001 | PASS | Job 174221 maps one stage each to three distinct nodes and GPU UUIDs. |
| FR-002 | PASS | Accepted allocation-scoped TCP/UDP probe job 173264 preceded live model execution. |
| FR-003 | PASS | Frozen revision `7ae557...a775` and ranges `[0,8)`, `[8,16)`, `[16,24)` are present in the runtime summary and response. |
| FR-004 | PASS | Three timing records show `cuda:0` and `cpuFallback=0`; the provider retains fail-closed CUDA loading. |
| FR-005 | PASS | Existing REQUEST/ACK/SELECTION, collaboration assignment, segmented dependency, and RESPONSE paths executed; CodeGraph shows no Qwen Core bypass. |
| FR-006 | PASS | Stage 0→1 and Stage 1→2 completed across `itiger07→08→09`; producer outputs equal consumer inputs by SHA-256. |
| FR-007 | PASS | Requester log records the normal secured flow and one successful final response. |
| FR-008 | PASS | Evidence correlates outer/indexed request identity, NDNSF request/session name, role, provider, host, GPU UUID, range, CUDA backend, Data name, digest, byte/segment counts, and timestamps. |
| FR-009 | PASS | Attempt 015 uses one coherent replacement SIF; runtime contract records no mounted replacement native libraries, bindings, or vendor-site. |
| FR-010 | PASS | Probe, artifact preparation, runtime smoke, and inference ran under bounded Slurm jobs; no login-node compute was used. |
| FR-011 | PASS | Attempt 015 was submitted once, linked to job 173319, preserved at its first terminal state, and was not retried; Attempt 016 does not exist. |
| FR-012 | PASS | Token/private-key scans are empty; bootstrap tokens and job-local key material were removed before checksums. |
| FR-013 | PASS | This is reported only as one layer-pipeline capability run, not tensor/model parallelism, throughput, scaling, or production evidence. |
| FR-014 | PASS | Formal live mounts are limited to the coherent SIF, read-only source/config/model artifacts, job-local scratch, and evidence; no external `.so`, pybind module, or vendor-site override is mounted. |
| FR-015 | PASS | Attempt 005 and constructor diagnostics remain labeled mixed-runtime pre-model failures and are not reclassified as Qwen/algorithm failures. |

## Success-criterion audit

| Criterion | Verdict | Evidence |
|---|---|---|
| SC-001 | PASS | Probe job 173264 exchanged signed named Data over TCP and UDP among the three allocated GPU nodes. |
| SC-002 | PASS | Job 174221 executed the three frozen ranges on three distinct GPU UUIDs with zero CPU fallback. |
| SC-003 | PASS | Both planned objects crossed physical boundaries; their producer/consumer identities, Data names, segments, and SHA-256 values match. |
| SC-004 | PASS | Requester top token `27024` and shape `[1,5,151936]` match the frozen reference. |
| SC-005 | PASS | Node/GPU/role mapping, one indexed request, three stage records, two digest chains, final response, checksums, and failure boundary reject the listed false positives. |
| SC-006 | PASS | The repaired coherent SIF passed local Docker and Slurm single-node operation-status collaboration gates before Attempt 015, without allocator corruption or runtime overrides. |

## Preserved negative evidence

- Live Attempt 005 remains a mixed-runtime native-constructor failure before
  model preload or request execution.
- Live Attempt 014 remains an operation-status Python binding failure after the
  request reached all three providers.
- Materialization and smoke harness failures remain recorded with their exact
  causes; none is silently discarded.
- Live Attempt 015 retains formal `FAILED (1:0)` even though its workload
  evidence satisfies T004d. The distinction is explicit in `POSTHOC.md`,
  `tasks.md`, `preflight.md`, and this summary.

## Scope boundary and next step

This closes Spec 160 only. It proves a single secured three-node Qwen
layer-pipeline execution. It does not prove tensor/model parallelism,
performance improvement, throughput scaling, resilience, production
readiness, or release qualification.

The next worthwhile step is review/archival of Spec 160. Any performance or
multi-request campaign must be a new feature with a separate statistical
experiment plan; it must not reuse this one-run capability result as a
baseline claim.
