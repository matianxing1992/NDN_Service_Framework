# Spec 116 Verification Evidence

All commands below exercised the candidate identified in
`candidate-identity.md`. Failures from diagnostic attempts are retained rather
than overwritten.

## Build and Deterministic Local Matrix

### Build

```bash
./waf build -j$(nproc)
```

Result: PASS. The build completed with only pre-existing third-party glibmm
deprecation warnings.

### C++ unit suite

```bash
./build/unit-tests --log_level=nothing --report_level=short
```

Result: PASS, 275/275 test cases and 46,200/46,200 assertions. The optional
environment-driven real-ONNX case remained outside this unit invocation.

### Spec 116 Python/API/security matrix

```bash
modules=$(find tests/python -maxdepth 1 -type f \
  -name 'test_ndnsf_di_*.py' \
  ! -name 'test_ndnsf_di_candidate_lineage.py' \
  -printf '%f\n' | sed 's/\.py$//' | sort | tr '\n' ' ')
PYTHONPATH=build/python:pythonWrapper:NDNSF-DistributedInference:tests/python \
  python3 -m unittest $modules test_ndnsf_collaboration_operation_status -q
```

Result: PASS, 484 tests run: 483 passed and one optional GUI test skipped.
This matrix covers public imports/signatures, ownership, signed definitions and
activation, catalog trust, request/deployment handles, remote coordinator
progress, generic Collaboration status, all-role readiness, cancellation,
restart, rollover/revocation/fencing, provider authority, ten optimizer seams,
compatibility, documentation snippets, and adversarial binding failures.

The broader `test_ndnsf_di_*.py` discovery ran 485 tests and reported exactly
one additional failure:
`test_ndnsf_di_candidate_lineage.CandidateLineageTest.test_frozen_historical_files_match`.
That frozen Spec 111 lineage fixture compares an unrelated modified
`specs/109-ndnsf-di-itiger-qwen-scaling/checklists/pre-implementation-audit.md`
hash. It is preserved as an unrelated worktree discrepancy and is not hidden by
changing its historical fixture.

After the completion re-audit corrected the documented coordinator-only
`optimization` argument and prose/status drift, the focused public API,
documentation, user journey, catalog, provider, optimizer, compatibility, and
Collaboration-status suite was rerun: 37/37 tests passed.

## MiniNDN Network Acceptance

### Exact-name signed deployment catalog

```bash
sudo -n env PATH="$PATH" \
  PYTHONPATH="$PWD/build/python:$PWD/pythonWrapper:$PWD/NDNSF-DistributedInference" \
  python3 Experiments/NDNSF_DI_Catalog_Minindn.py \
  --out results/spec116-minindn-catalog-completion-20260717-v2
```

Result: PASS. The Memphis Application publisher emitted the exact signed Data
name
`/example/hello/user/NDNSF/DI/DEFINITION/spec116/sha256:catalog-smoke`; the UCLA
requester fetched and verified the record. Authoritative result:
`results/spec116-minindn-catalog-completion-20260717-v2/summary.json`.

### Multi-provider readiness and execution

```bash
sudo -n env PATH="$PATH" \
  PYTHONPATH="$PWD/build/python:$PWD/pythonWrapper:$PWD/NDNSF-DistributedInference" \
  python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --full-network --requests 1 --concurrency 1 \
  --skip-provider-pair-telemetry-probe \
  --out results/spec116-minindn-readiness-completion-20260717-v2
```

Result: PASS (`status=SUCCESS`). Four separate providers served Backbone,
Head/Shard/0, Head/Shard/1, and Merge roles. All four produced readiness and
real ONNX Runtime CPU execution evidence. The one request succeeded with zero
timeouts, other failures, or negative ACKs. Eight dependency events completed
(four publish and four fetch). Authoritative result:
`results/spec116-minindn-readiness-completion-20260717-v2/summary.json`.

## Preserved Diagnostic Failures

The following earlier outputs remain as measured failures and document the
repair path:

- `results/spec116-minindn-readiness-barrier-20260717`
- `results/spec116-minindn-readiness-recovery-20260717`
- `results/spec116-minindn-readiness-certificate-20260717`
- `results/spec116-minindn-catalog-final-20260717`
- `results/spec116-minindn-catalog-diagnostic-20260717`
- `results/spec116-minindn-catalog-pass-20260717`

The readiness attempts exposed missing readiness/certificate completion. The
catalog attempts exposed shared-ownership `bad_weak_ptr` and exact URI binding
errors. Both classes were fixed before the two authoritative `-v2` runs.

## Scope Boundary

This is MiniNDN CPU acceptance. It does not claim Docker, Apptainer, iTiger,
CUDA, GPU, Qwen scaling, performance improvement, or production WAN readiness.
