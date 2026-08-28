# Frozen Replacement High-Level API Contract

This contract is intentionally breaking. The API below replaces the old
high-level `start(initial...)` / `announce()` / `publish()` workflow.

## Provider — C++17

```cpp
class StreamPublisher
{
public:
  PredictiveStreamDescriptor start();
  void push(std::shared_ptr<ndn::Data> signedData);
  void flush();
  LiveStreamStatus status() const;
  void stop();
};
```

The following public methods MUST NOT exist after migration:

```cpp
LiveStreamDescriptor start(uint64_t, const std::string&,
                           const std::vector<std::vector<uint8_t>>&);
void announce(uint64_t, const std::string&);
void publish(uint64_t, const std::vector<std::vector<uint8_t>>&);
```

`push()` transfers an immutable shared packet to Core. Core validates and
retains the exact signed wire and MUST NOT put that wire inside another Data
packet or sign it again.

## Provider — Python

```python
class StreamPublisher:
    def start(self) -> "PredictiveStreamDescriptor": ...
    def push(self, signed_data: bytes) -> None: ...
    def flush(self) -> None: ...
    def status(self) -> "LiveStreamStatus": ...
    def stop(self) -> None: ...
```

Python MUST NOT expose `announce`, `publish`, or `start_predictive`.

## Descriptor

```cpp
struct PredictiveStreamCheckpoint
{
  uint64_t initialSampleId = 0;
  uint64_t oldestRetainedSampleId = 0;
  uint64_t latestProducedSampleId = 0;
  uint64_t nextExpectedSampleId = 0;
};

struct PredictiveStreamDescriptor
{
  LiveStreamDefinition definition;
  PredictiveStreamCheckpoint checkpoint;
  ndn::Name frontierName;
  double measuredSamplePeriodMs = 0.0;
};
```

C++ and Python expose the same fields and validation rules.

## Frozen Predictive Wire Names

Let `R = descriptor.definition.mappingRoot()` and
`V = descriptor.definition.mappingVersion`. The replacement API uses exactly
these names:

```text
App-signed source Data:
  R/v/V/<SequenceNumber>

Core-signed group commit Data:
  R/v/V/group/<GroupId>

Core-signed repair Data:
  R/v/V/group/<GroupId>/repair/<RepairIndex>

Core-signed live frontier Data:
  R/frontier
```

`v`, `group`, and `repair` are generic keyword components. `V` and `GroupId`
are non-negative number components; the source suffix is an NDN SequenceNumber
component; `RepairIndex` is a non-negative number component. No alternate
spelling, implicit digest suffix, semantic application name, or workload name
is accepted by this high-level path.

The App owns and signs only source Data. Core owns and signs group commit,
repair, and frontier Data. `push()` accepts sources in strictly increasing,
gap-free SequenceNumber order beginning at
`descriptor.checkpoint.nextExpectedSampleId`. A byte-identical repeat is
reported as a duplicate and is not committed twice; the same name with
different wire bytes is equivocation and permanently fails the session.

## Frozen Group Commit and Frontier Content

The canonical group commit contains:

```text
contractVersion
streamId
sessionEpoch
mappingVersion
groupId
createdMs
expiresMs
ordered source names
exact source signed-wire lengths
SHA-256 digest of each exact source wire
ordered repair names
declared recovery capacity
```

The signed source wire, rather than only `Data::Content`, is the repair symbol.
This permits a recovered item to be validated and admitted exactly like a
received App-signed Data packet. Unequal source lengths are zero-padded only
for coding and are truncated back to the committed per-source length after
recovery.

The canonical frontier contains:

```text
contractVersion
streamId
sessionEpoch
mappingVersion
initialSampleId
oldestRetainedSampleId
latestProducedSampleId
nextExpectedSampleId
latestCommittedGroupId
ordered retained group-commit names
```

The frontier is replaced only after all source membership, repair Data, and
the group commit have been staged successfully. A Consumer validates the
frontier signature and identity before using it for late join or recovery.
Frontier regression, a changed wire under an already admitted group/frontier
name, or metadata that disagrees with the descriptor is rejected as
equivocation.

## Consumer — C++17

```cpp
struct StreamSubscriptionOptions
{
  std::function<LiveStreamItemAdmission(const VerifiedLiveStreamItem&)> onItem;
  std::function<void(const LiveStreamStatus&)> onStatus;
  bool enableFecRecovery = true;
  bool requireFullDelivery = false;
  size_t aggregateInterestLimit = 64;
  uint64_t interestLifetimeMs = 500;
  LiveStreamStart start = LiveStreamStart::Latest;
};

class PredictiveStreamSubscriber
{
public:
  void start();
  LiveStreamStatus status() const;
  void stop();
};

std::shared_ptr<PredictiveStreamSubscriber>
ServiceUser::subscribeStream(const PredictiveStreamDescriptor& descriptor,
                             StreamSubscriptionOptions options);
```

The former high-level `subscribeStream(LiveStreamDescriptor, ...)` facade is
removed. Internal low-level consumers may remain as implementation details.

## Consumer — Python

```python
subscriber = user.subscribe_stream(
    descriptor=predictive_descriptor,
    options=StreamSubscriptionOptions(on_item=on_item),
)
subscriber.start()
subscriber.stop()
```

The returned object wraps `PredictiveStreamSubscriber`, not the old
`LiveStreamConsumerHandle`.

## Lifecycle and Commit Semantics

```text
start()
  -> descriptor available; routes/frontier ready

push(packet)*
  -> validate authority/name/epoch/signature/budget
  -> retain exact wire
  -> satisfy a matching pending Interest
  -> add source to current uncommitted group

flush()
  -> atomically detach current pending group
  -> generate generic repair packet(s) with exact source lengths
  -> publish authenticated group metadata/frontier

stop()
  -> fence subsequent push/flush and network publication
```

`start()` and `stop()` are idempotent only in their already-active and
already-stopped states respectively. Every mutation is serialized per
publisher. Concurrent `push()` and `flush()` have one linearization order:
each accepted source belongs to exactly one group. `flush()` atomically
detaches the currently pending sources; a failure before frontier replacement
publishes no partial commit and fails the session. An empty `flush()` is a
side-effect-free no-op.

The Consumer keeps bounded in-flight, retry, reorder, group, and duplicate
state. It validates Data asynchronously and never calls the App while holding
its internal mutex. Recovery order is fixed:

```text
validated source Data
  -> validated repair/group commit
  -> bounded source retry
  -> explicit terminal gap
```

`stop()` invalidates callback generations and cancels pending Interests; late
Data, Nack, timeout, or validator callbacks cannot admit an item or schedule
new work after stop.

Errors are classified consistently in C++ exceptions/status reasons and
Python exceptions:

| Condition | Required result |
|---|---|
| null/malformed/unsigned source | reject; session remains active |
| wrong prefix/version/sequence or stale epoch | reject; session remains active |
| signed wire over budget | reject; session remains active |
| byte-identical duplicate | duplicate counter; no second commit |
| same name, different wire | fail session as equivocation |
| non-monotonic/gapped source sequence | reject; session remains active |
| group/frontier signing or staging failure | fail session; no partial frontier advance |
| operation after stop | reject without network publication |

## Migration and Rollback

- All C++, Python, examples, tests, and UAV callers migrate in one change.
- No alias, overload, feature flag, or dual publication path preserves the old
  high-level API.
- Rollback uses a pinned previous binary/commit.
- Frozen old measurements remain immutable and identify their old binary hash.
