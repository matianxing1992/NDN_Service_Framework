# Spec 148 High-Level API Removal Inventory

**Generated**: 2026-07-25  
**Scope**: Public `StreamPublisher` facade and descriptor-based high-level
subscription. Internal `LiveStreamPublisher` reservation/Mapping/FEC methods
are explicitly excluded.

## Provider Surface

| Location | Old surface/current state | Required migration |
|---|---|---|
| `ndn-service-framework/StreamFacade.hpp` | old `start(initial...)`, `announce`, `publish` plus predictive overload | retain only predictive `start/push/flush/status/stop`; return `PredictiveStreamDescriptor` |
| `ndn-service-framework/StreamFacade.cpp` | old bootstrap/announce/publish implementations and dual-mode state | remove old implementations/state; make predictive lifecycle unconditional |
| `pythonWrapper/src/ndnsf/_ndnsf.cpp` | two `start` overloads plus `announce/publish` bindings | bind only zero-argument `start`, `push`, `flush`, `status`, `stop` |
| `pythonWrapper/ndnsf/streaming.py` | old methods plus `start_predictive` | expose only `start/push/flush/status/stop`; `start()` wraps `PredictiveStreamDescriptor` |

## Consumer Surface

| Location | Old surface/current state | Required migration |
|---|---|---|
| `ndn-service-framework/ServiceUser.hpp/.cpp` | `subscribeStream(LiveStreamDescriptor, ...)` returns `LiveStreamConsumerHandle` | replace high-level overload with `PredictiveStreamDescriptor` returning `PredictiveStreamSubscriber` |
| `pythonWrapper/src/ndnsf/_ndnsf.cpp` | native `subscribe_stream` accepts old descriptor | bind predictive descriptor/subscriber |
| `pythonWrapper/ndnsf/service.py` | type-checks `LiveStreamDescriptor`, wraps old handle | require `PredictiveStreamDescriptor`, wrap predictive subscriber |

## C++ Callers

| Location | Old call | Migration |
|---|---|---|
| `tests/unit-tests/stream-facade.t.cpp` | old start/announce/publish and old consumer facade tests | replace with predictive contract and removal assertions |
| `examples/StreamFacadeProvider.cpp` | old start/announce/publish loop | App constructs/signs sequential Data, then start/push/flush |
| `examples/StreamFacadeConsumer.cpp` | old descriptor/handle | predictive descriptor/subscriber |
| `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp` | partial predictive push/flush mixed with low-level Mapping publication | one predictive provider path only |
| `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp` | old consumer path plus an incompletely wired predictive member | construct/start/stop only the predictive subscriber |

## Python Callers

| Location | Old call | Migration |
|---|---|---|
| `tests/python/test_ndnsf_stream_facade.py` | old descriptor/handle subscription contract | predictive descriptor/subscriber and removed-attribute assertions |
| `examples/python/live_stream/predictive_provider.py` | documents `start_predictive()` | call `start()` |
| `examples/python/live_stream/facade_consumer.py` | old descriptor subscription | predictive descriptor subscription |

`examples/python/live_stream/provider.py` and
`examples/python/live_stream/workload_provider.py` use the explicitly internal
`LiveStreamPublisher` API and are not public-facade removal targets unless later
call-graph analysis proves they are presented as high-level examples.

## Experiment/Runtime Callers

| Location | Current state | Migration |
|---|---|---|
| `Experiments/NDNSF_UAV_GUI_Minindn.py` | exposes `--discovery-mode`; sets `NDNSF_UAV_DISCOVERY_MODE`, but current UAV runtime does not prove consumption | new binary runs Predictive only; add real endpoint markers and acceptance mode |

## Negative Gates

After migration:

- C++ SFINAE/static assertions prove old `StreamPublisher` signatures absent.
- Python asserts `StreamPublisher` has no `announce`, `publish`, or
  `start_predictive`.
- Exact source scan permits `announceSample`/`publishSample` and
  `LiveStreamPublisher.publish`, but rejects the removed facade definitions,
  bindings, and calls.
- Full native/Python/UAV builds prove no unlisted caller remains.
