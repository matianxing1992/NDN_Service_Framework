# Quickstart: Spec 126 Validation

## Prerequisites

```bash
codegraph status .
node /home/tianxing/.codex/gsd-core/bin/gsd-tools.cjs validate health
sudo -n true
./waf build -j4
```

## Deterministic gates

```bash
./build/unit-tests --run_test=Stream,UavProtocolState --log_level=message
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_core_streaming.py
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_uav_unified_video.py
python3 tests/run_uav_stream_security_contract.py
```

Expected: all recovery/reorder permutations, session-stop fencing, malformed
Data, bounded state, wire golden vectors, and C++/Python status parity pass.

## Campaign preflight

The campaign command is generated and frozen by
`Experiments/run_spec126_loss_reorder_matrix.py`. Run its preflight/dry
manifest mode first and inspect 16 unique commands, both endpoint qdisc plans,
the source hash, and the Spec 125 evidence hash manifest.

Do not start the live matrix unless deterministic gates pass, no other MiniNDN
launcher/cleanup owner exists, and every output directory is unused.

## Frozen matrix

Invoke the frozen campaign once. Do not rerun a failed command. The wrapper
must preserve each outcome and produce `campaign-summary.json` and
`campaign-runs.csv`.

## Acceptance review

```bash
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  specs/126-loss-reorder-resilience --strict
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
git diff --check
```

Compare results with `spec.md` SC-001 through SC-008. A failed threshold is a
negative result, not authorization to tune or rerun the frozen cell.
