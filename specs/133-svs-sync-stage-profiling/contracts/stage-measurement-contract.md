# Stage Measurement Contract

## Structured Record Contract

The dedicated logger emits single-line key/value records beginning with one of:

```text
schema=spec133-stage-span-v1 event=stage-span
schema=spec133-stage-summary-v1 event=stage-summary
schema=spec133-profile-lifecycle-v1 event=profile-start|profile-stop
```

Every span carries `cell`, `peer`, `stage`, `kind`, `thread`, `trace`,
`startRawNs`, `durationNs`, `outcome`, `bytes`, `items`, `sampleModulus`, and
`correlationMode=exact|ambiguous|censored`. Summary records carry exact call/
outcome/sample/drop counts and all-call total/min/max duration. Unknown enum
values, malformed integers, negative/impossible durations, cross-cell records,
or wrong sample modulus make the affected process trace invalid.

Publication, Mapping, and Payload keys derive from existing node/sequence/range
identities. Sync Interest send/receive joins may use only existing wire-derived
fields and occurrence ordering. No wire-visible trace field may be added.
Ambiguous or censored joins never contribute a network-wait duration or a
critical-path conclusion.

## Frozen Stage Registry

### Application publication on the Face/io_context thread

| ID | Kind | Parent | Measured work |
|---|---|---|---|
| `PUB.TOTAL` | aggregate | - | Entire synchronous `SVSPubSub::publish()` call |
| `PUB.VV_READ_LOCK_WAIT` | lock-wait | `PUB.TOTAL` | Wait to acquire the version-vector mutex for sequence read |
| `PUB.SEQ_READ` | leaf-cpu | `PUB.TOTAL` | Read/reserve next local sequence |
| `PUB.INNER_BUILD` | leaf-cpu | `PUB.TOTAL` | Construct inner publication Data and set fields/content |
| `PUB.INNER_SIGN` | leaf-cpu | `PUB.TOTAL` | Sign inner publication Data |
| `PUB.INNER_WIRE_ENCODE` | leaf-cpu | `PUB.TOTAL` | Encode/read inner wire and size |
| `PUB.EXTRA_DATA_LOCK_WAIT` | lock-wait | `PUB.TOTAL` | Wait to acquire the piggyback/extra-data mutex |
| `PUB.PIGGY_QUEUE_INSERT` | leaf-cpu | `PUB.TOTAL` | Append eligible Data after acquiring the piggyback mutex |
| `PUB.OUTER_BUILD` | leaf-cpu | `PUB.TOTAL` | Build outer SVS Data and set content/metadata |
| `PUB.OUTER_SIGN` | leaf-cpu | `PUB.TOTAL` | Sign outer SVS Data |
| `PUB.OUTER_STORE_INSERT` | leaf-cpu | `PUB.TOTAL` | Insert outer Data into MemoryDataStore |
| `PUB.VV_UPDATE_LOCK_WAIT` | lock-wait | `PUB.TOTAL` | Wait to acquire the version-vector mutex for update |
| `PUB.LOCAL_STATE_UPDATE` | leaf-cpu | `PUB.TOTAL` | Update local version vector, excluding timer work |
| `PUB.SCHEDULER_LOCK_WAIT` | lock-wait | `PUB.TOTAL` | Wait to acquire the Sync scheduler mutex after local update |
| `PUB.OUTER_FACE_PUT` | leaf-cpu | `PUB.TOTAL` | Invoke Face put for outer Data |
| `MAP.TIMESTAMP_BUILD` | leaf-cpu | `PUB.TOTAL` | Create optional Mapping timestamp component |
| `MAP.NOTIFICATION_ENQUEUE` | leaf-cpu | `PUB.TOTAL` | Append Mapping notification entry |
| `MAP.STORE_INSERT` | leaf-cpu | `PUB.TOTAL` | Insert Mapping in MappingProvider map |

### Sync Interest production on the same Face/io_context thread

