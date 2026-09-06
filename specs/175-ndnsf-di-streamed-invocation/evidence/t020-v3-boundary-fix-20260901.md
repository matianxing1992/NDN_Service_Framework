# T020 current-source local qualification — 2026-09-01

## Decision

`T020 = PASS` for the repaired source/workload subject. This record closes
only G0, G1, and G2. It does not close the host/CPU MiniNDN G3 matrix, the
exact-SIF replay, CUDA/Tiger gates, or performance characterization.

## Subject identity

| Item | Value |
|---|---|
| Branch | `Experimental` |
| Source seal | `results/spec175/g0/source-seal-20260901-v3-boundary-fix.json` |
| Source-seal SHA-256 | `5d2be5b43a2267d0e1dda506a600abde22877983b61ad5d13433521dc0b0017a` |
| Recorded source revision | `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7` |
| Workload | frozen `tests/fixtures/spec175/tiny-causal-lm-v1` |
| Source change relevant to this checkpoint | rank-one Qwen candidates no longer carry a hybrid marker; non-rank-one candidates still require a sealed hybrid plan |

The worktree is intentionally dirty. The source seal records every in-scope
dirty source hash; G0 verifies that exact seal rather than treating a clean
Git tree as a prerequisite. Feature documents are separately hashed by G0.

## Gate results

| Gate | Manifest | Result | Evidence |
|---|---|---|---|
| G0 contract/traceability | `results/spec175/g0/qualification-manifest-20260901-v3-boundary-fix.json` | `PASS`, zero blockers | 82 FR, 21 SC, collision and task/traceability checks; final post-edit document digests |
| G1 bounded unit/Python/native gate | `results/spec175/g1/qualification-manifest-20260901-v3-boundary-fix.json` | `PASS`, 354 passed, 0 failed, 1 explicit unconfigured-real-model skip, exit 0 | same source-seal digest, native import/loader checks, bounded Spec175/shared-contract subject |
| G2 C++ integration gate | `results/spec175/g2/qualification-manifest-20260901-v3-boundary-fix.json` | `PASS`, I01--I20 registered, 3 healthy repetitions, no missing cases | exact integration binary `sha256:e8b9d0e8512844f46c0ebfbd99ce9ad738689d2e966c1d03e770d7c91193aebb` |

Manifest SHA-256 values are:

```text
G0 source seal:  5d2be5b43a2267d0e1dda506a600abde22877983b61ad5d13433521dc0b0017a
G0 manifest:     recorded in the stable manifest file above (compute SHA-256 from the final file)
G1 manifest:     407c36bc306f6295469f10ce0ff298a124e8a19306b81963a1be5a567c69da07
G2 manifest:     f6a4ca4cf1d515243915431ae1493b749cfa18c8b82fbed03356f2327677c78a
```

The focused regression set also passes 33 tests, including the rank-one
`QwenThreeStageSplitter` boundary assertion and the Spec170 hybrid/ordinary
path checks. The one G1 skip is the explicitly unconfigured real-model smoke;
it is not a failed Spec175 contract case and does not qualify the 27B CUDA
subject.

## Why the earlier real M01 did not qualify

The first root MiniNDN M01 reached the real Request/ACK path and then failed
before Response because the rank-one Qwen splitter unconditionally sealed a
`HybridPlan`. The ordinary Spec175 V3 validator correctly rejects a hybrid or
TensorGroup proposal, so this was a real design/code mismatch rather than a
network or SIF failure. The splitter and `SplitCandidate` contract were fixed,
the boundary regression was added, and a post-fix root M01 completed the full
Request/ACK/Selection/Response lifecycle with eight ordered events and clean
teardown. That single run is diagnostic; the strict three-repeat M01--M14
matrix remains owned by T022.

## Promotion boundary

The next valid action is to finish the already-running fresh 42-process G3
matrix under `results/spec175/g3/current-20260901/matrix/`, then run
`scripts/spec175_g3_manifest.py` with this source seal. No SIF build, upload,
CUDA allocation, or Tiger result may be promoted until strict G3 is `PASS`.
