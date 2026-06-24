# NDNSF Constitution

## Core Principles

### I. Canonical Dynamic Runtime

NDNSF development centers on the generic dynamic C++ runtime API. New work must
use unified `serviceName` paths, V2 request/ACK/selection/response naming, and
the current Targeted terminology for known-provider low-latency calls. Do not
reintroduce generated service/stub classes, split `ServiceName + FunctionName`
APIs, Direct terminology, or framework-specific HELLO wire types.

### II. Security Is Part Of The Data Path

Authorization and execution must preserve the current NAC-ABE, permission,
token, replay-protection, and provider-permission checks. Permission discovery
Data is controller-signed and encrypted to the target identity certificate.
REQUEST and SELECTION map to `/SERVICE/<service>`, while ACK and RESPONSE map to
`/PERMISSION/<service>`. Do not add debug bypasses such as forced
`isAuthorized = true`.

### III. CodeGraph First, Source Verified

For code questions, impact analysis, bug tracing, and source edits, start with
CodeGraph in this indexed repository. Use it to find symbols, callers, callees,
and affected files, then verify final claims against the actual source and
targeted tests. Use `rg` after CodeGraph for exact strings, scripts, logs,
configs, and docs.

### IV. Spec-Driven Changes For Durable Work

Use Spec Kit for new features, protocol/API changes, architecture changes,
evaluation-plan changes, and multi-file work that needs durable requirements.
The normal path is specify, clarify when needed, plan, tasks, analyze when
useful, implement, and converge. Small one-line fixes, direct command-output
requests, and narrow slide/text edits may use the direct Codex workflow.

### V. Verify With The Right Scope

Testing must match risk. NDNSF security and runtime changes require the focused
regression scripts plus relevant unit tests. Shared protocol or distributed
inference changes require broader unit tests. Network, security, and
performance regressions should use MiniNDN by default; host NFD is only a
temporary diagnostic path unless the user asks otherwise. Performance short
tests should keep a 60-second measured window unless explicitly changed.

## Project Constraints

GSD Core is installed for Codex and should be used for long-running,
multi-phase, unclear, or stateful work: VM setup, benchmark campaigns, major
protocol changes, distributed-inference work, and proposal-wide slide
revisions. Use GSD's discuss, plan, execute, verify, progress, and resume skills
to keep work scoped and recoverable across context windows.

Keep `results/` as local experiment output, not source of truth. Preserve only
canonical reproduction runs or the latest result for a distinct diagnostic
scenario once the finding is documented.

When README documentation is updated, keep Chinese and English versions in sync
when both exist.

## Development Workflow

1. Check the working tree before edits and never revert unrelated user changes.
2. For code work, start with CodeGraph unless the task is a literal text/doc
   lookup.
3. Use Spec Kit before implementing durable feature or architecture work.
4. Use GSD for multi-phase work that needs explicit state, verification, or
   recovery.
5. Prefer MiniNDN for final NDNSF network/security/performance validation.
6. After completion, summarize changed files, verification, residual risk, and
   the next best step.
7. Play the 1-second completion bell after success or failure.

## Governance

This constitution supplements `AGENTS.md`. If the two disagree, the more
specific NDNSF rule in `AGENTS.md` wins. Amend this constitution when project
workflow, security invariants, or required validation gates change. Spec Kit
artifacts must not override NDNSF runtime/security rules unless the user
explicitly approves the change.

**Version**: 1.0.0 | **Ratified**: 2026-06-24 | **Last Amended**: 2026-06-24
