# Spec 118 Completion Summary

## Outcome

The UAV video path now uses the app-neutral Spec 119 LiveStream publisher and
consumer handles. The UAV application owns camera naming, AES-256-GCM,
descriptor authorization, safe H264 join, replay admission, reorder/decoder and
UI behavior. Core owns Mapping, semantic-name exact Interests, Provider
validation, bounded prefetch/retry state and optional XOR over ciphertext.

The frozen protected MiniNDN candidate is:

```text
results/spec118-uav-session-key-acceptance-20260718-candidate3
```

It passed both required fresh 60-second cells. This does not claim a video/FEC
performance improvement.

## Implemented And Wired

- Camera start allocates a versioned Stream/session, 32-byte key and 4-byte
  nonce salt, then withholds the key-bearing response until routes, at least
  three measured publication groups, Mapping coverage and an SPS/PPS/IDR-safe
  join are ready.
- Each complete `VideoPacket` is bound to Provider/service/Stream/session/
  Mapping/key/cursor and semantic Data name in canonical AAD, encrypted before
  `publish`/`publishGroup`, and decrypted only in the ground-station callback.
- `ServiceProvider::createLiveStream` and `ServiceUser::openLiveStream` are the
  only live network owners. UAV has no separate Mapping fetch loop, future
  Interest pump, pending table or XOR publication/recovery state.
- Ordinary signed and `FecRecovered` opaque values pass the same application
  AEAD/session/replay gate before reorder or decode.
- Stop/replacement wipes application key/salt state, retires the Core handle and
  rejects old-session callbacks. Stop/status/failure/unrelated responses do not
  carry stream secrets.

## Executed Security And Contract Evidence

```bash
python3 tests/run_uav_stream_security_contract.py \
  --secret-scan \
  --scan-path results/spec118-uav-session-key-acceptance-20260718-candidate3
```

Result: PASS. All 12 source/integration/secret checks passed and the invoked
focused security/unit gate passed 84 cases. The suite covers strict descriptor,
nonce/AAD/envelope, wrong Provider/name/session, replay/corruption, refreshed
frontiers, readiness, lifecycle and opaque-FEC negative contracts. The final
focused suite also includes a 1,000-cursor semantic-name/nonce uniqueness gate.

The control-isolation campaign parser also passed 28/28 tests. Its control-only
path now marks the absent LiveStream policy as `not-applicable` instead of
inheriting the video-policy gate; command terminality, observed-state
convergence, lifecycle and security checks remain mandatory.

## Measured MiniNDN Evidence

Frozen command:

```bash
timeout 480s python3 Experiments/NDNSF_UAV_Stream_Parity_Campaign.py \
  --out results/spec118-uav-session-key-acceptance-20260718-candidate3 \
  --runs 1 --loss-percentages 0,5 --fec-parity-shards 1 \
  --auto-stop-seconds 60
```

| Configured loss | Accepted | Decoded frames | Max pending chunks/bytes | Timeouts / Nacks | Control | Persisted secrets |
|---:|:---:|---:|---:|---:|:---:|---:|
| 0% | yes | 375 | 0 / 10,800 | 0 / 0 | arm, takeoff, land | 0 |
| 5% | yes | 716 | 23 / 86,400 | 23 / 0 | arm, takeoff, land | 0 |

Both cells had zero decoded-frame gap and remained below the declared 48-chunk
and 16 MiB bounds. XOR recovery did not trigger (`fecRecoveredChunks=0`) in
either run because normal exact-name retrieval/retry delivered enough source
Data; that is retained as a measured non-event, not described as an FEC gain.

## Final-Code Post-Hardening Evidence

After splitting and hardening Provider Mapping/payload pending-Interest tables,
the final code was exercised under a new result identity:

```text
results/spec119-post-hardening-acceptance-20260718
```

Both fresh 60-second `mapped-pressure` cells passed. At 0% loss the path decoded
420 frames with 0 timeout/Nack and at most 9 Core in-flight / 26 NFD PIT entries.
At 5% loss it decoded 266 frames with 10 timeouts, no Nacks, at most 10 Core
in-flight / 23 NFD PIT entries, and no decoded-frame gap. Arm, takeoff and land
completed in both cells; persisted secret matches remained zero. The 5% cell
also recorded 1,030 Mapping Interests and 2,527,190 Mapping bytes (93.6% of
observed stream Interests), an efficiency limitation retained for follow-up.

The complete C++ unit suite passed 309 cases. Focused Targeted and token/replay
suites passed 18 and 10 cases respectively. Legacy shell regressions were not
used as final evidence because they require a host `/run/nfd/nfd.sock`; the
network acceptance remained MiniNDN-only.

## Preserved Security Failure

`results/spec118-uav-session-key-acceptance-20260718-candidate2` is retained as
`FAILURE 0/2`: generic response logging persisted `stream_key_hex` and
`nonce_salt_hex` in both cells. Candidate 3 received a new identity only after
response logs were reduced to byte counts and safe non-secret readiness fields,
and the campaign began scanning every persisted log. The failed cells were not
silently relabeled or rerun.

## Evidence Limits

- Candidate 3 predates the later capture-to-decode and NFD PIT instrumentation;
  those fields are measured in the Spec 119 matched campaign on the same UAV
  path rather than reconstructed here.
- MiniNDN validates protocol integration, not real radio, real camera, Docker,
  iTiger or long-duration operations. Those remain separate deployment work.
- Optional XOR correctness is established by deterministic one-source-loss and
  malformed/corrupt/two-loss unit vectors. The 60-second network cells do not
  manufacture loss solely to force a recovery counter.
