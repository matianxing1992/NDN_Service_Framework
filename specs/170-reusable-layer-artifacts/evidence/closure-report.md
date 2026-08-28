# Spec 170 closure report (2026-08-19)

## Scope and verdict

This report is the current-source audit of the final objective: protocol
completeness, negative coverage, and performance optimality. It is conservative:
qualification evidence is not promoted to a freeze or an optimum claim.

**Overall status: BLOCK.** The current source has strong local protocol and
named-negative qualification, but T029 is not frozen, the complete T018/3B/3C
lifecycle matrix is not closed, and T036's publication-quality performance
analysis is missing.

The local r23 SIF is usable only for its sealed source revision
`989a9daace669a4f93496dade3176c527edb2469` (SIF SHA-256
`5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb`). It is
not evidence for the uncommitted working tree.
The latest current-source local rerun is recorded in
[`current-local-regression-20260819.md`](current-local-regression-20260819.md).

## Gate summary

| Axis | Verdict | Current evidence | Remaining condition |
|---|---|---|---|
| Local C++ protocol regression | PASS (qualification) | 496/496 unit cases, 60,009/60,009 assertions; 34/34 integration cases, 480/480 assertions; provider-local epoch-key access and delayed-cancellation negatives included | Does not cover every Spec170 requirement or freeze identity |
| Real CPU-ONNX production D2b | PASS (local qualification) | Core fixture: 26/26 cases, 387/387 assertions; fixture SHA `6662d53f...db1ee53`; current host NativeTracer bundle-materialization rerun: 1/1 request, 4/4 dependency edges | No CUDA/Tiger equivalence; source still needs a new SIF before promotion |
| Exact r23 SIF D0/D1 lifecycle | PASS (bounded, repeated; warning) | Explicit Apptainer version, CLI, three earlier plus five post-parser-fix D0/D1 blocks: 16/16; `spec170-exact-sif-repeat-20260819.md` | Earlier combined runs had malformed producer timing evidence; r23 predates the source logger serialization fix, so deterministic release behavior remains unverified |
| DATA_V1 protocol unit coverage | PASS (named cases) | `DistributedInferenceCrossProviderGroup`: 9/9 cases, 722/722 assertions; 50-seed fault matrix included | Full production key-wrap/zeroization and 3A/3B/3C lifecycle remain open |
| Production SVS bridge faults | PASS (named cases) | tamper 9/9, drop 7/7, duplicate 13/13, reorder 13/13; positive 12/12 | Bridge-level evidence is not the complete cross-Provider fault corpus |
| Python Spec170 contracts | PASS (covered subset) | Fresh host glob 107/10; latest exact-SIF glob 111/6; see `spec170-python-contract-rerun-20260819.md` | Remaining skips are real NativeTracer/Qwen environments; no claim of full feature completion |
| Negative coverage | PARTIAL | peer mismatch, replay, partial output, unauthorized NAC-ABE, bounded loss/no-progress, tamper/SVS faults, terminal epoch-key access denial, and 77/78 auxiliary Python negative tests | Current-source baseline inventory still has eleven frozen-hash mismatches; complete T028/T037 cancellation, revocation, key lifecycle and 3C mutation matrix missing |
| Candidate source/build coverage | BLOCK | A 1,102-file pre-freeze runtime/build/harness/test/job inventory now exists, but model/canonical/security/route/schedule/Gate A/B/C inputs are not yet bound by T029; see `spec170-candidate-input-inventory-20260819.md` | Bind all executable and evidence inputs before T029 |
| Candidate freeze | BLOCK | No `frozen-candidate.json` or `freeze-report.md` | T025–T028 must pass first |
| Performance optimality | BLOCK | Spec171 60/60 descriptive cells; Spec173 v2 three-repetition rows | No T036 three-block P01–P05 corpus, hierarchical bootstrap, TOST or Holm |

## Reproducible local commands

```text
./build/unit-tests --report_level=short --log_level=error
  496 cases; 60,009 assertions; PASS (taskset -c 3)

./build/integration-tests --report_level=short --log_level=error
  34 cases; 480 assertions; PASS

NDNSF_DI_TEST_ONNX_MODEL=/tmp/ndnsf-di-onnx-integration.sB0BBP/linear.onnx \
  ./build/integration-tests --run_test=Spec170NdnsfDiCoreFlow \
  --report_level=short --log_level=error
  26 cases; 387 assertions; PASS

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference \
  python3 -m pytest -q tests/python/test_spec170_*.py
  107 passed; 10 skipped; 1 non-fatal warning (latest rerun)
```

The full detailed negative index is
[`security-failure-matrix.md`](security-failure-matrix.md); protocol scope is
in [`protocol-closure-audit-20260819.md`](protocol-closure-audit-20260819.md);
performance scope is in
[`performance-closure-audit-20260819.md`](performance-closure-audit-20260819.md).

## Performance conclusion

The one-Provider confirmatory corpus measured NDNSF slower than gRPC and NSC.
The matched Spec171 mobility corpus is complete for its registered 10-seed,
100/150 m, 2 m/s conditions, but its verdict is
`DESCRIPTIVE_RANGE_SPEED_MATRIX_ONLY`; the exploratory NDNSF-minus-gRPC
interval includes zero at both ranges. The separate 100 m opportunity holdout
supports a lower NDNSF p95 only within the switch-required subset, while its
all-request successful mean is still above gRPC. Therefore there is no
evidence for a global NDNSF optimum. The defensible claim is conditional and
range/switch-window dependent.

## Required closure sequence

1. Close the remaining production T018/3A/3B/3C and lifecycle negative rows.
2. Build an exact-source SIF for the current working tree and run Gate A/B/C.
3. Create and verify the T029 freeze manifest and post-freeze hash rejection.
4. Only then run T036's registered three-block P01–P05 workload and analysis.
5. Update this report and `traceability.md`; until then, retain status BLOCK.
