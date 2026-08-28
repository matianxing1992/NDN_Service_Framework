# Prepared PR #36 Description

This text is prepared locally and has not been applied to GitHub.

## Summary

- `eb0754d9d250f65789928cfdf7f3917fae996f07` is the single complete V3
  implementation owner. It implements the normative V3 name/envelope,
  parameters digest, validation-before-mutation, explicit V2 isolation, zero
  V3 Sync-Ack, and correct group-prefix registration/lifecycle behavior.
- `7804202578ffed5d160ac8178a59593942372b9f` keeps generic typed PubSub bounds
  separate from the protocol commit.
- `f8ef18f365df06a917c48b7759d480fbd4b5aa99` and
  `e42996df0bd5b2d3fbe032cbb31ae83c04a46b1e` own PubSub regression coverage
  and pending-fetch iteration safety.
- Mapping/Repair policy remains downstream on Experimental. The executable C++
  and TypeScript NDNts harness is owned by NDNSF and is not part of either
  active NDN-SVS branch.

## Local validation

- final ASan unit suite: 71/71;
- deterministic fixtures: zero diff;
- NDNSF-owned standalone C++/TypeScript NDNts matrix: 5/5;
- exact-candidate MiniNDN C++/TypeScript NDNts matrix: 6/6 cells at 0% and 5% loss,
  240/240 remote sequences, equal final vectors, zero duplicates, rejects,
  restarts, and Sync-Ack packets.

## Remote status

The prepared head is `e42996df0bd5b2d3fbe032cbb31ae83c04a46b1e`.
GitHub Actions has not run on this exact rewritten head because no remote ref or
PR metadata was changed. CI success must be reported only after publication
and a green exact-head run.
