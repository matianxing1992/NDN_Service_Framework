# Audit History

## Current disposition

The original pre-implementation `BLOCK` below is retained as a historical
snapshot. It was resolved by the coherent replacement runtime, local and
iTiger operation-status smoke gates, and Live Attempt 015. The final
requirement-by-requirement verdict is `PASS` in
[completion-summary.md](completion-summary.md).

Job `174221` itself remains recorded as `FAILED (1:0)`: the real three-node
workload passed every T004d technical condition before an incorrect post-run
request-ID assertion failed. The user selected the documented no-rerun
evidence-acceptance path; no terminal state or negative evidence was rewritten.

## Original pre-implementation audit

## Verdict

`BLOCK`

The documents now preserve the user's actual multi-node goal and the measured
runtime failure boundary. T002, T003, T004b, and T004c are closed. Live Attempt 005 and
follow-up diagnostics proved the previous live runtime was a mixed SIF plus
externally mounted replacement native libraries, Python extensions, and
vendor-site dependencies. T004b has now rebuilt a coherent local app-runtime
image and shown that the same provider-constructor path no longer aborts with
allocator corruption. T004c has materialized that runtime as a clean SIF and
passed a Slurm runtime smoke. The Spec remains blocked only for formal live
Qwen inference until T004d runs the three-node secured collaboration request.

## Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A-001 | BLOCK | Live inference | Clean Runtime Build 001; Archive Materialization Attempt 001; Runtime Smoke Attempt 003 | The failed mixed-runtime path has been replaced by a coherent local image, an archive-materialized SIF, and a Slurm runtime smoke that constructs controller/provider/user and completes `/HELLO` without allocator corruption. Three-node Qwen collaboration has not yet been run on this SIF. | T004d must use only `/project/tma1/ndnsf-di/releases/spec160-178122a30d0a-archive/runtime.sif`, frozen stage artifacts, and writable evidence mounts for the formal live request. |
| A-002 | MEDIUM | Code reality | `llm_pipeline_lib.py:1598,1888-1947` | Existing stage model and decoded tensors previously required explicit CUDA placement to satisfy FR-004. Stage Attempt 002 proved generated stage packages load on CUDA, but the live path remains blocked before model preload. | Retain fail-closed CUDA validation in the rebuilt runtime smoke and live T004d acceptance. |
| A-003 | LOW | Tooling | `.specify/extensions/agent-context/` | The configured update script is absent although its skill documents the path. | The active AGENTS.md plan pointer was updated directly; repair the extension separately if desired. |

## Readiness Scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | One request, three physical nodes, real layer pipeline |
| Architecture and ownership | Yes | Runtime ownership moved to a coherent local app-runtime image and archive-materialized SIF |
| Security/correctness | Yes | Normal secured collaboration path; fail closed |
| Task executability | Yes | Runtime rebuild, smoke, and live gates are separated |
| Task cohesion/granularity | Yes | Mixed-runtime evidence, rebuild, smoke, live run, and closure are independently meaningful gates |
| Validation/evidence | Blocked | T002/T003/T004b/T004c passed; live inference is blocked until T004d passes |
| Migration/rollback | Yes | Failed mixed-runtime evidence is preserved; replacement runtime must be immutable per attempt |
| Code reality | Partial | Clean SIF passes Slurm controller/provider/user smoke; three-node Qwen GPU path remains unverified |

## Metrics

- User stories: 3
- Functional requirements: 15
- Success criteria: 6
- Tasks: 8
- Mechanically fragmented task groups: 0
- Requirement coverage: 15/15
- Unmapped tasks: 0
- Critical / High / Medium / Low findings: 0 / 0 / 1 / 1 plus 1 remaining BLOCK

## Evidence limits

Spec 160 has submitted transport, artifact, live, and diagnostic Slurm jobs.
Cross-node NFD TCP/UDP transport passed in Attempt 016, and Qwen stage artifact
CUDA loadability passed in Stage Attempt 002. Three-node live inference has not
passed. Live Attempt 005 reached NFD routing and provider launch markers, then
failed during native provider construction before Qwen model preload; it is
mixed-runtime evidence only, not Qwen execution evidence. Clean Runtime Build
001 produced local image
`ndnsf-di:spec158-spec160-clean-runtime-20260727T215320Z`
(`sha256:16b99a0b82fb6ae4e98b1767cca3627eb1e322ed9a1493698666546caa5cbf69`)
and passed local static probe plus provider-constructor smoke. Archive
Materialization Attempt 001 produced SIF
`/project/tma1/ndnsf-di/releases/spec160-178122a30d0a-archive/runtime.sif`
(`sha256:8d8d95e7a5de86036ad62d972b911a66f2160553c6b3295d029dd3e7ae26106f`),
and Runtime Smoke Attempt 003 passed in Slurm. None of this is yet three-node
Qwen execution evidence.
