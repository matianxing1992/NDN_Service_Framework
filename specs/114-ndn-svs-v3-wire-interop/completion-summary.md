# Spec 114 Completion Summary

Date: 2026-07-16  
Feature: NDN-SVS V3 Wire Compatibility and Interoperability

NDN-SVS Experimental now emits and accepts the standard V3 route/envelope:
`/<group>/v=3/<ParametersSha256Digest>` contains signed Data named
`/<group>/v=3`, whose Content is the canonical StateVector. Embedded Data is
validated before semantic vector decoding; state validation/merge is atomic;
serial and parallel modes share the contract. Optional MappingData/RepairData
are bounded trailing extensions and commit as one separately validated
collection after the core merge.

Explicit V2 remains byte/profile isolated. V3 neither silently downgrades nor
uses whole-parameters LZMA. NDNSF User, Provider, and GUI default to V3 with
200 ms suppression while preserving explicit V2/1 ms rollback and rejecting
invalid versions at startup.

## Final acceptance

- NDN-SVS units: 67/67.
- independent standalone C++/NDNts: 5/5.
- MiniNDN candidate `spec114-9116d37e2e705bf281f4`: 6/6; 120/120 remote
  sequences at both 0% and 5% loss; six equal vectors; zero duplicates,
  rejects, Sync Ack Data, or restarts.
- NDNSF candidate `spec112-7f67052175cf629158ab`: 8/8; four boundary modes
  24/24, burst 102/102, V2 2/2, and both degraded Targeted APIs terminate
  exactly once near the 1 s deadline.
- C++ focused: 18/18 Targeted, 2/2 NdnSvsSmoke, 2/2 MessageValidator.
- Python/GUI: 41 passed + 3 network-gated skips, plus GUI 16/16.

Final NDN-SVS head/tree: `53dd1588201b967a4aa9decd3e51ade3263e0f88` /
`be3d9ccd348377160ab7afffabcbdea9459cff4f`.

Setup-invalid and superseded candidates are preserved and explicitly described
in the evidence; no failed result was overwritten or promoted. Spec 114 does
not merge, push, tag, remove V2, build Docker, use iTiger, or claim physical
network evidence. Experimental remains a clean human-review candidate nine
commits ahead of unchanged local/remote master.
