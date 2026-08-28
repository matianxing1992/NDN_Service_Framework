# T001 Implementation Readiness

**Recorded**: 2026-07-24  
**Verdict**: PASS  
**Authority**: current on-disk worktree, not `HEAD`

## Single-Writer and Frozen-Evidence Gate

The process audit found no active Spec 125/126 runner, MiniNDN process, NFD,
UAV application, or historical result writer. The only matches were the audit
shell and its `rg` child.

Frozen directory-root hashes:

| Directory | Observed SHA-256 | Contract |
|---|---|---|
| `specs/125-adaptive-sample-atomic-prefetch` | `08fbc07481e4d2565e99dad9e8ea2259c811e1f1abe973fa55bfdfcf06f409c9` | MATCH |
| `specs/126-loss-reorder-resilience` | `766969e6e8dfffff86732ff84ab7567c15bc0cbe0541bf2b6d8648d7ed98c493` | MATCH |
| `results/spec125-adaptive-sample-atomic-20260719-confirm06` | `80dbfbd02f5f7aeaaeb5a6c7556c82c0429d5dc43109c9a72b613655b28c6c0d` | MATCH |
| `results/spec126-loss-reorder-20260720-confirmation07` | `aab1ff1c340f8acb1858fbf8ee3c44a274c0e32581a144e0786c50a297e8412a` | MATCH |

No historical runner was invoked.

## Current On-Disk Baseline

These hashes identify the user-owned baseline before Spec 145 implementation:

| Path | SHA-256 |
|---|---|
| `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp` | `1dc43de0d476b4054bdb845deaa162df1b662c568945d36217dee5573dbf163b` |
| `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp` | `3e2fb64b38b180b0d2ce5c248f4cfab7f7b4f96848666d0c3f0075236b4c3301` |
| `NDNSF-UAV-APP/shared/UavProtocol.hpp` | `b99f5c6dad9813b187e01a89a5b888d5bd885adcde50efa6691e66e66743af9b` |
| `NDNSF-UAV-APP/shared/UavProtocol.cpp` | `96f6fc982b23a8399734a3a1534ce16f1cbacb3473fce72c5b98ad1cb309114a` |
| `NDNSF-UAV-APP/shared/UavVideoPipeline.hpp` | `235bf0dbbad791c340159a3be0864405437ab33d3cd85882f06c1dafd6648fbf` |
| `NDNSF-UAV-APP/shared/UavVideoPipeline.cpp` | `8565113ff95245e2f1b21071b9d2d19c08a2f59f48fe3e827f930170caafedcd` |
| `tests/unit-tests/uav-protocol-state.t.cpp` | `17cd78471ccee3fa44e32affb2edcb655e7f8f5eb39852010164639495c71c8a` |
| `tests/wscript` | `2b8a0fb65ad35dda1612e2f61539eed1e5e67a0cc274bb489eb08563ecb8837f` |

`UavVideoPipeline.hpp/.cpp` are currently untracked user-owned files. They are
not disposable generated files.

Generic paths that Spec 145 must not change:

| Path | Baseline SHA-256 |
|---|---|
| `ndn-service-framework/Stream.hpp` | `8ab1a74a8c491317b59ca4d24700c8a94bc3156b33d2ac307a5f239f5076faa4` |
| `ndn-service-framework/Stream.cpp` | `ec9f8278606ca7c5105f259ca21a484e481b08d2f42571a4623b2dc386500488` |
| `ndn-service-framework/ServiceUser.cpp` | `423cae2ef2e8d89b6b2559e64772393d9b4ebbfe40032f38780b3b33b9809e52` |
| `ndn-service-framework/ServiceProvider.cpp` | `5ff07e432ac464c2dfbbf903263724c349182e712a3950558374fd8d174fa00d` |
| `pythonWrapper/src/ndnsf/_ndnsf.cpp` | `00ca6ccba6cf391971aa8fd5dc5a57e3eeec40fada64d80968a96bf7e0361aae` |

## CodeGraph Owner and Blast-Radius Gate

CodeGraph status was up to date: 2,637 indexed files, 57,544 nodes, and
187,368 edges.

Allowed implementation regions:

- `VideoPublisher` class scheduling, future announcement, publication
  validation, and GStreamer/legacy capture setup;
- `GStreamerVideoPipeline::Impl` C callback adapter and first-failure state;
- `VideoAdaptiveState` additive fields and their `fromFields`, `toFields`,
  summaries, and focused tests;
- Ground Station active live-stream status callback, generation-fenced Core
  decision cache, stop/reset, and `currentVideoAdaptiveState`;
- focused UAV tests, build registration, new Spec 145 analyzer/runner, and
  Spec 145 evidence.

Explicitly forbidden regions:

- generic Stream fetch/publish logic;
- ServiceUser/ServiceProvider;
- Python bindings;
- Mapping, FEC, retry, timeout, or Nack algorithms;
- unrelated existing UAV, DI, test, and experiment hunks;
- Spec 125/126 files or results;
- Spec 144 until T006 PASS.

## Readiness Conclusion

No generic defect or architecture expansion is required. T002 may start with
test-first changes inside the allowed regions. Any need to cross a forbidden
region returns this gate to BLOCK.

