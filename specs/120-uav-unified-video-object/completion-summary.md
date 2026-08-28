# Spec 120 Completion Summary

## Outcome

Spec 120 now uses one canonical, semantically named, encrypted and Provider-signed
NDN Data packet set for both live viewing and durable recording. The UAV APP
drains an immutable Core published-packet feed into RepoCore; recording manifests
reference those unchanged wires. Historical playback serves the same wires and
reuses the normal LiveStream validation, decryption, ordering and decoder path.

## Acceptance evidence

All network acceptance cells used fresh identities and 60-second measured
windows. A separate 30-second GTK smoke validates visible presentation rather
than performance:

| Cell | Result | Key evidence |
|---|---|---|
| live-only, 0% | PASS | 116 decoded frames; eligible future Interests 25/25 |
| recording-only, 0% | PASS | 14,935 canonical Data; complete manifest; 21 replay frames |
| live + retention, 0% | PASS | 15,902 canonical Data; complete manifest; no legacy writer |
| live + retention, 5% + storage failure | PASS | 128 live frames; one bounded storage circuit failure; incomplete manifest; 80 durable packets |
| late retention start/restart | PASS | two new intervals; decoder-safe joins; 96 replay frames |
| certificate rotation replay | PASS | current certificate rotated; archived packets unchanged; 16 replay frames |
| visible GTK live view | PASS | nonblank screenshot; GUI-owned decoded frames 116; render gate PASS |

Canonical result directories are recorded in `quickstart.md`. Failed candidates
remain preserved and were never overwritten or silently rerun.

## Performance attribution

Five fresh matched 0%-loss tracing-off/on pairs completed 116/116 frames and
retained 25/25 eligible future-Interest hits. At the stable 2% cursor sampler,
trace-on runs captured 5, 6, 10, 7 and 7 cursors. The group-correlated
encoded-output-to-decoder-output metric had 24 samples per run: all five p95
pairs stayed within 5%, while four of five CPU pairs stayed within 5%. Pair 3's
+7.91% CPU result is retained as explicit variability. This meets SC-015's 4/5
gate but is not an optimization claim.

Same-clock traces consistently show sub-millisecond validation/decryption,
roughly 3 ms p50 protection-to-signed-materialization, and much larger bounded
decrypted-to-reorder waits (up to roughly 1.1 seconds). Therefore the evidence
does not support blaming crypto or network transit for the residual wait.
Cross-process one-way time remains unavailable without clock-offset uncertainty.

## Design decisions and boundaries

- Names-only signed Mapping is retained. Accepted 0%-loss live cells reached
  100% eligible Interest-before-production, so inline Data-in-Data is unnecessary
  and remains disabled.
- Storage failure opens one bounded retention circuit and records an explicit
  gap; it does not retry or log every subsequent packet and does not stop live
  publication.
- A viewer joining an already-publishing recording waits for the next
  Mapping-covered SPS/PPS/IDR boundary before receiving its descriptor.
- Plaintext media keys do not enter Repo, manifests, catalogs, status, or logs.
- Legacy raw recording databases are rejected; migration requires export with
  the old implementation or deletion.
- MiniNDN is the authority for this feature. Real-radio, hardware-camera and
  long-duration deployment claims remain deferred.

## Verification

- `Stream,UavProtocolState`: 89 C++ tests passed.
- `test_ndnsf_core_streaming.py`: 18 tests passed.
- `test_ndnsf_uav_unified_video.py`: 11 tests passed after the GUI regression gate was added.
- UAV stream security contract and persisted-secret scan passed with no matches.
- `UavDroneApp` and `UavGroundStationApp` built successfully.

## Post-implementation audit

Verdict: **PASS**. The strict structural scan reports 31/31 functional
requirements traced, 17/17 success criteria covered, seven cohesive tasks, no
placeholder, duplicate, malformed task, or untraced requirement. Code-aware
review confirms that Core owns only the generic bounded immutable packet feed
and timeline mechanism, while UAV APP owns media protection, retention policy,
manifest/key authorization, and playback admission. Security negatives,
legacy-path removal, fresh 60-second MiniNDN cells, preserved failed candidate
identities, and five matched tracing pairs provide the required post-
implementation evidence. A requirement-by-requirement completion audit rejected
the original 20% matrix because its sparse, uncorrelated p95 passed only 3/5
pairs. The 10%, 5%, and initial 2% attempts remain preserved as negative or
measurement-invalid candidates. After correlating decoder output to its source
group, the final fresh 2% matrix passes SC-015 in 4/5 matched pairs.

The first post-completion visual audit exposed a separate acceptance defect:
the ServiceContainer decoded frames, but the GUI still listened for the retired
`Video packet stream` status while the canonical path emitted `Protected
LiveStream`. Consequently its display gate stayed closed and its counter
remained zero. The GUI now recognizes the canonical ready status, the launcher
requires its dedicated render gate, and
`results/spec120-gui-visible-fixed-candidate1-20260718/gui-visible.png` records
the corrected nonblank display. The failed pre-fix candidate remains preserved
at `results/spec120-gui-visible-repro-candidate1-20260718`.
