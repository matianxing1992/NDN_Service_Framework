# Contract: Validation And Evidence

## Required Gates

1. Pinned baseline and backup manifest verification.
2. Conflict-free reconstructed branch and per-commit scope audit.
3. Clean Boost 1.71 configure/build.
4. Complete NDN-SVS unit suite plus new commit-failure, thread-owner,
   DataStore, and exact-boundary tests.
5. Focused NDNSF normal/Targeted segmented and timeout regressions.
6. One new integrated Spec 112 MiniNDN candidate and its six declared cells.
7. Post-implementation structure, semantic, code-aware, evidence, and
   convergence audits.

## Candidate Identity

The manifest binds:

- local master, Experimental, backup, and origin/master OIDs;
- tracked dirty diff and selected artifact hashes;
- NDN-SVS/NDNSF/NAC-ABE dependency identities;
- rebuilt NDN-SVS and Python/native binary hashes;
- campaign/test script hashes and configuration;
- exact cell identifiers and immutable result roots.

## Run-Once Rule

- A valid candidate/cell pair runs once.
- Source, binary, configuration, or script change creates a new candidate.
- Startup/infrastructure failures are retained and classified; they are not
  product passes or failures until the declared role path starts.
- Negative product results are retained and never overwritten.

## Acceptance Counts

- NDN-SVS unit suite: 100% pass.
- Boundary matrix: 24/24 byte-exact.
- Same-epoch burst: 80/80 8-KB, then 10/10 64-B and 12/12 4-KB, restart 0.
- Degraded Targeted timeout: exactly one timeout, zero responses, within the
  existing `timeout_ms + 500 ms` acceptance bound.
- Final source/branch identity matches the manifest after all cells.
