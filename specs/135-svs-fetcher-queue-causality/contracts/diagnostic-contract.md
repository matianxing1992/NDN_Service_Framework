# Diagnostic Contract

## Immutable controls

- Stage-A rates: 200, 400, 600, 800, 1000 publications/s/peer
- Stage-B rate: first stage-A instability boundary, or labeled 1000-pps stress
  point if no boundary occurs
- Peers: two MiniNDN nodes/processes
- Threading: one Face/io_context thread per peer
- Topology: 10 ms, 100 Mbps, zero configured loss
- Payload: deterministic 256 bytes, non-segmented
- Timing: 10 s warmup, 60 s measurement, 10 s drain
- Security: HMAC Sync Interest, RSA-2048 Data, validators disabled
- RSA proof: identity created before warmup; same `KeyChainSigner` signs a probe
  whose TLV signature type must equal `SignatureSha256WithRsa`
- Compression: disabled
- Sampling: exact stage summaries plus deterministic one-in-100 spans

## Interventions

Stage A varies only offered rate with `W10-P4096`. Stage B varies only:

```text
Fetcher window: 10 or 40
maxApplicationParametersSize: 4096 or 7168 bytes
```

## Claim rules

- Do not call a factor causal from one favorable cell. Both matched contrasts
  must agree.
- Do not add queue wait, network wait, aggregate callback residence, and leaf
  CPU into one total.
- A larger window that only shifts time from Fetcher queue to network wait is
  not a complete improvement.
- A larger piggyback block that reduces fallback but increases invalid packets,
  route failure, or process failure is not a supported improvement.
- The shared-I/O claim remains mechanism-supported, not experimentally isolated,
  because the historical subject cannot safely run a cross-thread treatment.
- All failures and censored waits remain visible.
- A stage-A sweep cell is never repeated as the stage-B baseline.
