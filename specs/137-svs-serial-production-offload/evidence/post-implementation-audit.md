# Spec 137 Post-Implementation Audit

**Date**: 2026-07-23  
**Mode**: post-implementation  
**Verdict**: BLOCK

## Findings

### HIGH — The formal campaign cannot support the requested necessity claim

Formal receipts 01 and 02 both fail the pre-registered attempted-rate
fidelity gate. The outcome table therefore classifies the campaign
`INADMISSIBLE`. No cell may be retried or replaced.

### HIGH — The sealed analyzer does not implement the complete T006 output contract

The analyzer frozen before formal execution verifies preflight and receipt
integrity, but it does not emit the registered run, paired, stage, and traffic
CSV files or apply the complete decision table. Modifying it now would violate
the sealed analyzer hash. The tracked report therefore labels its paired
calculations descriptive and does not claim a clean frozen-analyzer
reproduction.

### MEDIUM — The treatment proves a combined mechanism only

`worker-serial` combines asynchronous queueing and execution on one worker.
`publishAsync()` is common to both treatments. The experiment can evaluate the
combined asynchronous worker-offload mechanism, but it cannot identify
separate worker and asynchronous effects.

## Verified Evidence

- One exact source commit, one build, and one binary.
- Four-core MiniNDN execution with one publisher, one receiver, and one
  treatment worker.
- 13/13 focused contract tests.
- Preflight admitted all checks.
- Fixed 60 pps diagnostic admitted both modes.
- Six formal cells ran exactly once and produced six unique receipts.
- Every cell reports one maximum active Sync signer, zero fallback, complete
  accounting, and 100% delivery.
- Both admissible pairs show about 55% lower Face production CPU with the
  worker.
- The admissible pairs do not show the required repeatable heartbeat or
  delivery-tail improvement.

## Gate Decision

T001–T005 are complete. T006 remains open. Spec 137 must not be described as
proof that workerization or asynchronous offload is necessary. A new experiment
would need a new Spec and must preserve this campaign unchanged.
