# Spec 137 Four-Core Redesign Audit

**Mode**: pre-implementation re-audit  
**Date**: 2026-07-23  
**Verdict**: PASS

## Controlling Correction

The rejected design incorrectly required exclusive CPUs for every NFD, pacer,
Face thread, and worker. That requirement was not requested and cannot execute
on the known four-vCPU host.

The corrected experiment is the minimum causal comparison:

```text
peer-a: sole active publisher
  face-serial vs one worker-serial
peer-b: fixed receiver
rate: fixed 60 pps
subject: one source tree, one build, one binary
```

The four CPUs serve the whole experiment. The worker is one thread using CPU 3
only in `worker-serial`; it does not require four or eight cores.

## Audit Findings

- **Intent fidelity**: PASS. The only treatment is the location of serial Sync
  Interest production in the sole publisher.
- **Occam necessity**: PASS. The rate-search grid and symmetric second worker
  were removed. One fixed diagnostic pair precedes six paired formal cells.
- **Code reality**: PASS. The benchmark exposes the runtime production mode and
  a fixed publisher/receiver role. The runner starts one publisher worker only.
- **Four-core executability**: PASS. Both NFDs share CPU 0, publisher
  pacer/Face share CPU 1, receiver uses CPU 2, and the sole worker uses CPU 3.
- **Evidence integrity**: PASS. The three earlier preflights remain immutable
  as superseded design evidence; a new campaign and rebuilt binary are
  required.
- **Validation**: PASS for implementation readiness. Python syntax and 13
  contract tests pass. Real MiniNDN preflight and formal evidence remain to be
  executed and cannot yet be described as measured.

No unresolved CRITICAL or HIGH finding remains. This PASS authorizes a fresh
four-core build and preflight; it does not authorize reuse of an old result.

## Tool Note

The repository's configured agent-context update script path is absent. The
existing Spec Kit strict structural audit and prerequisite resolver both
passed; this missing helper is a workflow-tool issue, not experiment evidence.
