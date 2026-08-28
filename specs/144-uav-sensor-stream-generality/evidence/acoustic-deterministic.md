# T005 Acoustic/Audio Deterministic Evidence

**Date**: 2026-07-24  
**Verdict**: PASS

UAV-APP owns deterministic/file-replaceable opaque source bytes and complete
block admission. Core sees three opaque exact-extent classes:
`opaque-block-2`, `opaque-block-3`, and `opaque-block-4`. Their text is never
interpreted by Core; only the generic exact lower/upper extent is used.

Deterministic native cases prove:

- 2/3/4 source-count cycle and exact extent class;
- 40 ms period, two generic GF(256) repairs, and recovery capacity two;
- complete ordered block admission exactly once;
- direct and `FecRecovered` provenance accounting;
- malformed bytes, conflicting header, duplicate, and late-complete rejection;
- maximum complete encoded source item of 512 bytes; a 513-byte construction
  is rejected before publication.

The dedicated two-role `UavSensorStreamNode` announces the exact APP-known
extent, prepares it, publishes opaque sources, and admits only complete
application blocks. There is no microphone, PCM, codec, playback, or
audio-specific Core branch.

Verification:

- `UavProtocolState`: 70/70 PASS;
- Python metric/neutrality suite: 8/8 PASS;
- generic Stream recovery/security tests: PASS;
- final acoustic zero-loss preflight: to be frozen in
  `preflight-and-freeze.md`.
