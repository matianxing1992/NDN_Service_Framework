# Spec175 T020/T022 qualification rerun — 2026-09-01

## Subject

This record is the current host/CPU qualification subject after the canonical
artifact transport repair.  It uses the source seal
`results/spec175/g0/source-seal-canonical-transport-20260901.json` and the
checked-in tiny ONNX fixture.  No prior SIF, manifest, or source seal was
reused.

## G0--G2 results

| Gate | Durable record | Result |
|---|---|---|
| G0 | `results/spec175/g0/qualification-manifest-canonical-transport-20260901.json` | PASS; zero blockers |
| G1 | `results/spec175/g1/qualification-manifest-canonical-transport-20260901.json` | PASS; 358 passed, 1 skipped, zero failed |
| G2 | `results/spec175/g2/qualification-manifest-canonical-transport-20260901.json` | PASS; 20 registered I-cases, three healthy repetitions, no missing cases |

The source seal digest recorded by all three gates is
`sha256:0bde55f3e01938f213aa47b2ccdce602d5c255ba8d0b32162744178049fc1980`.
G1 verified the native unit binary, Python extension import, and loader
closure.  G2 used the current `build/integration-tests` binary and fixed seed
`1750001`.

## G3 result

The strict host/CPU matrix ran each M01--M14 case in three fresh root
MiniNDN processes (42 total), with four Providers, real NFD/SVS/ABE, the tiny
ONNX runtime, disabled admission control, and the frozen 100-Mbit/s/10-ms
topology.  Workload cases used seed `1750001`; fault cases M05--M09 used
`1750002` as required by the contract.  All 42 processes returned the
registered PASS result, produced terminal evidence and runner logs, and left
no owned process behind.

The durable run tree is
`results/spec175/g3/canonical-transport-20260901/`.  The strict manifest is
`results/spec175/g3/qualification-manifest-canonical-transport-20260901.json`
with digest
`sha256:95ff59aa6ae7b3274f86884690089124f9e19be134cc1eec7e414c0ba6059cec`.
It records 42/42 PASS entries and binds the same source-seal digest above.

## Decision

T020 and T022 now close for this source/workload subject.  The next release
gate is T023: build exactly one SIF from this source seal and replay the same
M01--M14 subject, including the post-Selection canonical source/root fetch.
The old v5 SIF and all pre-repair manifests remain historical diagnostics.
