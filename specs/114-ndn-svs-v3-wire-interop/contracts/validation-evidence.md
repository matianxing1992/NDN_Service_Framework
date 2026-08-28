# Contract: Candidate and Validation Evidence

## Candidate manifest

`Experiments/spec114_candidate_manifest.py` creates
`results/spec114-svs-v3/<candidate>/candidate-manifest.json` containing:

- target NDN-SVS commit, branch, clean status, tracked tree hash, and four Spec
  113 ancestor OIDs;
- ndn-cxx, NFD, Boost, compiler, MiniNDN, Node, npm, and NDNts identities;
- installed header/library hashes and rebuilt C++/Python consumer hashes;
- protocol version, timers, signer/validator test profile, extension profile,
  batching/compression settings, topology, loss, and workload;
- fixture and harness digests;
- expected six cell IDs and run-once status.

The candidate identifier is a SHA-256 digest of the canonical manifest inputs.
Secrets, credentials, and user-specific tokens are excluded.

## Evidence levels

- `proposed`: document only;
- `implemented`: code and tests exist;
- `executed`: the real path ran;
- `measured`: counts/timing/artifact identity captured;
- `interop-proven`: both independent directions and formal network cells meet
  the contract.

No artifact may claim a higher level than its evidence.

## Required evidence files

```text
specs/114-ndn-svs-v3-wire-interop/evidence/
├── baseline.md
├── fixed-vectors.md
├── focused-validation.md
├── full-build.md
├── standalone-interop.md
├── minindn-validation.md
├── ndnsf-regression.md
├── commit-review.md
└── final-audit.md
```

Formal result directories additionally contain per-cell configuration, stdout,
stderr, JSONL events, bounded packet samples, summary JSON, and immutable
candidate manifest.

## Candidate failure

- Any failed formal cell marks the candidate failed.
- A corrected source/configuration creates a new candidate ID.
- Failed results are preserved and not overwritten or relabeled.
- Invalid setup before a peer starts is recorded as invalid evidence; it is not
  a protocol failure, but still requires a new candidate/cell identity before a
  formal retry.

## Completion gate

Completion requires all tasks checked, 100% requirement/task/evidence mapping,
strict structure pass, no placeholders, complete candidate identity, all
declared tests executed, and post-implementation audit/convergence with no
Critical or High finding.
