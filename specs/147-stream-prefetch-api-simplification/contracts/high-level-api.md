# Frozen High-Level Stream/Prefetch API Contract

This document is the frozen contract implemented by Spec 147.

## C++17

```cpp
struct StreamAdvancedOptions
{
  size_t mappingBlockCapacity = 16;
  size_t mappingAheadBlocks = 4;
  size_t retainedItems = 600;
  size_t maxNameReservations = 65536;
  size_t maxPendingInterests = 256;
  size_t signedWireCap = ndn::MAX_NDN_PACKET_SIZE;
  uint64_t startupTimeoutMs = 1000;
};

struct StreamConfig
{
  std::string streamId;
  ndn::Name dataPrefix;
  double samplePeriodMs = 0.0;
  std::vector<SampleClassProfile> sampleClasses;
  LiveStreamFecOptions fec = LiveStreamFecOptions::none();
  std::optional<uint64_t> sessionEpoch;
  StreamAdvancedOptions advanced;
};

class StreamPublisher
{
public:
  LiveStreamDescriptor start(
    uint64_t initialSampleId,
    const std::string& initialSampleClass,
    const std::vector<std::vector<uint8_t>>& opaqueItems);
  void announce(uint64_t sampleId, const std::string& sampleClass);
  void publish(uint64_t sampleId,
               const std::vector<std::vector<uint8_t>>& opaqueItems);
  LiveStreamStatus status() const;
  void stop();
};

struct StreamSubscriptionOptions
{
  LiveStreamStart start = LiveStreamStart::Latest;
  std::optional<LiveStreamPrefetchPolicy> prefetchPolicy;
  size_t aggregateInterestLimit = 64;
  std::optional<bool> enableFecRecovery;
  uint64_t interestLifetimeMs = 500;
  std::function<LiveStreamItemAdmission(const VerifiedLiveStreamItem&)> onItem;
  std::function<void(const LiveStreamStatus&)> onStatus;
};

std::shared_ptr<StreamPublisher>
ServiceProvider::createStream(const StreamConfig& config);

std::shared_ptr<LiveStreamConsumerHandle>
ServiceUser::subscribeStream(const LiveStreamDescriptor& descriptor,
                             StreamSubscriptionOptions options);
```

## Python

```python
@dataclass(frozen=True)
class StreamAdvancedOptions:
    mapping_block_capacity: int = 16
    mapping_ahead_blocks: int = 4
    retained_items: int = 600
    max_name_reservations: int = 65536
    max_pending_interests: int = 256
    signed_wire_cap: int = 8800
    startup_timeout_ms: int = 1000

@dataclass(frozen=True)
class StreamConfig:
    stream_id: str
    data_prefix: str
    sample_period_ms: float
    sample_classes: tuple[SampleClassProfile, ...]
    fec: LiveStreamFecOptions = field(default_factory=LiveStreamFecOptions.none)
    session_epoch: int | None = None
    advanced: StreamAdvancedOptions = field(default_factory=StreamAdvancedOptions)

class StreamPublisher:
    def start(
        self, initial_sample_id: int, initial_sample_class: str,
        opaque_items: Iterable[bytes],
    ) -> LiveStreamDescriptor: ...
    def announce(self, sample_id: int, sample_class: str) -> None: ...
    def publish(self, sample_id: int, opaque_items: Iterable[bytes]) -> None: ...
    def status(self): ...
    def stop(self) -> None: ...

@dataclass(frozen=True)
class StreamSubscriptionOptions:
    on_item: Callable[[VerifiedLiveStreamItem],
                      bool | LiveStreamItemAdmission]
    start: str = "latest"
    prefetch_policy: str | None = None
    aggregate_interest_limit: int = 64
    enable_fec_recovery: bool | None = None
    interest_lifetime_ms: int = 500
    on_status: Callable[[LiveStreamStatus], None] | None = None

ServiceProvider.create_stream(config: StreamConfig) -> StreamPublisher

ServiceUser.subscribe_stream(
    descriptor: LiveStreamDescriptor,
    options: StreamSubscriptionOptions,
) -> LiveStreamConsumerHandle
```

## Semantic Parity

| Concept | C++ | Python |
|---|---|---|
| Create | `createStream(config)` | `create_stream(config)` |
| Bootstrap and activate | `start(id, class, items)` | `start(id, class, items)` |
| Announce later future | `announce(id, class)` | `announce(id, class)` |
| Publish actual extent | `publish(id, items)` | `publish(id, items)` |
| Subscribe and auto-start | `subscribeStream(desc, options)` | `subscribe_stream(desc, options)` |
| Status | `status()` | `status()` |
| Stop | `stop()` | `stop()` |

## Delegation Contract

```text
createStream
  -> derive LiveStreamDefinition
  -> existing createLiveStream

start
  -> bounded wait for existing route readiness off the Face thread
  -> generate canonical names
  -> existing announceSample
  -> existing publishSample
  -> derive LiveStreamReadiness
  -> existing activate

announce
  -> generate canonical names
  -> existing announceSample
  -> retain reservation by sample ID

publish
  -> lookup retained reservation
  -> existing publishSample

subscribeStream
  -> derive LiveStreamOpenOptions
  -> existing openLiveStream
  -> existing handle.start()
  -> return existing handle
```

No step calls or reimplements `StreamAdaptiveFetcherState`, Mapping admission,
FEC encoding/recovery, retry, timeout, Nack, validation, or packet production.

The derived `mappingVersion` equals the selected nonzero session epoch. This
preserves the existing Core invariant that the final Version component of the
semantic payload prefix equals `mappingVersion`, while adding exactly one
session Version component to `dataPrefix`.

The bounded readiness primitive exposes only whether both existing publisher
routes have registered or failed. It does not perform network fetching and
must never block the Face I/O thread.

## Canonical Default Names

The facade first appends a version component derived from the session epoch to
`dataPrefix`, producing `sessionPrefix`. It then generates:

```text
<sessionPrefix>/sample/<SequenceNumber=sampleId>/source/<Segment=index>
<sessionPrefix>/sample/<SequenceNumber=sampleId>/repair/<Segment=index>
```

This is the only facade naming policy. Custom names use the existing low-level
API.

## Error Categories

Both languages expose equivalent failure categories:

- invalid configuration;
- session collision or invalid injected epoch;
- unknown sample class;
- duplicate sample ID;
- invalid or empty bootstrap sample;
- bootstrap readiness timeout or Face-thread invocation;
- duplicate start;
- publish without announcement;
- duplicate publish;
- actual extent outside class/FEC bounds;
- operation after stop/failure;
- invalid descriptor/options/callback;
- underlying validation or publication failure.

C++ uses typed exceptions from the public library; Python maps them to
`ValueError`, `RuntimeError`, or the existing binding exception category.

Errors detectable from facade state or arguments are rejected before
delegation. If `announceSample`, `publishSample`, or `activate` throws after a
mutating operation may have begun, `StreamPublisher` enters `Failed`, rethrows
the original error, and rejects subsequent announce/publish/start operations.
The facade never retries, re-announces, or claims rollback of already committed
Mapping or payload state.

Session collision validation is process-local. Its key is Provider identity,
base data prefix, and session epoch. An automatically generated epoch retries a
live collision; an injected epoch must be nonzero and not currently live for
the same Provider/base prefix. No cross-process or persistent uniqueness is
claimed.
