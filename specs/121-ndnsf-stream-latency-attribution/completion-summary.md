# Spec 121 Frozen Completion Summary

## Result

The investigation found and fixed two independent causes of the apparent live
video delay:

1. The Core consumer stopped after its initial Mapping horizon because accepted
   resolver frontiers were not handed to the adaptive fetcher, and equality at
   the legal `nextReserved` boundary was incorrectly rejected. Latest-mode UAV
   decoding also started at media sequence zero instead of the verified join
   boundary. After correction, the 60-second baseline decoded 1800 frames and
   continued through the final ten seconds with no timeout or stale-frontier
   failure.
2. The Provider used `fread(..., 8192)` on low-bitrate H.264 output. It could
   wait almost one second to fill the stdio request, while the old latency clock
   started only after that read returned. Fixed-size packet/FEC batching added
   another wait. POSIX pipe reads plus a bounded 20 ms partial-packet flush
   remove most of this hidden batching.

The old approximately 1 second `encoded-output-to-decoder-output` percentile is
withdrawn. One H.264 input group produces many decoded frames, so FIFO pairing
is not exact. The analyzer now rejects these correlations and reports exact
Core stages, decoder-process startup/cadence, and decoded-callback-to-GUI timing
separately.

## Frozen 60-second evidence

| Metric | Corrected batching baseline | Bounded batching candidate |
|---|---:|---:|
| Result directory | `results/spec121-attribution-corrected-candidate2-20260718` | `results/spec121-bounded-provider-batching-candidate3-20260718` |
| Decoded frames | 1800 | 1842 |
| Decoder first-input to first-output | 917 ms | 87 ms |
| Decoder output interval p99 | 1066 ms | 142 ms |
| Decoder callback to GUI p95 | 30 ms | 19 ms |
| Shared-host Data put to receive p95 | 23.107 ms | 14.643 ms |
| Future Interest hit ratio | 357/357 (100%) | 2951/2951 (100%) |
| User CPU | 51.59 s | 55.47 s |
| System CPU | 14.20 s | 14.00 s |
| Max RSS | 254820 KiB | 254612 KiB |
| Max PIT entries | 36 | 35 |

The candidate reduced decoder startup by 90.5% and the periodic p99 stall by
86.7%. It increased future payload work by about 8.3 times because smaller,
more frequent chunks interact with the fixed four-source-plus-one-repair FEC
group. CPU rose by about 7.5%, while RSS and PIT bounds remained flat. The
candidate is therefore a useful diagnostic result, but it is neither an
accepted optimization nor an accepted runtime default.

## Meaning of 1/2 RTT

Prefetch makes the exact-name Interest pending before production, so no second
Interest round trip is added after Data exists. In the one-host MiniNDN setup,
the configured link delay is 1 ms, but measured `data-put -> data-received`
steady p95 was 14.643 ms in the candidate. NFD forwarding, process scheduling,
socket delivery, and Face callback dispatch are part of that measured one-way
delivery. Decoder and GUI work remain outside 1/2 RTT.

## Superseded acceptance gate

Spec 121 is frozen after T001-T006 evidence and the T007 governance closeout.
Its originally planned five matched proxy-metric pairs are not run: repeating
them cannot establish camera-acquisition-to-display latency while H.264
input-group-to-output-frame identity remains ambiguous. Spec 122 T009 owns new
counterbalanced confirmatory and trace-control cells after exact source-frame
identity is available. The current POSIX/20 ms code path is therefore only a
provisional code default. It cannot be promoted as the accepted performance
default unless Spec 122 passes the hard Interest, correctness, latency, and
resource gates.
