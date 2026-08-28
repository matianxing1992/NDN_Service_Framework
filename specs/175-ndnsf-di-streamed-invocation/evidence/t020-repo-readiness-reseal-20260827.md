# T020 repository-readiness correction and reseal

## Failure classification

The post-audit G3 attempt stopped at `M07-r2` before a case result existed.
This was a preparation failure, not an M07 protocol result. The repository
publisher reported:

```text
repo-store-insufficient-cover: candidateCount=0 successfulCount=0
eligibleCount=0 requestedReplicas=1 requiredBytes=2906 excluded=none
```

The event order identifies a startup race. The publisher installed its Store
permission at epoch `1787888171.916496`; the Repo Provider registered the Store
handler at `1787888171.939078` and installed its Store permission at
`1787888171.999728`. The publisher could therefore issue the first request
before the Provider was ready. With a 30-second periodic SVS interval and a
shorter preparation deadline, that publication was not guaranteed to be seen
before the attempt ended.

## Correction

Commit `f5f2cab9983d41a8e9f6d867fedf7b3e2a4a9d07` keeps the publisher and Repo
Provider initialization concurrent so both can obtain controller DKEY and
permission state. The already-initialized publisher then waits on a unique,
token-checked file barrier. The MiniNDN parent releases it atomically only after
the publisher reports that it is waiting and the Repo Provider log proves that
the exact Store permission is installed. A stale or partial barrier, missing
peer readiness, process exit, or timeout fails closed.

Focused verification:

- `tests/python/test_spec175*.py`: 195 passed;
- real four-Provider M07 diagnostic:
  `results/spec175/g3/diagnostic-m07-repo-barrier-r2-20260827`;
- result: expected `Cancelled` terminal, case PASS, and clean MiniNDN teardown.

The diagnostic is not part of the formal G3 sample.

## Current coherent T020 subject

- source revision: `f5f2cab9983d41a8e9f6d867fedf7b3e2a4a9d07`;
- source seal: `results/spec175/g0/source-seal-post-repo-readiness-r5-20260827.json`;
- source-seal SHA-256:
  `194b9742442c1eba9353f8c96faf5b50dc846c83e146a3eddbad7b0409e15a93`;
- G0: PASS, zero blockers, manifest SHA-256
  `8e57a9674eb9d2fa651a0c51e24f36b64c9ea02f2eb3aafce86c2ee5718ef2c9`;
- G1: native unit PASS; Python 222 passed with zero failures/skips, manifest
  SHA-256
  `4d15b224e1b1c0e7f9720eb8901876ea30418d4ba5533e15d1bc07a83aacbc0c`;
- G2: PASS, 38/38 registered processes, manifest SHA-256
  `2fc1d67d26bf95f4ffc9409a75f36bc04f283a37757085adfcb9fbcb81f288ad`.

T020 is closed for this source seal. T022 subsequently used the same seal and a
fresh M01--M14 three-repeat root, producing the 42/42 PASS manifest recorded in
`t022-post-repo-readiness-g3-20260828.md`. T023 may now build exactly one final
SIF from that frozen subject.
