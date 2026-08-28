# Spec 136 400 pps Claim Audit

**Date**: 2026-07-23  
**Mode**: Post-implementation code, document, and evidence audit  
**Verdict**: **CONDITIONAL PASS for a bounded descriptive claim; BLOCK for a
formal or generalized proof claim**

## Audited Claim

The audit evaluated whether the corrected 400 pps confirmation proves that a
single publication-preparation worker relieves the Face event loop and extends
sustainable capacity without accelerating RSA itself.

## Confirmed Facts

- Both cells used the same binary
  `e6753ac76b196c8680c0cf3e1d9ffa83208192d75f5c84783b6f8b16976ce3dd`
  and NDN-SVS library
  `7945f22bcdaaef149f4e3cc2a75a39d124e4dfd54d07027cadcc83b4f5b1308f`.
- Both used two MiniNDN nodes, bidirectional PubSub, 400 pps per peer,
  10/60/10 timing, 10 ms delay, zero configured loss, and publication Fetch
  window 64.
- Both peers in both cells attempted exactly 24,000 measured publications.
- Inline delivered 27,773 aggregate publications, or 231.44 pps/peer and
  57.86%. Worker delivered all 48,000, or 400 pps/peer and 100%.
- Inline recorded 2,173 measured-window publication Fetch timeouts and 352
  pending Face publication calls. Worker recorded zero for both.
- Worst-peer heartbeat p99 changed from 19.921 ms to 11.252 ms, a 43.5%
  decrease. Aggregate skipped heartbeat ticks changed from 89,903 to 49,747, a
  44.7% decrease.
- The same serialized RSA signer served both modes. Data and Interest
  cryptographic service times were similar, so the evidence does not show RSA
  acceleration.
- Source and focused tests show that treatment moves publication construction,
  encoding, and two Data RSA signatures to one FIFO worker, then commits in
  order on Face. Sync Interest signing, validation, and commit remain on the
  Face path.

## Source And API-Contract Cross-Check

- `ndn-svs/svspubsub.hpp:174-184` limits publication preparation to zero or
  one worker; `:298-316` states that only the byte-oriented `publishAsync()`
  overload may be called from an application thread and that mapping,
  active-Data emission, and state-vector advertisement remain Face-owned.
- `ndn-svs/svspubsub.cpp:405-455` posts accepted byte publications to the
  single preparation pool. `:458-517` prepares them on that worker and posts
  the prepared result back to `m_face.getIoContext()`.
- `ndn-svs/svspubsub.cpp:580-628` constructs and encodes the inner and outer
  Data packets and performs both Data signatures during preparation.
  `:758-870` restores sequence order and `:914-955` performs the Face-owned
  mapping/store/active-put commit.
- `ndn-svs/core.cpp:410-518` performs Sync Interest validation from the Face
  receive callback. The Spec 136 benchmark explicitly disables parallel Sync
  processing and production, so `sendSyncInterest()` follows its serial path.
- `svs-rsa-single-worker.cpp:400-431` assigns the same RSA signer and validator
  to both modes, changes only `publicationPreparationWorkers` from zero to one,
  and disables both independent Sync-worker options. `:510-547` makes the
  control call `publishAsync()` on Face while treatment calls the same API from
  the APP pacer thread.

This code and API review supports the mechanism claimed by the experiment. It
does not upgrade a single fixed-order pair into formal causal or population
evidence.

## Required Qualifications

1. This was one non-formal fixed-order pair, not the sealed ten-cell matrix.
   It supports a descriptive observation under the recorded condition, not
   population inference, production readiness, or SC-006.
2. Delivery p99 changed from 156.096 ms to 83.121 ms among delivered
   publications. Because completion was 57.86% versus 100%, this is a censored
   survivor-set statistic and is not independent latency evidence.
3. The inline terminal was originally recorded as `HARNESS_INVALID`. The
   corrected analyzer preserves that receipt and separately interprets its only
   two admission messages as `LOAD_UNSUSTAINED`.
4. Inline recorded 118 duplicate callbacks and worker recorded 162. These are
   disclosed diagnostics; the confirmation does not prove identical duplicate
   behavior.
5. Each cell generated a fresh RSA-2048 keypair. The algorithm, size, signer
   implementation, and signature type match, but key material was not reused
   across the pair.
6. The result does not retain runner invocation, effective CPU affinity, raw
   percentile samples, or an immutable source/build manifest. These remain
   formal-campaign provenance requirements.

## Approved Wording

> In this two-node bidirectional 400 pps/peer, 60-second confirmation, the same
> binary and NDN-SVS library delivered 231.44 pps/peer in Face-inline mode and
> 400 pps/peer with one publication-preparation worker. Heartbeat p99 decreased
> by 43.5%, while skipped heartbeat ticks decreased by 44.7%. These descriptive
> results support that moving publication construction, encoding, and Data RSA
> signing off the Face thread reduces Face obstruction and extends sustainable
> capacity under this recorded condition. They do not show faster RSA and do
> not replace the formal Spec 136 matrix. Delivery p99 among delivered
> publications was 46.8% lower, but unequal completion makes it a secondary,
> censored metric rather than independent latency evidence.

## Verification

- NDN-SVS `TestSVSPubSub`: 27/27 passed.
- Focused repeated-Mapping and worker thread/order tests passed.
- Binary and library hashes still match the campaign manifest.
- Independent analyzer recomputation reproduced 231.44, 400.00, 43.51%, and
  46.75%.
- ARS statistical fallacy scan: 11/11 checked; survivor-set bias and
  fixed-order/non-replicated causal scope require the qualifications above.
