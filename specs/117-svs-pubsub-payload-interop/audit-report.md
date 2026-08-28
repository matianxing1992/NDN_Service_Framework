# Spec 117 Post-Implementation Audit

**Verdict**: PASS for measured-negative closure; **DO NOT ADMIT MiniNDN** and do
not claim SVS-PS application-data compatibility.

## Intent and necessity

The implementation tests the missing claim directly: whether public C++
NDN-SVS and TypeScript NDNts PubSub APIs can exchange application bytes, not
only synchronize StateVectors. The four cases are the minimum useful corpus
for text, binary safety, larger data, and mandatory segmentation. The three
tasks remain cohesive; no mechanical per-file/per-command task expansion is
needed.

## Architecture and ownership

- All peer, dependency, runner, oracle, and Spec 117 files are in NDNSF.
- `/home/tianxing/NDN/ndn-svs` is a clean read-only subject at
  `70e682f8500e2ad205ec17722287ea0b8bd6a9f0`.
- Both peers use public PubSub APIs; no private decoder or test-only wire
  translation was introduced.
- The conditional network launcher imports MiniNDN only after an exact 8/8
  standalone success. It adds only the explicit inter-host sync-prefix route;
  publication and Mapping registration remains peer-owned.

## Security, migration, and rollback

Shared HMAC signing/verification is enabled for sync, Mapping, outer, and inner
test data. Exact name/length/SHA-256 acceptance fail-closes missing, duplicate,
unexpected, and corrupted receipts. This feature changes no production
protocol, persistence, or deployment state, so no data migration or production
rollback is required. Removing the NDNSF-owned example/spec files reverts the
feature without changing NDN-SVS.

## Code and evidence findings

| ID | Severity | Finding | Resolution |
|---|---|---|---|
| A-001 | High | Peer/corpus hashes alone did not bind the loaded library/runtime. | Resolved: result and future admission bind NDN-SVS HEAD/tree/library, Node binary, and package-lock. |
| A-002 | High | The standalone run produced 0/8 remote receipts after bilateral sync progress. | Preserved as `INTEROP_INCOMPATIBLE`; Mapping is the last completed boundary in both directions. |
| A-003 | Medium | The raw run included C++ local self-publication callbacks and a late TypeScript error after JSONL close. | Oracle rejected local callbacks; peers were hardened and rebuilt/type-checked. The failed formal result was not rerun or relabeled. |
| A-004 | Low | Host Unix-face traffic yielded only a capture header in `ndndump.log`. | JSONL, RIB, commands, stderr, hashes, and bounded Mapping timeouts support the diagnosis; no packet-content claim is made. Admitted MiniNDN cells retain real link captures. |

The measured mismatch agrees with source reality: C++ Mapping names and entries
include bootstrap session identity, while pinned NDNts uses the sequence-only
SVS-PS Mapping form. Because Mapping failed, no conclusion is drawn about
outer-fetch, inner decoding, or reassembly.

## Verification

```text
strict Spec Kit structure             PASS (10 FR, 5 SC, 3/3 tasks)
Python focused tests                  7/7 PASS
C++ peer build                        PASS
TypeScript type-check                 PASS
existing StateVector standalone       5/5 PASS
canonical sync MiniNDN evidence        6/6 SUCCESS
standalone payload gate               0/8, INTEROP_INCOMPATIBLE
MiniNDN admission                     NOT_ADMITTED, miniNdnLaunched=false
NDN-SVS working tree                  clean
```

## Claim and next gate

Evidence quality is **A for the bounded Mapping incompatibility diagnosis** and
**none for positive application-payload compatibility**. A separately audited
NDN-SVS SVS-PS wire repair is required. Its unit vectors must settle Mapping
and outer-name session semantics before one fresh Spec 117 standalone run may
attempt to produce 8/8 receipts and admit MiniNDN.
