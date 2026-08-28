# Data Model: UAV Sensor Stream Generality

## ReferenceImplementationProvenance

Fields:

- reference Spec and completion/audit paths;
- immutable MiniNDN result root;
- campaign-summary and run-summary SHA-256 identities;
- reusable pattern list;
- prohibited workload-semantic list;
- verification timestamp and verdict.

Validation:

- Spec 145 post-implementation verdict is `PASS`;
- recorded hashes match the promoted result files;
- reusable patterns are limited to APP/Core ownership, generic Streaming
  lifecycle, security/admission, callback containment, and truthful Core
  status;
- video class, key/delta, FPS/GOP, codec, payload, and measured-threshold
  semantics are absent from telemetry/acoustic implementation decisions;
- Spec 145 observations never enter a Spec 144 cell or treatment denominator.

## TelemetrySample

Fields:

- `streamId`, `sessionEpoch`, `sampleId`
- `sourceTimestampNs` in the declared monotonic clock domain
- `droneId`
- compact position/motion fields
- compact battery/readiness/link fields
- `encodedSize` in `{256, 384, 512}`
- deterministic content digest
- protected opaque bytes

Validation:

- exactly one source item and zero repair items;
- sample ID and source timestamp strictly increase;
- encoded size follows the frozen cycle;
- identity/session/name binding validates before application admission;
- a late older sample may be measured but cannot replace newer application
  state.

State transition:

```text
scheduled -> announced -> produced -> protected -> published
          -> fetched -> verified -> admitted | rejected | terminal-skip
```

## AcousticBlock

Fields:

- `streamId`, `sessionEpoch`, `blockId`
- `captureTimestampNs`
- `actualSourceItems` in `{2, 3, 4}`
- ordered source-item digests and opaque bytes
- two repair symbols
- per-item provenance: signed Data or FEC recovered
- block completion and terminal reason

Validation:

- source count follows the frozen 2/3/4 cycle and never exceeds four;
- each source item is at most 512 encoded bytes;
- source and repair name/index/extent bindings validate;
- direct plus recovered bytes exactly match the expected ordered source bytes;
- application receives a block once, only after complete reconstruction.

State transition:

```text
scheduled -> announced -> extent-prepared -> produced/protected -> published
          -> partial-fetch -> complete-direct
                           -> recovery-pending -> complete-recovered
                           -> terminal-skip
```

## StreamWorkloadDefinition

Fields:

- workload ID and semantic type;
- cadence and measured duration/count;
- payload/source-count schedule;
- source byte bound and FEC rule;
- Mapping capacity/ahead policy, retention, pending-Interest bound;
- latest-start and adaptive sample-atomic consumer policy;
- security and clock-domain identifiers;
- immutable configuration digest.

Validation:

- workload definition is frozen before formal execution;
- application semantics do not become Core policy inputs;
- formal runner rejects a config digest mismatch.

## InterestOutcome

Fields:

- cell, node, stream/session, Interest kind;
- exact name, cursor/block/item kind/index, attempt number;
- initial or retry; future or already-produced;
- expression timestamp and terminal timestamp;
- terminal type: new Mapping, duplicate Mapping, admitted source, consumed
  repair, unconsumed repair, duplicate Data, Nack, timeout, late, suppressed,
  or unresolved;
- linked application sample/block and recovery attempt, if any.

Derived categories:

- **application-useful**: admits a new source item or provides repair bytes
  consumed by a successful reconstruction;
- **protection-only**: obtains valid repair bytes not consumed by recovery;
- **nonproductive**: obtains no newly admissible source/repair information.

Conservation:

```text
all Payload Interest attempts
  = application-useful + protection-only + nonproductive
```

## LatencyObservation

Fields:

- workload, cell, sample/block ID;
- clock domain;
- origin event/timestamp;
- terminal event/timestamp;
- duration;
- delivery/recovery provenance;
- measured-window and inclusion flags;
- exclusion reason.

Telemetry origin is source observation; terminal is monotonic application
admission. Acoustic/audio origin is block capture-ready; terminal is complete
block admission.

## FormalCell

Fields:

- workload/profile/repetition and stable cell ID;
- unique output path and invocation count;
- exact command, source/tree, binary, config, policy, and analyzer digests;
- topology and qdisc before/after state;
- launcher and cleanup ownership;
- readiness, warm-up, measured-window, and sample-count gates;
- process exit/timeout state;
- metric summary and gate verdict.

State transition:

```text
declared -> preflighted -> running -> terminal
                            terminal = accepted | failed | incomplete
```

No terminal formal cell can return to `running`.

## WorkloadVerdict

Fields:

- workload and treatment profile;
- accepted/attempted repetition count;
- required accepted count;
- exact binomial interval for five-repetition treatments;
- controlling failed metrics and cell IDs;
- positive or negative workload conclusion.

The shared verdict is positive only when every workload verdict passes.
