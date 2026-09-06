# Spec170 pre-freeze candidate-input inventory (2026-08-19)

This is a deterministic pre-freeze inventory, not `frozen-candidate.json` and
not a T029 completion record. It closes a specific audit gap: the source-only
SIF archive covers runtime trees, while the formal candidate must also bind
build definitions, experiment harnesses, tests, jobs, and configuration.

## Reproduction

```bash
python3 tools/ndnsf-di/collect_spec170_candidate_inputs.py \
  --workspace . \
  --output /tmp/spec170-candidate-input-inventory-20260819-r2.json
```

Observed output:

```text
schemaVersion:       spec170-candidate-input-inventory-v1
sourceRevision:      989a9daace669a4f93496dade3176c527edb2469
fileCount:            1102
worktreeStatusRows:   4170
inventoryDigest:      sha256:5ff13dcd5b6895481059a1c0074cb272ffb8e16bf890b317b081238a56715071
status:               PASS
```

The inventory includes the runtime C++/Python trees, all `examples`, the
candidate-relevant MiniNDN/performance harnesses, Apptainer/build scripts and
definitions, native/Python/unit/integration/container tests, and the Spec170
contracts/jobs/plans. It records each file's relative path, role, size,
SHA-256, mode, and executable bit. It excludes `.codex-tmp`, `results`, build
outputs, caches, generated Python bytecode, and native build products.

## Interpretation

The inventory is materially broader than the 294-row runtime source seal and
is the correct input for reviewing source/build/harness coverage before a new
SIF. It does not itself prove that every model, canonical artifact, prompt,
security manifest, route, schedule, Gate A/B/C output, or staged SIF is bound;
those remain separate T029 sections. It also captures the current dirty
worktree status and therefore cannot be treated as a frozen candidate until
the deliberate source review, Gate A/B/C closure, and exact-current-source SIF
are complete.

The inventory tool is covered by
`tests/python/test_spec170_candidate_input_inventory.py` (2/2 tests), and its
generated digest must change when an included input changes.
