# Pre-Implementation Audit

**Date**: 2026-07-24  
**Verdict**: PASS

## Intent and baseline

Spec 146 is a successor repair, not a reinterpretation or rerun of Spec 144.
The immutable predecessor hashes match the contract:

- campaign summary:
  `01e95d9e79ccf879829a02d981d451e1cd35582aca86b57944330b2cdc2df738`
- campaign manifest:
  `5f980d857fb66f70e1939e408f35c780b14d4c202aad226d844b469da0f6a517`
- cell CSV:
  `c9fd955a7a5880888c91608ebfe3566d44501f3e280c4e9918c2bc0de1052a96`

The predecessor remains COMPLETE / MEASURED NEGATIVE for acoustic impaired
profiles.

## Code-aware findings

CodeGraph traced `schedule -> fetchPayload -> tryRecover` and found:

1. `fetchPayload` keeps separate `m_payloadInFlight` and
   `m_payloadProcessing` ownership.
2. `tryRecover` determines missing sources solely from absence in
   `m_signedOpaque`; it does not reject in-flight or processing cursors.
3. The class comment says only deadline-skipped sources remain recoverable,
   but the implementation permits recovery of sources that have not reached
   terminal missing state.
4. Source admission loops through every entry in `m_repairs` to rediscover the
   matching group.
5. `m_repairs`, group exhaustion, and source-group relations have no complete
   group retirement path.

Frozen measurements corroborate the mechanism:

- reorder r01: recovered 1,502, late arrivals 1,501, configured loss 0%;
- reorder r02: 1,909 recovery attempts, recovered 399, p50 grew from about
  36 ms in the first 125 observations to about 4,149 ms in the last 125;
- reorder r05: recovered 387, late arrivals 387, plus terminal delivery loss;
- loss/combined profiles show the same redundant work amplified by timeout and
  retry pressure.

## Necessity and ownership

- **Necessity: PASS.** The defect is reproduced by source ownership and frozen
  counters; no speculative feature is required.
- **Core ownership: PASS.** Eligibility, exact-once cursor ownership, group
  indexing, and retirement are generic Streaming/FEC responsibilities.
- **Application boundary: PASS.** UAV/acoustic semantics remain in the
  existing fixture and analyzer only.
- **Occam check: PASS.** Repair invalid transitions and unbounded scans before
  considering any new policy or workload knob.

## Security, migration, rollback

- Existing signature/provider/name/session and FEC digest validation remain
  mandatory.
- No wire-format or public-API migration is planned.
- Rollback is the isolated Core/status/runner change set; frozen Spec 144
  remains the comparison evidence.
- Spec 144's payload-confidentiality finding remains open. Spec 146 makes no
  new confidentiality claim and does not conceal that gap.

## Evidence readiness

- Deterministic tests precede implementation.
- New runner/output names prevent historical overwrite.
- Full build and binding rebuild precede preflight.
- The formal campaign is one-shot, single-owner, 16 cells, and 60 seconds per
  measured cell.

## Allowed changed-file set

Only the paths listed in `plan.md` are authorized. Any public API change,
workload selector in Core, change to frozen workload/network thresholds, or
write under the Spec 144 result root changes this verdict to BLOCK.

## Implementation-discovery amendment

Deterministic and MiniNDN diagnostics found three additional generic causes
inside the same audited boundary: completed out-of-order groups consumed the
unresolved scheduling horizon, Mapping discovery remained anchored to a
contiguous cursor after authenticated higher-cursor progress, and payload
attempt lifetime discarded the existing stream-period jitter margin.

The repair also distinguishes provisional timeout eligibility from terminal
retry exhaustion. Once a source Interest has timed out and neither network nor
validation owns the cursor, already authenticated repair may recover it;
otherwise the unchanged finite exact-name retry continues. This is narrower
than changing a prefetch algorithm or FEC policy and preserves the audit PASS:
no public API, wire format, workload selector, threshold, retry budget, or
prefetch-window rule changes.

Post-test profiling found a second generic Core bottleneck inside the same
Mapping ownership boundary. `admitVerifiedBlock()` copied and rebuilt the
complete retained Mapping state for every strict sequential block. Acoustic
publishes one six-entry Mapping block every 40 ms, and measured latency grew
monotonically while the Face thread approached saturation and the two
signature-validation workers remained mostly idle. The allowed repair adds a
strict-successor incremental admission path; every gap, fork, out-of-order
arrival, and quarantine still uses the original atomic rebuild.

The inherited experiment command also enabled per-packet Stream timeline
construction at sample rate one despite the Core warning that this probe can
become the bottleneck being measured. Spec 146 formal commands disable only
that packet probe by explicit environment value; an opt-in sampled diagnostic
remains available. This is an evidence-integrity correction and changes no
wire, scheduling, FEC, retry, or prefetch semantics.
