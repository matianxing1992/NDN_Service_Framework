# Data Model: SVS PubSub Payload Interoperability

## PayloadCase

| Field | Meaning | Validation |
|---|---|---|
| `caseId` | Stable case key | Unique, non-empty |
| `name` | Application NDN name | Absolute and unique per sender/case |
| `path` | Canonical payload file | Regular file under the run directory |
| `length` | Expected byte count | Equals file size |
| `sha256` | Expected byte digest | 64 lowercase hexadecimal characters |
| `requiresSegmentation` | Required transfer property | True only when one Data packet cannot contain the object |
| `segmentHint` | Publisher chunk/packet boundary | Positive when segmentation is required |

## PayloadReceipt

| Field | Meaning | Validation |
|---|---|---|
| `implementation` | `cpp` or `ndnts-typescript` | Fixed enumeration |
| `event` | publish, sync-update, mapping/fetch error, receive, shutdown | Fixed enumeration |
| `direction` | Sender to receiver | Matches implementation and expected sender |
| `caseId` | Corpus case | Exists in manifest |
| `name` | Decoded application name | Exact normalized name match |
| `sequence` | SVS publication sequence | Positive integer for publish/receive |
| `length` | Received byte count | Exact manifest match |
| `sha256` | Received digest | Exact manifest match |
| `segments` | Configured/observed segment count | Greater than one for segmented case |
| `stage` | Last protocol stage | Required for errors and timeouts |
| `reason` | Bounded diagnostic | Non-empty on reject/error |

## InteropCell

| Field | Meaning |
|---|---|
| `cellId` | Stable standalone or MiniNDN identity |
| `lossPercent` | Link-loss setting |
| `corpusSha256` | Identity of the canonical manifest |
| `peerSha256` | C++ binary and TypeScript source identities |
| `expectedReceipts` | Four cases times two directions |
| `observedReceipts` | Verified unique receipts |
| `duplicates` | Repeated receipt identities |
| `errors` | Classified peer or orchestration failures |
| `packetEvidence` | Capture paths and SHA-256 digests |
| `status` | SUCCESS, INTEROP_INCOMPATIBLE, INFRA_FAILURE, or NOT_ADMITTED |

## State Rules

```text
CREATED -> PREFLIGHTED -> RUNNING
RUNNING -> SUCCESS
RUNNING -> INTEROP_INCOMPATIBLE
RUNNING -> INFRA_FAILURE
INTEROP_INCOMPATIBLE -> NOT_ADMITTED (next validation layer only)
```

Only `SUCCESS` permits the next validation layer. StateVector convergence may
update progress but cannot change the cell to `SUCCESS` without all exact
payload receipts.
