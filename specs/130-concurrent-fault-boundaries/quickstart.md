# Spec 130 Validation Quickstart

## Planning and audit gates

```bash
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  specs/130-concurrent-fault-boundaries --strict
.specify/scripts/bash/check-prerequisites.sh \
  --json --require-tasks --include-tasks
```

The withdrawn central-coordinator implementation and old `PASS` audit do not
authorize execution. Start implementation only from the redefined `tasks.md`
after the new code-aware audit has no blocking finding.

## Focused gates

The implementation phase must provide focused commands for:

- late ACK after another Provider was selected and after callback closure;
- two maintained-client Requesters with production full-jitter retry;
- long-running execution pin, renewal loss, stop-before-release and stale
  completion;
- maintained client/provider chain and fork/join dependency execution;
- generic Core/binding negative scans for DI policy;
- ordinary non-DI ACK compatibility and Spec 129 security invariants;
- new runner manifest/topology/fault/evidence validation.

Expected test modules are recorded in `tasks.md`; exact commands and counts are
written to `implementation-evidence.md` after implementation.

## Full source and binding gate

Use the repository's established full C++ build/test path and force an in-place
Python binding/package rebuild. A Python import-only check is not a binding
rebuild. Record exact commands, environment, hashes and results. Do not run a
formal MiniNDN cell until these gates and the Spec 129 hash preflight pass.

## Formal confirmation

Dry-run freezes and validates the new sixteen-cell manifest without launching
MiniNDN:

```bash
python3 Experiments/run_spec130_boundary_repair_matrix.py \
  --output results/spec130-boundary-dry-<timestamp> --dry-run
```

Formal execution uses a fresh output directory:

```bash
sudo -E python3 Experiments/run_spec130_boundary_repair_matrix.py \
  --output results/spec130-boundary-confirmation-<timestamp> \
  --require-minindn
```

The runner must refuse existing output, concurrent writers, shared-host/NFD
actors, missing real-fault evidence, manifest/source drift and Spec 129 hash
drift. It runs each cell once and performs no automatic retry. Never invoke the
Spec 129 runner.

## Required artifacts

- `campaign-summary.json`
- `campaign-runs.csv`
- `campaign-cells.csv`
- frozen manifest and source hashes
- Spec 129 before/after hash report
- topology, host/identity/NFD/PID and route manifest
- per-cell real fault action/effect record
- message, late-liability, reservation, pin, retry and dependency ledgers
- analyzer report with Payload, Mapping, new-Mapping-information ratio, ACK,
  SELECTED/NOT_SELECTED, receipt, retry, timeout, Nack/rejection, pin/release,
  stage, execution, safety, availability and terminal cause
- explicit unavailable values where a metric denominator does not exist
