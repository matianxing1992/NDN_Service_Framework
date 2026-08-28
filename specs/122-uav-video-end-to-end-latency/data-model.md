# Data Model: UAV Video True End-to-End Latency

## SourceFrameIdentity

- `streamId`: stable NDNSF LiveStream identifier
- `sessionEpoch`: anti-replay stream generation
- `sourceFrameId`: monotonic frame identity within the session
- `captureOriginNs`: earliest observable acquisition time
- `captureClockId`: clock authority for the acquisition time
- `codecPts`: codec presentation timestamp in a declared time base
- `timeBaseNumerator`, `timeBaseDenominator`: PTS time base
- `keyFrame`: whether the source output is independently decodable
- `codecConfigEpoch`: codec configuration generation
- `bindingVersion`: additive metadata contract version

Invariant: `(streamId, sessionEpoch, sourceFrameId)` is unique. PTS maps to at most one live source frame within one codec configuration epoch. Identity metadata is authenticated with the UAV payload and never inferred from queue order.

## VideoStageEvent

- `identity`: `SourceFrameIdentity` or an explicit uncorrelated output identity
- `role`: provider, consumer, or GUI
- `stage`: canonical stage from the evidence contract
- `timestampNs`: monotonic stage time
- `clockId`: clock authority
- `phase`: startup, warmup, or steady
- `queueDepth`: bounded queue depth at the event
- `outcome`: accepted, displayed, dropped, unavailable, or failed
- `reason`: machine-readable drop/unavailability/failure reason
- `sampled`: whether the identity belongs to the stable sampler

Invariant: a duration exists only for compatible endpoints with the same exact identity and a safe clock relationship.

## VideoPipelineCapabilities

- `backend`: `legacy-pipe`, `gstreamer`, or a future explicit adapter
- `captureTimestamp`: none, application-ingress, driver, or sensor
- `codecPtsRoundTrip`: supported or unsupported
- `accessUnitAlignment`: supported or unsupported
- `persistentDecoder`: supported or unsupported
- `directRendering`: none, widget-submission, or observable-presentation
- `hardwareDecode`: supported or unsupported
- `headless`: supported or unsupported
- `versionDigest`: backend/library/plugin identity

Invariant: a capability is advertised only after a focused executable probe; configuration never assumes plugin or hardware availability.

## BoundedFrameQueueSnapshot

- `queueName`: encoder, transport admission, decoder input, decoded mailbox, or GUI
- `capacity`: hard item/byte bound
- `depth`, `bytes`: current occupancy
- `highWaterDepth`, `highWaterBytes`: session maxima
- `accepted`, `displayed`, `dropped`: cumulative counts
- `dropReasons`: per-reason counters
- `oldestFrameAgeMs`: age of oldest retained frame

Invariant: capacity is finite; every discarded frame has one reason and identity; a stopped/retired session cannot deliver a queued callback.

## PresentationObservation

- `identity`: exact decoded source identity when available
- `decoderOutputOrdinal`: diagnostic output order, never an identity substitute
- `callbackQueuedNs`: decoder-to-GUI callback enqueue time
- `widgetSubmittedNs`: GTK/widget update completion time
- `presentedNs`: optional backend-observed presentation time
- `presentationAuthority`: widget-only, sink-clock, compositor, or unavailable
- `displayRefreshHz`: observed/configured refresh context
- `staleAtSubmission`: whether a newer frame superseded it

Invariant: `presentedNs` is absent unless the backend contract proves what it observes. Widget submission is never relabelled physical display.

## PipelineCandidate

- `candidateId`: immutable experiment identity
- `backend`: selected pipeline adapter
- `changedVariable`: exactly one change from matched baseline
- `configurationDigest`: frozen effective settings
- `rollback`: explicit baseline selection
- `capabilities`: verified `VideoPipelineCapabilities`
- `status`: planned, probed, rejected, retained-experimental, or retained-default
- `decisionReason`: failed gate or retained evidence

## MatchedExperimentCell

- `cellId`, `pairId`, `order`: unique frozen identity and precommitted paired order
- `evidenceRole`: exploratory or confirmatory; exploratory cells cannot satisfy the default gate
- `predecessorCellId`: prior cell in the frozen execution order, when present
- `candidate`: immutable candidate configuration
- `source/topology/workload`: frozen inputs
- `warmupSeconds`, `measuredSeconds`: timing contract
- `traceMode`, `sampleRate`: instrumentation state
- `completion`: completed, failed, invalid, or timed out
- `stageDistributions`: counts and p50/p95/p99 by exact endpoints
- `correctness`: identity, security, continuity, and frame accounting gates
- `efficiency`: Interest/future-hit, CPU, RSS, PIT, and queue metrics
- `evidencePaths`: logs, CSV, JSON, screenshots when applicable

State: `planned -> running -> completed|failed|invalid|timed-out`. A terminal cell is immutable and cannot be silently rerun with the same identity. Confirmatory plans are frozen only after candidate selection and before the first confirmatory cell starts.
