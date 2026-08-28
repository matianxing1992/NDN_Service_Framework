# Contract: Payload Corpus and Receipts

## Corpus Manifest

The generated JSON manifest contains a schema version, application prefix,
sender set, and ordered `cases`. Every case contains `caseId`, sender-specific
`names`, `path`, `length`, `sha256`, `requiresSegmentation`, and `segmentHint`.
Paths resolve relative to the manifest directory. Both peers consume the same
immutable manifest.

## Peer JSONL Events

Every line is one JSON object with:

```text
event, implementation, protocolVersion, timestampNs
```

Publication and receipt events additionally contain:

```text
direction, caseId, name, sequence, length, sha256, segments
```

Errors additionally contain:

```text
stage, reason
```

Required stages are `sync`, `mapping`, `outer-fetch`, `inner-decode`,
`validation`, `reassembly`, and `payload-check`. Implementations may emit a
more precise reason, but orchestration must retain one of these stable stages.

## Acceptance

For each `(direction, caseId)` exactly one receive receipt must match:

```text
expected name == received name
expected length == received length
expected sha256 == received sha256
```

For `requiresSegmentation=true`, `segments > 1` must also hold. Missing and
duplicate receipts fail the cell. Sync updates are diagnostic and never count
as receive receipts.

## Claim Levels

- `implemented`: peer/harness source exists.
- `executed`: both real peers ran and exchanged protocol traffic.
- `measured-negative`: bounded evidence identifies an incomplete direction or
  incompatible stage.
- `measured-compatible`: every bilateral exact payload gate passed.

No lower level may be described as application-data interoperability.
