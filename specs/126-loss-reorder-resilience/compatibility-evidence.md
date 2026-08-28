# Spec 126 Compatibility Evidence

## Build and bindings

- Full source build: `./waf build -j$(nproc)` completed 346/346.
- The Python extension was force-rebuilt after the native status additions and
  relinked by the final full build.
- Native/Python status parity covers `payloadInterests`, `mappingInterests`,
  `mappingDataResponses`, `mappingNewDataResponses`, `retryAttempts`,
  `timeouts`, and `nacks`.

## Deterministic and security gates

- `./build/unit-tests --run_test=Stream,UavProtocolState --log_level=message`:
  112/112 passed.
- Focused Python Core/UAV/latency/runner suites: 50/50 passed (19 + 13 + 9 + 9).
- `python3 tests/run_uav_stream_security_contract.py`: 12/12 checks passed.

These gates cover Mapping v2 golden behavior, exact-name retry, byte-exact
single-source recovery, multiple-source fail-closed behavior, reordered
Mapping/source/repair admission, bounded mapped-live horizon, scheduling-budget
restoration, stop fencing, analyzer parsing, and C++/Python field parity.

## Frozen evidence preservation

The accepted confirmation stores both before/after manifests:

- `results/spec126-loss-reorder-20260720-confirmation07/source-hashes-before.json`
- `results/spec126-loss-reorder-20260720-confirmation07/source-hashes-after.json`
- `results/spec126-loss-reorder-20260720-confirmation07/spec125-hashes-before.json`
- `results/spec126-loss-reorder-20260720-confirmation07/spec125-hashes-after.json`

The campaign summary records `sourceUnchanged=true` and
`spec125EvidenceUnchanged=true`. Mapping v2, public sample APIs, semantic Data
names, and historical Spec 125 result directories were not revised.
