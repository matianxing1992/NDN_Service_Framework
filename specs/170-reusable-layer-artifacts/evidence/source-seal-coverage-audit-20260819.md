# Current source-seal coverage audit (2026-08-19)

## Scope

This is a read-only audit of the source-only preparation probe. It does not
declare a T029 candidate or authorize a SIF build.

The preparation tool intentionally seals the runtime source trees and selected
Waf graph inputs, while T029 must additionally bind the build definition,
packaging scripts, experiment harnesses, tests, model/configuration, and
evidence manifests. These two scopes must not be conflated.

## Working-tree coverage at probe capture

```text
tracked dirty paths:       168
untracked paths:            325
runtime source seal rows:   294
NDN-SVS dependency rows:     31
dirty tracked paths sealed: 42
dirty tracked paths outside:126
untracked paths sealed:       8
untracked paths outside:    317
```

These counts are a probe snapshot; subsequently written evidence files may
increase the untracked count without changing the source-seal conclusion.

The source-only archive and NDN-SVS archive both validate successfully, and
their path-independent seal is reproducible. However, important changed paths
outside that archive include:

```text
examples/App_Provider.cpp
Experiments/NDNSF_DI_NativeTracer_Minindn.py
Experiments/NDNSF_NewAPI_Minindn_Perf.py
packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh
packaging/ndnsf-di-container/lib/spec170_sif_build_boundary.py
tests/integration-tests/ndnsf-di-core-flow.t.cpp
tests/python/test_spec170_default_application_path.py
specs/170-reusable-layer-artifacts/tasks.md
```

Some omitted entries are generated build copies or documentation, but that
classification is not sufficient for a release: every omitted executable,
build, harness, test, route, and configuration input must be deliberately
classified and either included in the T029 manifest or excluded with a reason.

## Decision

The probe is **SOURCE_SEAL_VALIDATED / CANDIDATE_NOT_FROZEN**. Do not build a
new SIF from it yet. The next candidate manifest must bind the source seal plus
the complete selected build/harness/test/configuration hash inventory, review
the eleven authorization-baseline mismatches, and then rerun Gate A/B/C before
T029. This prevents a valid runtime archive from hiding a changed harness or
packaging definition.

## Broader pre-freeze inventory

The first executable broader inventory is recorded in
[`spec170-candidate-input-inventory-20260819.md`](spec170-candidate-input-inventory-20260819.md).
It covers 1,102 runtime, build, harness, test, job, contract, and configuration
files and has digest
`sha256:3ead9e221f2002a9ba63e715c0af3246c3e0b443aec4a3af588523257ca393c5`.
This materially reduces the unclassified coverage gap, but it remains a
pre-freeze review artifact: model/canonical/security/route/schedule inputs,
Gate A/B/C outputs, staged SIF identity, and the post-freeze mismatch command
are still not bound by a T029 manifest.
