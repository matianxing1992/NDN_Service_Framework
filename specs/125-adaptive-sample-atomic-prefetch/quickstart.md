# Quickstart: Adaptive Sample-Atomic Prefetch

## Provider

```cpp
LiveStreamDefinition definition;
definition.streamId = streamId;
definition.provider = providerName;
definition.semanticDataPrefix = semanticPrefix;
definition.samplePeriodMs = 1000.0 / fps;
definition.sampleClasses = {
  SampleClassProfile::bounded("key", keySeed, hardMaxItems),
  SampleClassProfile::bounded("delta", deltaSeed, hardMaxItems),
};

auto stream = provider.createLiveStream(definition);
stream->start();

auto reservation = stream->announceSample(
  frameId, expectedClass,
  [=] (size_t index, LiveStreamItemKind kind) {
    return makeSemanticFrameName(semanticPrefix, frameId, index, kind);
  });

// For name-bound AEAD, reconcile the real packetization first; this is an
// actual count, not an APP-selected prediction or window.
auto exactNames = stream->prepareSampleExtent(
  reservation, unprotectedFrameItems.size());
auto protectedFrameItems = appProtect(unprotectedFrameItems, exactNames);
stream->publishSample(reservation, protectedFrameItems);
```

Fixed FPS/GOP lets the APP announce future frame IDs/classes before encoding.
Fixed bitrate helps choose cold-start seeds. Core learns actual item counts and
owns Mapping lead, conservative prediction, FEC grouping, and packet windows.

## Consumer

```cpp
LiveStreamOpenOptions options;
options.prefetchPolicy = LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
options.enableFecRecovery = true;
options.onItem = onProtectedItem;

auto stream = user.openLiveStream(descriptor, options);
stream->start();
```

The APP decrypts/decodes accepted bytes in its callback. It does not calculate
key/delta packet windows. `status()` reports predictions, errors, complete-group
expansions/deferrals, and Interest evidence.