| ID | Kind | Parent | Measured work |
|---|---|---|---|
| `SYNC.TIMER_ARM_CANCEL` | leaf-cpu | - | Cancel/arm local-publication Sync timer |
| `SYNC.TIMER_WAIT` | queue-wait | - | Requested 1 ms deadline to actual callback; record coalesced updates |
| `SYNC.PRODUCE_TOTAL` | aggregate | - | Entire `sendSyncInterest()` work |
| `SYNC.EXTRA_BLOCK_TOTAL` | aggregate | `SYNC.PRODUCE_TOTAL` | Build notification Mapping and piggyback block |
| `SYNC.EXTRA_DATA_LOCK_WAIT` | lock-wait | `SYNC.EXTRA_BLOCK_TOTAL` | Wait to acquire the Mapping/piggyback extra-data mutex |
| `SYNC.MAPPING_CANDIDATE_ENCODE` | leaf-cpu | `SYNC.EXTRA_BLOCK_TOTAL` | Each candidate MappingList encode used for fit selection |
| `SYNC.PIGGY_WIRE_SCAN` | leaf-cpu | `SYNC.EXTRA_BLOCK_TOTAL` | Encode/size/select queued piggyback Data |
| `SYNC.EXTRA_BLOCK_FINAL_ENCODE` | leaf-cpu | `SYNC.EXTRA_BLOCK_TOTAL` | Final extra block parse/append/encode and queue update |
| `SYNC.VV_ENCODE_LOCK_WAIT` | lock-wait | `SYNC.PRODUCE_TOTAL` | Wait to acquire the version-vector mutex for Sync encoding |
| `SYNC.VV_ENCODE` | leaf-cpu | `SYNC.PRODUCE_TOTAL` | Encode current version vector |
| `SYNC.APP_PARAMS_ENCODE` | leaf-cpu | `SYNC.PRODUCE_TOTAL` | Prepend outer lengths and finalize ApplicationParameters wire after excluding extra-block and vector encoding |
| `SYNC.INTEREST_BUILD` | leaf-cpu | `SYNC.PRODUCE_TOTAL` | Construct Interest and set parameters/lifetime |
| `SYNC.INTEREST_SIGN` | leaf-cpu | `SYNC.PRODUCE_TOTAL` | Sign Sync Interest using resolved mode |
| `SYNC.INTEREST_EXPRESS` | leaf-cpu | `SYNC.PRODUCE_TOTAL` | Invoke Face expressInterest |

### Sync Interest receive and state processing

| ID | Kind | Parent | Measured work |
|---|---|---|---|
| `SYNC.NETWORK_WAIT` | external-wait | - | Express-to-peer-receive interval where correlatable |
| `SYNC.RECEIVE_TOTAL` | aggregate | - | Entire validated receive/state path before return |
| `SYNC.INTEREST_VERIFY` | leaf-cpu | `SYNC.RECEIVE_TOTAL` | HMAC/digest/validator path or explicit disabled count |
| `SYNC.APP_PARAMS_PARSE` | leaf-cpu | `SYNC.RECEIVE_TOTAL` | Obtain and parse ApplicationParameters |
| `SYNC.VV_DECODE` | leaf-cpu | `SYNC.RECEIVE_TOTAL` | Construct VersionVector from StateVector TLV |
| `SYNC.EXTRA_MAPPING_DECODE` | leaf-cpu | `SYNC.RECEIVE_TOTAL` | Decode MappingList blocks and insert mappings |
| `SYNC.PIGGY_DATA_DECODE_CACHE` | leaf-cpu | `SYNC.RECEIVE_TOTAL` | Decode piggyback Data, satisfy/cache it, and evict if needed |
| `SYNC.VV_MERGE_LOCK_WAIT` | lock-wait | `SYNC.RECEIVE_TOTAL` | Wait to acquire the version-vector mutex for merge |
| `SYNC.VV_MERGE` | leaf-cpu | `SYNC.RECEIVE_TOTAL` | Compare/merge remote and local state vectors |
| `SYNC.UPDATE_DISPATCH` | leaf-cpu | `SYNC.RECEIVE_TOTAL` | Dispatch missing ranges into SVSPubSub update path |
| `SYNC.SUPPRESSION_DECISION` | leaf-cpu | `SYNC.RECEIVE_TOTAL` | Record vector and schedule immediate/suppressed response |
| `SYNC.RECORDED_VV_LOCK_WAIT` | lock-wait | `SYNC.RECEIVE_TOTAL` | Wait to acquire the recorded-vector mutex during suppression handling |
| `SYNC.SCHEDULER_LOCK_WAIT` | lock-wait | `SYNC.RECEIVE_TOTAL` | Wait to acquire the scheduler mutex while rescheduling a response |

### Mapping resolution and fallback

