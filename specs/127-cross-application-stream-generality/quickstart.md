# Quickstart: Spec 127 Validation

## Prerequisites

```bash
codegraph status .
node /home/tianxing/.codex/gsd-core/bin/gsd-tools.cjs validate health
sudo -n true
./waf build -j$(nproc)
```

## Deterministic gates

```bash
./build/unit-tests --run_test=Stream --log_level=message
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_core_streaming.py
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_live_stream_generality.py
python3 tests/python/test_spec127_cross_application_runner.py
python3 tests/run_uav_stream_security_contract.py
```

Expected: both workload manifests are deterministic; complete-sample receipts,
pause/resume, extent transitions, Mapping delay/staleness, stop fencing, and
malformed/replayed Data pass; scoped source inspection finds no application-
specific Core change.

## Dry campaign

```bash
python3 Experiments/run_spec127_cross_application_matrix.py \
  --output-root results/spec127-cross-application-dry \
  --duration-seconds 60 --dry-run
```

Inspect 12 unique commands, workload/source/history hashes, deterministic
ordering, effective qdisc plans, required fields, no automatic retry, and
single-writer preflight before live execution.

## Frozen campaign

Run one newly named output root exactly once. Do not select or replace failed
cells. A source defect requires deterministic reproduction and a complete new
12-cell confirmation after the repair.

## Closure gates

```bash
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  specs/127-cross-application-stream-generality --strict
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
git diff --check
```

Compare every measured result with SC-001..SC-010 and the evidence contract.
