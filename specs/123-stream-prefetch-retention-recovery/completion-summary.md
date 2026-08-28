# Spec 123 Completion Summary

## Outcome

Spec 123 is complete. The paper-aligned mapped future pipeline passed a unique
60-second zero-loss MiniNDN UAV run at the restored 3600-byte, 12-source plus
one-repair, 8 Mbps load. Frozen Spec 121/122 evidence and every superseded Spec
123 attempt remain unchanged.

## What was actually wrong

The final defect was in prefetch, not GStreamer or GUI presentation:

1. Materialized-but-evicted Data could be classified as never produced.
2. Scheduling did not consistently apply the current adaptive budget, range,
   window, and Interest lifetime.
3. An application capture-to-receive value was fed into the network RTT state;
   it was not the paper's Data Retrieval Delay (DRD).
4. A network slot remained occupied until signature validation, decryption and
   the APP callback finished, so application processing gated replacement
   Interest expression.
5. `CHASING`, `ADJUSTING`, and `FETCHING` changed the reported window, but the
   cursor lookahead stayed fixed at steady-state demand. Chasing therefore did
   not produce the paper's actual Interest burst.
6. The paper assumes one Data per sample. UAV video maps one frame to 12 source
   Data plus one repair Data, but Core reserved only one extra packet rather
   than one complete source group. The minimum steady window had no room for
   Mapping delay or burst jitter.
7. A 16-name Mapping block doubled avoidable control-plane fetch and validation
   work versus a 32-name block that still fits safely in one signed Data packet.

## Repair

Core now measures DRD directly from exact Interest expression to matching Data
reception. Data reception immediately frees the bounded network slot and
expresses its replacement before validation/decryption/APP processing, while a
separate bounded processing owner prevents duplicate fetch or unbounded work.
Chasing and Adjusting windows now control the real cursor horizon; Fetching
collapses to measured demand. Segmented/FEC streams retain one complete source
group as their recovery margin. UAV uses 32-name Mapping blocks while retaining
semantic application Data names, names-only signed Mapping, 3600-byte payloads,
12+1 FEC, Provider signatures, AES-GCM, and the existing trust checks.

This is the mechanism in Gusev et al., *Real-Time Streaming Data Delivery over
Named Data Networking* (2016): bootstrap, chase cached Data faster than the
producer, detect the live edge from arrival timing, estimate simultaneous
Interest demand from DRD and production period, then maintain that demand with
early exact-name Interests. NDNSF adds signed names-only Mapping so those future
Interests retain the application's semantic Data names.

## Preserved evidence sequence

| Candidate | Result | Workload | Decoded | Provider future hits | Capture-to-decode p95 | Meaning |
|---|---:|---|---:|---:|---:|---|
| `results/spec123-stream-recovery-20260719-042407` | FAIL | earlier candidate | 1054 | 9/9 | 9301.914 ms | retention alone did not create a useful future horizon |
| `results/spec123-stream-recovery-20260719-043413` | FAIL | earlier candidate | 1648 | 83/83 | 5834.288 ms | continuity improved but lag accumulated |
| `results/spec123-stream-recovery-20260719-044641` | PASS, superseded | reduced 7 KiB/10+1 | 1831 | 968/968 | 240.346 ms | packetization reduction passed, but did not prove the original workload |
| `results/spec123-paper-prefetch-traced-20260719-052334` | FAIL | 3600-byte/12+1, high-rate trace | 1017 | not authoritative | 9300.735 ms | per-packet DEBUG tracing perturbed the system |
| `results/spec123-paper-prefetch-phase-window-20260719-055220` | FAIL | 3600-byte/12+1 | 1634 | 188/188 | 6534.666 ms | real Chasing reached the edge, but one-packet reserve could not hold it |
| `results/spec123-paper-prefetch-sample-reserve-20260719-060053` | **PASS** | 3600-byte/12+1 | **1830** | **7153/7153** | **205.946 ms** | phase-driven issuance plus one-sample reserve stayed live |

The accepted run had exact identity coverage 174/174 (100%), no rejected
correlations, no security failures, and decoder startup of 18 ms.
Capture-to-decode p50/p95/p99 was 142.654/205.946/269.402 ms;
decode-to-widget p95 was 4.076 ms. Mapping plus payload work was
`(746 + 23840) / 1830 = 13.435` Interests per decoded frame, 3.35% above the
13 configured source-plus-repair items. Provider future Interests were
7153/7153 satisfied, consumer rejection was zero, FEC recovered 11 items, and
both Provider pending and consumer in-flight state drained to zero at stop.

## Verification

- Build: full `./waf build -j4` succeeded.
- C++: `Stream` 40/40 and `UavProtocolState` 58/58 passed.
- MiniNDN: the final unique 60-second original-load run passed identity,
  continuity, latency, future-hit, work, security, and terminal-drain gates.
- High-rate packet timeline is independently switchable with
  `NDNSF_STREAM_PACKET_TIMELINE_TRACE`; the accepted latency run disabled that
  perturbing probe while retaining sampled exact source/decode/widget identity.
- Strict Spec Kit structure and post-implementation semantic audit passed.

## Compatibility and remaining boundary

No wire version, semantic name, Mapping trust rule, media encryption, or FEC
contract was weakened. `legacy-pipe` remains the runtime default because this is
one zero-loss MiniNDN acceptance run, not a loss/jitter promotion matrix or a
physical display scan-out measurement. The next most valuable work is a paired
zero-loss plus realistic loss/jitter matrix before changing the default.