| ID | Kind | Parent | Measured work |
|---|---|---|---|
| `MAP.PROCESS_TOTAL` | aggregate | - | Entire `processMapping()` call |
| `MAP.LOCAL_LOOKUP` | leaf-cpu | `MAP.PROCESS_TOTAL` | MappingProvider local map lookup |
| `MAP.FRESHNESS_CHECK` | leaf-cpu | `MAP.PROCESS_TOTAL` | Timestamp block scan and age decision |
| `MAP.SUBSCRIPTION_MATCH` | leaf-cpu | `MAP.PROCESS_TOTAL` | Prefix/regex subscription matching |
| `MAP.EXTRA_DATA_LOCK_WAIT` | lock-wait | `MAP.PROCESS_TOTAL` | Wait to acquire the piggyback cache mutex |
| `MAP.PIGGY_CACHE_LOOKUP` | leaf-cpu | `MAP.PROCESS_TOTAL` | Exact/prefix piggyback cache lookup |
| `MAP.FETCH_QUEUE_INSERT` | leaf-cpu | `MAP.PROCESS_TOTAL` | Enqueue publication for later fetch |
| `MAP.PIGGY_CALLBACK` | leaf-cpu | `MAP.PROCESS_TOTAL` | Deliver piggyback payload to subscription callback |
| `MAP.INTEREST_BUILD` | leaf-cpu | - | Build fallback Mapping Interest/range name |
| `MAP.FETCHER_QUEUE_WAIT` | queue-wait | - | Mapping Interest queue admission to express |
| `MAP.INTEREST_EXPRESS` | leaf-cpu | - | Invoke Face expressInterest for Mapping |
| `MAP.NETWORK_WAIT` | external-wait | - | Mapping Interest express to Mapping Data receive |
| `MAP.QUERY_PARSE` | leaf-cpu | - | Provider parse Mapping range query |
| `MAP.RANGE_LOOKUP` | leaf-cpu | - | Provider lookup requested Mapping entries |
| `MAP.LIST_ENCODE` | leaf-cpu | - | Encode MappingList response |
| `MAP.DATA_BUILD` | leaf-cpu | - | Build Mapping Data and set content/freshness |
| `MAP.DATA_SIGN` | leaf-cpu | - | Sign Mapping Data |
| `MAP.DATA_FACE_PUT` | leaf-cpu | - | Invoke Face put for Mapping Data |
| `MAP.DATA_VERIFY` | leaf-cpu | - | Consumer Mapping Data validation or disabled count |
| `MAP.CONTENT_EXTRACT` | leaf-cpu | - | Extract Mapping content Block |
| `MAP.LIST_DECODE` | leaf-cpu | - | Decode received MappingList |
| `MAP.REMOTE_STORE_INSERT` | leaf-cpu | - | Insert newly received mappings locally |

### Publication payload fallback and callback boundary

| ID | Kind | Parent | Measured work |
|---|---|---|---|
| `PAYLOAD.INTEREST_BUILD` | leaf-cpu | - | Build publication Data Interest |
| `PAYLOAD.FETCHER_QUEUE_WAIT` | queue-wait | - | Publication Interest queue admission to express |
| `PAYLOAD.INTEREST_EXPRESS` | leaf-cpu | - | Invoke Face expressInterest for publication |
| `PAYLOAD.NETWORK_WAIT` | external-wait | - | Publication Interest express to outer Data receive |
| `PAYLOAD.PROVIDER_STORE_FIND` | leaf-cpu | - | Provider MemoryDataStore lookup |
| `PAYLOAD.PROVIDER_FACE_PUT` | leaf-cpu | - | Provider Face put of stored Data |
| `PAYLOAD.OUTER_VERIFY` | leaf-cpu | - | Consumer outer Data validation or disabled count |
| `PAYLOAD.OUTER_CACHE_INSERT` | leaf-cpu | - | Cache validated outer Data |
| `PAYLOAD.INNER_DECODE` | leaf-cpu | - | Extract content block and construct inner Data |
| `PAYLOAD.INNER_VERIFY` | leaf-cpu | - | Validate encapsulated inner Data or disabled count |
| `PAYLOAD.SUBSCRIPTION_CALLBACK` | leaf-cpu | - | NDNSF-free library callback dispatch duration |
| `APP.PAYLOAD_CHECK` | leaf-cpu | - | Benchmark-only payload decode/hash/duplicate check |
| `APP.STATE_UPDATE` | milestone | - | Benchmark state-update timestamp |
| `APP.DELIVERY` | milestone | - | Benchmark validated-delivery timestamp |

## Accounting Rules

1. Only `leaf-cpu` spans with non-overlapping registry semantics enter CPU
   demand/share sums.
2. `aggregate` spans provide total, residual, and containment checks only.
3. `queue-wait` and `external-wait` are reported separately and never included
   in CPU share.
4. `lock-wait` is reported separately; the corresponding protected leaf timer
   starts after acquisition so the intervals do not overlap.
5. A disabled signer/validator path has an exact zero-call or explicit
   `outcome=skipped` record; it is not silently absent.
6. Mapping/Payload piggyback and fallback opportunities use exact counters.
7. The analyzer reports both peers and both delivery directions independently.
8. No bottleneck claim is valid if subject identity, overhead gate, stage
   schema, or terminal receipt validation fails.
9. Formal attribution is limited to the compression-disabled, non-segmented
   256-byte path; segmented and compressed branches are explicit exclusions.
