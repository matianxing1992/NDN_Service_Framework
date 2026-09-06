# Current-source G3 host/CPU matrix after Provider lazy-load correction

Date: 2026-08-29

This checkpoint reruns the real MiniNDN M01--M14 matrix after the Provider
lazy-load correction. It is bound to the current source seal and must not be
pooled with the earlier `fe285147` candidate or its SIF evidence.

## Bound inputs

- Source revision: `53fd4635a8ef8bd1a3cc2ea56450ce32b9102b50`
- Source-seal JSON SHA-256: `sha256:810eaf45041bb5d0c087b6977fe14450fa658a564a1167441a4ebc2cd54d7021`
- G3 manifest JSON SHA-256: `sha256:f8d6ba02314d1353aae7550ca6db247f38d669892c053ffe76d2bbfbe9b667ab`
- Runtime: tiny ONNX CPU reference fixture
- Topology: frozen 100 Mbit/s, 10 ms host-gate topology
- Providers: four independent processes
- Admission control: disabled
- Targeted prefetch: disabled
- Workload seed: `1750001`; fault seed: `1750002`

## Result

The fresh serial runner completed with exit code 0. The strict manifest
validator reports `PASS`, with 42/42 fresh process results: each of M01--M14
has three repetitions, and every repetition has the expected terminal state,
four-Provider contract, disabled-admission contract, and required artifact
hashes. M11--M14 include their conversation evidence fields and report zero
state-tensor bytes on NDN and zero runner calls after rejected validation.

This is host/CPU MiniNDN evidence only. It does not prove Qwen3.6-27B CUDA
residency, exact-SIF equivalence, Tiger execution, or throughput. The M01
smoke's measured `distributed_ms` is a request-chain diagnostic, not a prefill
or decode measurement.

The next authorized step is one new local SIF build from a source archive whose
file list closes every dirty in-scope source path, followed by the exact-SIF
preflights and replay. The first attempted build was intentionally stopped by
the strict source/archive check because the modified host checklist was not
present in the prepared archive; that is a packaging-boundary issue, not a
runtime or model failure.
