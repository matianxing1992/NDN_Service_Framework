# Implementation Plan: Stream/Prefetch API Simplification

**Spec**: [spec.md](spec.md)  
**Date**: 2026-07-25  
**State**: Implemented and verified

## Summary

Add one thin, additive C++/Python facade over the validated live-stream engine.
Provider callers configure common fields, bootstrap and activate from one real
sample, then explicitly announce future samples and publish opaque items by
sample ID. Consumer callers subscribe and start the existing handle in one
operation. Low-level APIs remain unchanged.

The exact target signatures are frozen in
[contracts/high-level-api.md](contracts/high-level-api.md), with paste-ready
comparisons in [contracts/before-after.md](contracts/before-after.md).

## Technical Context

- C++17, ndn-cxx, existing `LiveStreamPublisher` and
  `LiveStreamConsumerHandle`;
- pybind11 native binding plus idiomatic Python wrappers;
- Boost.Test and Python `unittest`;
- no wire, algorithm, network topology, or formal experiment change.

## Current Code Reality

CodeGraph confirms:

- `ServiceProvider::createLiveStream` validates Provider identity, constructs
  the existing publisher, and starts its routes;
- `LiveStreamPublisher` owns `announceSample`, `prepareSampleExtent`,
  `publishSample`, `activate`, status, packet feed, and stop;
- existing `activate` requires both routes ready, at least one Mapping block,
  and a materialized safe-join source, so facade bootstrap must preserve that
  ordering;
- `ServiceUser::openLiveStream` constructs the existing consumer handle;
- Python `create_live_stream` wraps the native publisher;
- Python `open_live_stream` already provides a descriptor-aware v2 prefetch
  default but returns an unstarted handle.

No `createStream/create_stream` or
`subscribeStream/subscribe_stream` symbol currently exists.

## Architecture

### Additive ownership

```text
APP
  -> StreamPublisher facade
       -> existing LiveStreamPublisher
            -> existing Mapping/sign/publish/FEC implementation

APP
  -> subscribeStream facade
       -> existing openLiveStream
       -> existing LiveStreamConsumerHandle.start()
            -> existing prefetch/fetch/validate/recover implementation
```

The facade is orchestration and validation only. It must not call
`StreamAdaptiveFetcherState` or modify Core decisions.

Bootstrap performs:

```text
bounded route-ready wait on APP thread
  -> announce initial sample
  -> publish initial sample
  -> activate from its first materialized source
```

The wait uses a read-only condition from the existing publisher route
callbacks. Face-thread invocation fails immediately; it never blocks the event
loop needed to satisfy readiness.

### Provider facade state

`StreamPublisher` privately owns:

- one existing publisher;
- validated config and derived definition;
- a mutex-protected bounded map of outstanding sample reservations;
- a process-local live session-identity claim;
- bootstrap source cursor;
- lifecycle state.

No application callback runs under its mutex. Reservation bounds derive from
the existing `maxNameReservations` and sample-class/FEC maxima. Facade
validation errors occur before delegation; an exception from a mutating
underlying call moves the facade to `Failed` instead of attempting unsafe
rollback or retry.

### Symmetric defaults

The C++ and Python facade share one golden defaults table. Conversion tests
compare the derived native definition/open options field-by-field. Low-level
C++ and Python defaults remain unchanged.

### Compatibility

The existing APIs remain public. The facade uses distinct type and method
names; no overload changes existing resolution. Existing examples and tests
are compiled and run unmodified. Rollback removes only facade symbols,
bindings, wrappers, and new examples/tests.

## Changed-File Boundary for Future Implementation

Expected:

```text
ndn-service-framework/StreamFacade.hpp
ndn-service-framework/StreamFacade.cpp
ndn-service-framework/ServiceProvider.hpp
ndn-service-framework/ServiceProvider.cpp
ndn-service-framework/ServiceUser.hpp
ndn-service-framework/ServiceUser.cpp
pythonWrapper/src/ndnsf/_ndnsf.cpp
pythonWrapper/ndnsf/streaming.py
pythonWrapper/ndnsf/service.py
pythonWrapper/ndnsf/__init__.py
examples/StreamFacadeProvider.cpp
examples/StreamFacadeConsumer.cpp
examples/python/live_stream/facade_provider.py
examples/python/live_stream/facade_consumer.py
tests/unit-tests/stream-facade.t.cpp
tests/python/test_ndnsf_stream_facade.py
specs/147-stream-prefetch-api-simplification/**
```

Forbidden:

```text
StreamAdaptiveFetcherState decision/update implementation
Mapping wire/admission behavior
FEC encoder/recovery behavior
retry/timeout/Nack scheduling behavior
Specs 144/146 result roots
UAV/acoustic/audio/codec/workload branches
```

## Verification

1. compile-time C++ contract tests and Python signature/default tests;
2. golden derived-definition, naming, descriptor, and error parity;
3. delegate-spy tests proving each facade operation invokes the existing
   primitive exactly once;
4. existing Stream/validator/binding suites unchanged;
5. paired low-level/facade local integration examples comparing packets and
   callback/status results;
6. post-implementation CodeGraph, diff, neutrality, and frozen-hash audit.

No new MiniNDN performance matrix is planned because the accepted design must
produce identical network behavior. Any discovered network difference is a
BLOCK, not permission to tune prefetch.

## Constitution Check

- Canonical dynamic runtime: PASS; facade is additive over current generic API.
- Security data path: PASS; descriptor, signing, validation, and callbacks are
  reused unchanged.
- CodeGraph first: PASS; exact current symbols and callers were inspected.
- Spec-driven durable API change: PASS; spec, plan, contracts, tasks, and audit
  are required before implementation.
- Verification scope: PASS; golden delegate parity and full existing
  regressions match the syntax-only risk.
- Cohesive tasks: PASS; Provider, consumer, and compatibility outcomes are
  independently reviewable.
