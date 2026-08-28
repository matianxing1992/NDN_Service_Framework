# Before/After Examples

The examples show equivalent lifecycle behavior. Bootstrap combines the first
announcement, publication, and activation because the existing descriptor
requires a materialized safe-join source. After bootstrap, `announce()` remains
explicit because prefetch needs a future name before `publish()`.

## C++ Before - Existing Low-Level API

```cpp
nsf::LiveStreamDefinition definition;
definition.contractVersion = nsf::STREAM_NAME_MAP_CONTRACT_VERSION_V2;
definition.streamId = "sensor";
definition.provider = providerName;
definition.semanticDataPrefix =
  ndn::Name("/example/provider/sensor").appendVersion(sessionEpoch);
definition.sessionEpoch = sessionEpoch;
definition.mappingVersion = sessionEpoch;
definition.samplePeriodMs = 40.0;
definition.sampleClasses = {
  nsf::SampleClassProfile::bounded("block", 2, 4, 32, 1),
};
definition.fec = nsf::LiveStreamFecOptions::gf256TwoRepair(4, 4096, 500);

auto publisher = provider.createLiveStream(definition);
auto reservation = publisher->announceSample(
  0, "block", [&] (size_t index, nsf::LiveStreamItemKind kind) {
    return ndn::Name(definition.semanticDataPrefix)
      .append("sample").appendSequenceNumber(0)
      .append(kind == nsf::LiveStreamItemKind::Source ? "source" : "repair")
      .appendSegment(index);
  });
publisher->publishSample(reservation, opaqueItems);
auto descriptor = publisher->activate(
  {40.0, reservation.group.sources.front().cursor});
```

## C++ After - Frozen Facade

```cpp
nsf::StreamConfig config;
config.streamId = "sensor";
config.dataPrefix = "/example/provider/sensor";
config.samplePeriodMs = 40.0;
config.sampleClasses = {
  nsf::SampleClassProfile::bounded("block", 2, 4, 32, 1),
};
config.fec = nsf::LiveStreamFecOptions::gf256TwoRepair(4, 4096, 500);

auto stream = provider.createStream(config);
auto descriptor = stream->start(0, "block", opaqueItems);

stream->announce(1, "block");
stream->publish(1, nextOpaqueItems);
```

## C++ Consumer Before

```cpp
nsf::LiveStreamOpenOptions options;
options.start = nsf::LiveStreamStart::Latest;
options.prefetchPolicy =
  nsf::LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
options.aggregateInterestLimit = 64;
options.enableFecRecovery = true;
options.onItem = onItem;

auto handle = user.openLiveStream(descriptor, std::move(options));
handle->start();
```

## C++ Consumer After

```cpp
nsf::StreamSubscriptionOptions options;
options.onItem = onItem;

auto handle = user.subscribeStream(descriptor, std::move(options));
```

## Python Before - Existing Low-Level API

```python
definition = LiveStreamDefinition(
    stream_id="sensor",
    provider=provider_id,
    semantic_data_prefix=f"/example/provider/sensor/v={session_epoch}",
    session_epoch=session_epoch,
    mapping_version=session_epoch,
    sample_period_ms=40.0,
    sample_classes=(SampleClassProfile("block", 2, 4, 32, 1),),
    fec=LiveStreamFecOptions.gf256_two_repair(4, 4096, 500),
)

publisher = provider.create_live_stream(definition)
reservation = publisher.announce_sample(0, "block", name_factory)
publisher.publish_sample(reservation, opaque_items)
descriptor = publisher.activate(
    measured_sample_period_ms=40.0,
    safe_join_cursor=reservation.group.sources[0].cursor,
)
```

## Python After - Frozen Facade

```python
config = StreamConfig(
    stream_id="sensor",
    data_prefix="/example/provider/sensor",
    sample_period_ms=40.0,
    sample_classes=(SampleClassProfile("block", 2, 4, 32, 1),),
    fec=LiveStreamFecOptions.gf256_two_repair(4, 4096, 500),
)

stream = provider.create_stream(config)
descriptor = stream.start(0, "block", opaque_items)

stream.announce(1, "block")
stream.publish(1, next_opaque_items)
```

## Python Consumer Before

```python
handle = user.open_live_stream(
    descriptor,
    start="latest",
    prefetch_policy="adaptive-sample-atomic",
    aggregate_interest_limit=64,
    enable_fec_recovery=True,
    on_item=on_item,
)
handle.start()
```

## Python Consumer After

```python
options = StreamSubscriptionOptions(on_item=on_item)
handle = user.subscribe_stream(descriptor, options)
```

## Continuous Operation

After bootstrap, the facade does not remove proactive announcement. A periodic
producer keeps a small application-known identity horizon:

```python
stream.announce(next_sample_id, classify(next_sample_id))
stream.publish(current_sample_id, current_opaque_items)
```

The application still owns sample IDs, classes, timing, and opaque content.
Core still owns Mapping, exact-name Interests, prefetch decisions, signing,
validation, FEC, retry, recovery, and status.
