# Spec170 local candidate r6 — post-Selection integration release gate (2026-08-17)

## Candidate identity

```text
release:       spec170-runtime-31d9f547-post-selection-integration-20260817
source:        31d9f547a32f941ab948f005f583c429d23bacbb
SIF:           /tmp/spec170-local-candidate-31d9f547-20260817-r6/runtime.sif
SIF SHA-256:   ba7180590e880c35db95e2a7bd2c6d73e838e9e8bf8c276003eaaee1e81757c9
SIF bytes:     4398710784
Provider SHA:  d9d993c0145020a5b3b2a61789c809c36c9414c27db3fb7bcc30315d2597c74e
Framework SHA: bfbc1b36c36ac47389ff68559b1782a8936966d3e83d1f130be865b0c39e1878
Python 3.10 SHA: dfc6ec22216f10b5a605d9fc00b3af626ee08f370799578d45d040b3354b07da
Apptainer:     local 1.3.4 / Tiger 1.3.4-1.el9
ORT:           1.20.0 (the SIF runtime and Provider link agree)
SVS:           Experimental prefix used for both headers and libraries
Tiger action:  verify hash and execute only
```

## Local gates

| Gate | Result |
|---|---|
| C++ integration | **PASS** — 21 cases, 288 assertions |
| C++ unit | **PASS** — 490 cases, 57,176 assertions, run with the SIF-matching ORT1.20/SVS library path |
| Python Spec170 suite | **PASS** — 59 passed, 2 skipped, 1 warning |
| Python 3.10 SIF import | **PASS** — `import ndnsf._ndnsf`; `make_predictive_data_name` is exported |
| Provider wiring check | **PASS** — `/Inference/NativeTracer`, 4 roles, 4 artifacts, `--check-only --wiring-check-only` |
| SIF library closure | **PASS** — `validate-sif-library-closure.py`, `ndnsf-sif-library-lock-v2` |
| SIF RPATH | **PASS** — Provider, framework, and Python extension use only `/opt/ndnsf-di/current/lib:/opt/onnxruntime/lib` |

The unit binary initially failed when launched without the matching runtime
library path because the host's default `/opt/onnxruntime` is 1.26.0 while the
candidate is deliberately built and packaged with ORT 1.20.0. Re-running with
the exact SIF-matching ORT/SVS paths passed all 490 cases. This is recorded as
toolchain selection evidence, not as a test failure.

## Earlier local failures retained as diagnostic evidence

The discarded r3 candidate exposed two release-gate defects before submission:

1. replacing a pre-existing SONAME symlink in `%post` created a framework
   symlink cycle; r6 installs the versioned file first and recreates the
   unversioned link afterwards;
2. the Provider was first linked against host ORT 1.26 while the base SIF
   contained ORT 1.20; r6 rebuilds Provider and the Python extension against
   the same Experimental SVS and ORT 1.20 inputs.

Neither discarded candidate was uploaded or submitted to Tiger.

## Remaining gate

This record proves that the exact source/build passes all local tests and SIF
closure checks. It does **not** claim TigerCluster acceptance. The next gate is
to copy this exact SIF, lock, build record, and workload bundle to project
storage, verify the remote SHA-256 and Apptainer version, then run D0, D1, D2a,
D2b, and D2h one bounded job per case while retaining per-case lifecycle,
assignment, Provider execution, and final Response evidence.
