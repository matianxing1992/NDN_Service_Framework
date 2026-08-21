# Data Model: NDNSF-DI Streamed Invocation

## 1. StreamedInvocationOptionsV1

Signed request options. Values are immutable after Request publication.

| Field | Type | Required | Default / validation |
|---|---|---:|---|
| `version` | uint64 | yes | exactly 1 |
| `mode` | enum | yes | `Normal`; `Targeted` requires one Provider and existing cached/bootstrap/refill behavior |
| `generationId` | 128-bit opaque hex | yes | nonzero, user generated |
| `streamEpoch` | uint64 | yes | nonzero, user generated |
| `eventKeyGrant` | protected bytes | yes | inside ABE-protected Request only; decrypts to exactly 32 key bytes |
| `maxEvents` | uint32 | yes | 512; 1..4096 |
| `interestWindow` | uint16 | yes | 16; 1..64 |
| `interestLifetimeMs` | uint32 | yes | 500; 100..5000 |
| `maxEventRetries` | uint8 | yes | 3; 0..8 |
| `publisherQueueCapacity` | uint16 | yes | 64; 1..1024 |
| `callbackQueueCapacity` | uint16 | yes | 64; 1..1024 |
| `reorderCapacity` | uint16 | yes | 64; window..1024 |
| `retentionMs` | uint32 | yes | 30000; 1000..300000 |
| `completionGraceMs` | uint32 | yes | 5000; 0..30000 |
| `maxEventWireBytes` | uint32 | yes | 16384; 512..65536 |
| `allowReplacement` | bool | yes | false |
| `maxReplacements` | uint8 | yes | 0; exactly 1 only when replacement enabled |

Validation is atomic. Unknown critical fields, duplicated fields, invalid ranges,
or a replacement combination other than `(false,0)` or `(true,1)` reject the
request as `STREAM_OPTIONS_INVALID`.

## 2. StreamBindingV1

Immutable lineage shared by request, event consumer, provider writer, End event,
and final Response.

| Field | Type | Rule |
|---|---|---|
| `requestId` | Name | exact current V2 request ID |
| `requester` | Name | identity that issued Request |
| `serviceName` | Name | unified service name |
| `producer` | Name | selected final-role Provider |
| `producerBootId` | digest/string | exact selected boot epoch |
| `attemptEpoch` | uint64 | starts at 0; increments on replacement |
| `planDigest` | SHA-256 | accepted plan or Targeted binding digest |
| `generationId` | 128-bit opaque | from signed options |
| `streamEpoch` | uint64 | unique within attempt; changes on replacement |
| `userToken` | bytes | exact request token echoed/verified |
| `policyEpoch` | uint64 | accepted authorization epoch |
| `deadlineEpochMs` | uint64 | immutable absolute deadline |

Equality is field-for-field. A partial match is a mismatch.

## 3. InvocationEventMessageV1

Canonical plaintext before stream-epoch encryption.

| Field | Type | Rule |
|---|---|---|
| `version` | uint64 | exactly 1 |
| `bindingDigest` | SHA-256 | digest of canonical `StreamBindingV1` |
| `cursor` | uint64 | 1..maxEvents, strictly monotonic at publisher |
| `eventType` | enum | `Application=1`, `End=2` |
| `payload` | bytes | opaque application event; empty only for End |
| `payloadDigest` | SHA-256 | digest of payload bytes |
| `publishedAtUs` | uint64 | evidence timestamp, not acceptance authority |
| `userToken` | bytes | exact token echoed from request |
| `terminal` | bool | true iff event type is End |
| `end` | EndEventV1 | present iff terminal |

`bindingDigest`, Data name, signer, AEAD associated data, cursor, and decoded
fields must agree before buffering. `publishedAtUs` is advisory and cannot
override lineage or deadline.

## 4. EndEventV1

| Field | Type | Rule |
|---|---|---|
| `finalCursor` | uint64 | equals containing event cursor |
| `finishReason` | enum | EOS, STOP_SEQUENCE, MAX_TOKENS, APPLICATION_COMPLETE, DEADLINE, CANCELLED, FAILED |
| `applicationEventCount` | uint64 | count excluding End |
| `generatedTokenCount` | uint64 | application-supplied; <= maxEvents-1 |
| `transcriptDigest` | SHA-256 | digest chain over accepted application events |
| `finalResultDigest` | SHA-256 | digest of complete final application payload |
| `errorCode` | enum/string | empty on successful reasons; bounded on failure |

Transcript digest chain:

```text
D0 = SHA256("NDNSF-STREAM-V1")
Di = SHA256(D(i-1) || cursor || eventType || payloadDigest)
```

The End event itself is not added to the application transcript chain.

## 5. StreamCompletionV1

Optional block in the authoritative `ResponseMessage`.

| Field | Type | Rule |
|---|---|---|
| `bindingDigest` | SHA-256 | exact accepted binding |
| `endEventName` | Name | exact full End Data name |
| `finalCursor` | uint64 | equals End |
| `finishReason` | enum | equals End |
| `applicationEventCount` | uint64 | equals End |
| `transcriptDigest` | SHA-256 | equals End and local consumer chain |
| `finalResultDigest` | SHA-256 | equals End and Response payload digest |

