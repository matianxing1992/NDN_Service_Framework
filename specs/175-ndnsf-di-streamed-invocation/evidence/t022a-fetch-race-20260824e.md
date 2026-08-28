# T022-A M01 fetch-race reproduction — 2026-08-24

Status: **diagnostic failure; not pooled as a pass and not a completed T022-A
instrumentation record**.

## Invocation

```text
case: M01
seed: 1750001
output: results/spec175/g3/t022a-repro-20260824e
NDNSF_SELECTION_TARGETED_PREFETCH=0
SPEC175_TRACE=1
```

The run reached the real four-Provider streamed path and did not fail during
topology or repository preparation. It failed while the User was fetching the
first event: the User exhausted the registered exact-event retry budget for
cursor 1 and returned `stream event gap exceeded retry budget`.

The Provider-3 log records cursor 1 committed through
`stream-event-committed` with a nonzero SVS publication sequence and subsequent
cursors were also committed. The User log records exact Interest expressions
and timeouts for the cursor-1 event. The process tree was torn down by the
runner.

This run used the existing `SPEC175_TRACE` publication/retry messages only. It
did **not** yet record Provider exact-Interest arrival, IMS hit/miss,
pending-Interest insertion/satisfaction, or `face.put` completion. Therefore it
does not distinguish a routing/FIB loss from an IMS/pending/return-path loss and
does not close T022-A. No retry lifetime, retry count, deadline, topology, or
targeted-prefetch setting was changed.
