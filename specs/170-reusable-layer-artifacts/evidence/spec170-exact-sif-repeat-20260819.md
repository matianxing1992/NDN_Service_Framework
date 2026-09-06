# Exact r23 SIF D0/D1 repeat gate (2026-08-19)

This is a bounded reproducibility check for the retained r23 image and the
current D0/D1 workload cleanup contract. It is not a new SIF qualification and
does not claim that the dirty working tree is present in r23.

The parser/logger investigation and regression evidence are summarized in
[`spec170-dependency-trace-integrity-20260819.md`](spec170-dependency-trace-integrity-20260819.md).

## Fixed identity

```text
SIF: .codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif
SHA-256: 5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb
Apptainer: /opt/apptainer/1.5.3/bin/apptainer (1.5.3)
Bundle: .codex-tmp/spec170-container-build-20260818-r14/r18-network-bundle
Expected runner: onnxruntime-cpu
```

## Command and result

The exact-SIF D0 four-Provider and D1 single-Provider dependency tests were
run sequentially in three independent blocks with the same explicit SIF,
bundle, Apptainer version, and Python path:

```text
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments:pythonWrapper \
python3 -m pytest -q tests/python/test_spec170_real_minindn_gate.py \
  -k 'exact_sif_d0_four_provider_dependency_chain or exact_sif_d1_single_provider_dependency_chain'
```

| Block | D0 | D1 | Elapsed | Post-run Provider census |
|---:|---|---|---:|---|
| 1 | PASS | PASS | 20.77 s | zero |
| 2 | PASS | PASS | 20.94 s | zero |
| 3 | PASS | PASS | 21.01 s | zero |

Each block completed the real Controller → Provider bootstrap → User request
→ ACK → Selection → dependency fetch → Response path. The test also checked
that all four dependency edges were published and fetched, and that no
job-local `/opt/ndnsf-di/current/bin/di-native-provider` survived cleanup.

After the evidence parser was corrected to compare the actual `data_name` URI
(the Python trace uses `planned_name=true|false` as a marker), five further
combined D0→D1 blocks also passed with zero residual Providers. The exact-SIF
Python contract glob now reports 111 passed, 6 skipped, and one warning.

This closes the parser false-positive and the repeatability/cleanup concern
for the bounded exact-SIF D0/D1 gate. It does not close T028's full
lifecycle-fault corpus, T029's frozen candidate, T036 performance optimality,
or GPU/Tiger qualification.

## Preserved first failure

Before the repeated blocks, two combined D0→D1 invocations produced a D1
`dependency status=incomplete` assertion after D0 had passed. The retained
diagnostics show that the consumer fetched the expected bytes and the final
response succeeded, while one producer timing record was malformed or had an
empty/mismatched name. One later record contained `data_name=0.004`, which is
consistent with concurrent `std::cout` field interleaving rather than a failed
wire transfer. The source now serializes the complete NativeProvider timing
and capacity diagnostic block with a shared mutex; the host Provider target
builds successfully, but r23 predates this source change. A new source-bound
SIF must still verify the fix in the exact image before this warning can be
closed as deterministic runtime evidence.