Successful application completion is delivered only after all equality checks
pass and cursors 1..`finalCursor` have been accepted in order.

## 6. StreamedInvocationState

User-side states:

```text
Created -> Requesting -> Selecting -> Streaming -> Draining -> Completed
                    \          \          \          \-> Failed
                     \          \          \-----------> Cancelled
                      \----------\----------------------> Failed
```

| State | Accepted actions |
|---|---|
| `Created` | serialize request and allocate binding seed |
| `Requesting` | collect/validate ACKs or Targeted binding |
| `Selecting` | accept one committed plan and final event producer |
| `Streaming` | fetch/buffer/deliver application events |
| `Draining` | End and/or Response seen; close remaining declared gaps |
| `Completed` | read result/metrics only |
| `Failed` | read terminal error/metrics only |
| `Cancelled` | read cancellation/metrics only |

All terminal states are absorbing. Cancellation is idempotent from a terminal
state and fencing from any nonterminal state.

Provider writer states:

```text
Prepared -> Active -> Ending -> Finished
    \          \          \------> Fenced
     \----------\----------------> Failed
```

Cursor commitment occurs only after bounded queue admission. `finish` first
admits End, then claims/publishes the final Response exactly once.

## 7. StreamedInvocationMetrics

All counters are per invocation and bounded.

| Field | Meaning |
|---|---|
| `eventsPublished` | application plus End accepted by publisher queue |
| `eventsFetched` | valid event Data fetches including duplicates |
| `eventsDelivered` | application callbacks/iterator values only |
| `duplicateEvents` | valid duplicate cursor Data suppressed |
| `staleEvents` | lineage-failed Data |
| `gapRetries` | exact cursor Interest re-expressions |
| `maxPublisherQueueDepth` | high-water mark |
| `maxCallbackQueueDepth` | high-water mark |
| `maxReorderDepth` | high-water mark |
| `backpressureUs` | cumulative provider admission wait |
| `ttftUs` | Request publication to first application event delivery |
| `interEventUs` | bounded per-cursor intervals or summary histogram |
| `terminalLatencyUs` | Request publication to complete Response delivery |
| `finishReason` | accepted terminal reason |

## 8. GenerationTokenEventV1

Application payload; Core treats the bytes as opaque.

| Field | Type | Rule |
|---|---|---|
| `tokenId` | int64 | sampled vocabulary token |
| `tokenEpoch` | uint64 | 1-based generated-token count |
| `textDelta` | UTF-8 bytes/string | incremental decoder output, may be empty |
| `cumulativeTokenCount` | uint64 | equals tokenEpoch |
| `finishHint` | enum | NONE, EOS, STOP_SEQUENCE, MAX_TOKENS |
| `samplingDigest` | SHA-256 | sealed sampling configuration identity |

## 9. QwenGenerationSpecV2

Extends the current generation spec without changing plan ownership.

Required additions:

- tokenizer and adapter digests;
- sampling digest and canonical parameters;
- EOS token set and ordered stop strings;
- deterministic seed;
- accepted-token prefix digest/count;
- `StreamBindingV1` digest;
- prefill/decode runner and I/O-layout identities;
- explicit replacement permission and maximum;
- exact event and terminal-result contracts.

## 10. KvStateIdentityV1

| Field | Comparison |
|---|---|
| `modelDigest` | exact |
| `graphSemanticDigest` | exact |
| `artifactDigest` | exact |
| `adapterDigest` | exact |
| `tokenizerDigest` | exact |
| `runnerDigest` | exact |
| `roleName` | exact |
| `roleSplitDigest` | exact |
| `layerRange` | exact |
| `prefixDigest` | exact |
| `prefixTokenCount` | exact |
| `positionDigest` | exact |
| `precision` | exact |
| `layoutDigest` | exact |
| `runtimeAbiDigest` | exact |
| `securityDomainDigest` | exact |
| `providerIdentity` | exact |
| `providerBootId` | exact |
| `cacheEpoch` | exact |
| `requestId` | exact for version 1 |
| `attemptEpoch` | exact |
| `generationId` | exact |

Version 1 intentionally forbids cross-request prefix sharing even when public
prompt bytes match. A future Spec may define privacy-safe shared-prefix reuse.

## 11. QualificationManifestV1

Required fields:

- schema/version and gate ID;
- start/end UTC timestamps and status;
- git commit/tree and dirty-path inventory;
- SIF, native library, Python extension, model, tokenizer, adapter, workload,
  and contract digests;
- Python/SOABI, glibc, Boost, ndn-cxx, NDN-SVS, NFD, MiniNDN, ONNX Runtime,
  CUDA/driver, Apptainer, and compiler versions;
- topology and exact one-to-one Provider-role map;
- seed, prompt IDs, warmup/repetition counts, maximum tokens, timeouts, and
  stream options;
- request/event/End/Response counts and transcript/oracle verdicts;
- TTFT, per-token latency summary, total latency, tokens/s, retransmission,
  queue, KV, CPU fallback, GPU/CPU/memory, and failure metrics;
- command, working directory, environment allowlist, artifact paths, and SHA-256
  hashes of evidence files;
- final verdict: `FAIL`, `FUNCTIONAL_PASS_PERFORMANCE_MISS`, or
  `PERFORMANCE_PASS`.
