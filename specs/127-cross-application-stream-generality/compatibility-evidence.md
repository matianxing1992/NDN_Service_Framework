# Spec 127 Compatibility and Generic-Ownership Evidence

**Gate:** T005 PASS  
**Verified:** 2026-07-20  
**Live Spec 127 cells executed:** none at this gate

## Ownership result

Spec 127 adds only application-neutral workload fixtures, a MiniNDN cell
launcher, a one-shot campaign/analyzer, deterministic tests, and feature
documents. It adds no public mode, wire field, semantic-name version, policy
route, Core threshold, UAV path, codec parser, or payload-content input.

The fixtures call the existing Mapping v2 surface: `announce_sample`,
`prepare_sample_extent`, `publish_sample`, `open_live_stream`, and
`observe_accepted_sample`. The workload identifier selects only a frozen opaque
byte sequence and application-side acceptance threshold; it is never passed to
Core. End-of-window incomplete-sample terminalization is owned by the generic
application receipt tracker and does not change network retry, recovery, or
scheduling behavior.

## Accepted-source identity

These SHA-256 values exactly equal
`results/spec126-loss-reorder-20260720-confirmation07/source-hashes-after.json`:

| Accepted surface | SHA-256 |
|---|---|
| `ndn-service-framework/Stream.hpp` | `b55d32e8f8d63612a3e728e3bc37b176b59ddf2ee8e8078218ca069ec78ca01c` |
| `ndn-service-framework/Stream.cpp` | `4ac97b25d6d69c6723c890954be300ad3cc4c819072d40f197810f0da48333f2` |
| `pythonWrapper/src/ndnsf/_ndnsf.cpp` | `30ea806747caf429127443b7938aebfec79e849f7503052e2b4de23c212b0d2f` |
| `pythonWrapper/ndnsf/streaming.py` | `9ad7a91aa86cd8b998d0a66d842a6d801ec305722f2888334c7594d6e04574a2` |
| `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp` | `3e2fb64b38b180b0d2ce5c248f4cfab7f7b4f96848666d0c3f0075236b4c3301` |
| `NDNSF-UAV-APP/shared/UavProtocol.cpp` | `96f6fc982b23a8399734a3a1534ce16f1cbacb3473fce72c5b98ad1cb309114a` |
| `NDNSF-UAV-APP/shared/UavProtocol.hpp` | `b99f5c6dad9813b187e01a89a5b888d5bd885adcde50efa6691e66e66743af9b` |

`test_accepted_core_and_binding_sources_match_spec126_confirmation` makes the
four Core/binding identities a deterministic pre-campaign gate. The campaign
recursively hashes every retained Spec 125 and Spec 126 evidence file before
and after all cells and rejects any change.

## Build and deterministic regression evidence

| Command | Result |
|---|---|
| `./waf build -j$(nproc)` | PASS |
| `cd pythonWrapper && python3 setup.py build_ext --inplace --force` | PASS; extension rebuilt and copied in place |
| `./build/unit-tests --log_level=message` | PASS, 338/338 |
| `PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_core_streaming.py` | PASS, 19/19 |
| `PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_live_stream_generality.py` | PASS, 27/27 |
| `PYTHONPATH=pythonWrapper python3 tests/python/test_spec127_cross_application_runner.py` | PASS, 6/6 |
| `python3 tests/run_uav_stream_security_contract.py` | PASS, 12/12 contract checks and focused native 112/112 |

The complete C++ run reports two expected local-mock skips for unavailable
NAC-ABE large-data production; Boost reports `*** No errors detected`.

## Evidence boundary

This gate proves implementation neutrality and deterministic readiness only.
It does not claim live latency, continuity, future-hit utility, loss/reordering
reliability, or cross-application generality. Those claims are controlled
solely by the fresh 12-cell T006 campaign, including retained failures.
