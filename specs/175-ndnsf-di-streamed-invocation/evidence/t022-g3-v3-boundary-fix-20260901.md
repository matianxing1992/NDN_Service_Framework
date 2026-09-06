# T022 G3 Host/CPU Matrix — 2026-09-01

## Verdict

`PASS`: the strict G3 validator accepted 42 valid run entries (M01–M14,
three repetitions per case) from the post-fix source subject. The manifest is
`results/spec175/g3/qualification-manifest-20260901-v3-boundary-fix.json`.

## Bound subject

- Source seal: `results/spec175/g0/source-seal-20260901-v3-boundary-fix.json`
- Source revision: `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7`
- Source-seal SHA-256: `sha256:5d2be5b43a2267d0e1dda506a600abde22877983b61ad5d13433521dc0b0017a`
- G3 manifest file SHA-256: `sha256:5765da19f352ddc1c3901b71a6b2cdd343a426294b7ebbd73589991879128fab`
- G3 manifest self-digest: `sha256:0cfa23ac9f1eb0906e752c28237597d681c65a842d1a11a76750115176736fe1`
- Matrix root: `results/spec175/g3/current-20260901/matrix/`
- Topology: frozen four-Provider MiniNDN star, real NFD/SVS/ABE, tiny ONNX,
  admission control disabled
- Fault seed: `1750002`; healthy/workload seed: `1750001`
- Every valid run has a case result, user log, runner log, terminal evidence,
  route snapshot, SVS fanout evidence, and clean process-tree teardown.

## Matrix result

The strict manifest records:

- 14 cases: M01–M14
- 3 repetitions per case
- 42 total entries
- 42 passed
- one registered fault dimension per case
- complete M11–M14 conversation evidence

The first M10-r2 directory timed out while waiting for the repository
publisher to exit after the route probe had already passed. It produced no
case result and is preserved as
`matrix/M10/r2-failed-20260901/`; it is excluded from the valid matrix. A
same-input M10-r2 rerun produced a normal PASS result in `matrix/M10/r2/`.
This is recorded as a preparation/teardown failure, not pooled as a runtime
failure or silently overwritten.

M14-r2 existed as a complete PASS result when the original serial launcher
lost its bookkeeping process. Its original result and launcher output were
retained and registered as an existing repetition; M14-r3 was then executed
under the same source seal and passed.

## Promotion boundary

T022/G3 is now closed for this source seal. No historical SIF or Tiger result
is promoted by this evidence. T023 may build exactly one SIF from this sealed
subject and must rebind all G4 evidence to the resulting SIF digest.
