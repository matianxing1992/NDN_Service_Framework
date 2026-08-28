# T006 Preflight and Formal Freeze Gate

**Date**: 2026-07-24  
**Verdict**: PASS  
**Formal cells started at this gate**: 0

## Build and Deterministic Gates

| Gate | Result |
|---|---|
| `./waf build -j2` | PASS; final no-op confirmation 5.726 s |
| forced Python binding rebuild, `--force -j2` | PASS |
| native full unit suite | 375/375 PASS |
| first-future regression stress | 50/50 PASS |
| Python Core streaming | 19/19 PASS |
| Python generality | 27/27 PASS |
| Python Spec 144 metric/neutrality | 8/8 PASS |
| Python Spec 144 runner | 8/8 PASS |
| UAV stream security contract | PASS; 127 native cases |
| strict Spec Kit structure | PASS; 22 FR, 12 SC, 9 tasks |
| renewed pre-implementation audit | PASS |
| Core/binding prohibited selector scan | zero violations |

The one full-suite failure observed before freeze was diagnosed, not ignored.
The test had treated `DummyClientFace.sentInterests` as proof that the provider
I/O context had already admitted the Interest. The corrected test waits for
provider-side `providerFutureInterests`; its 50-run feedback loop had zero
failures. Production logic and formal thresholds were unchanged.

## Final Post-Change MiniNDN Preflights

These paths are diagnostic and excluded from every formal denominator:

| Metric | Telemetry | Acoustic/audio |
|---|---:|---:|
| path | `results/spec144-preflight-telemetry-freeze-20260724` | `results/spec144-preflight-acoustic-freeze-20260724` |
| result | PASS | PASS |
| attempted / produced / delivered | 1200 / 1200 / 1200 | 1500 / 1500 / 1500 blocks |
| delivery | 100% | 100% |
| mean latency/AoI | 7.074 ms | 19.345 ms |
| p50 | 4.111 ms | 18.302 ms |
| p95 | 16.409 ms | 29.668 ms |
| p99 | 20.137 ms | 38.771 ms |
| max | 25.234 ms | 53.617 ms |
| longest gap | 76.988 ms | 80.750 ms |
| future hit | 1275/1275, 100% | 6690/6690, 100% |
| Mapping novelty | 1300/1300, 100% | 1625/1625, 100% |
| Mapping / Payload Interests | 1307 / 1300 | 1632 / 8124 |
| source / repair Payload | 1300 / 0 | 4874 / 3250 |
| retry / timeout / Nack | 2 / 4 / 0 | 2 / 3 / 0 |
| application useful | 1300 | 4874 |
| protection only | 0 | 3250 |
| nonproductive / unresolved | 0 / 0 | 0 / 0 |

Every per-cell gate, application-count check, Payload-kind equation, terminal
utility equation, clock-domain check, and cleanup check passed. Acoustic repair
Data fetched under zero loss is explicitly protection-only, not useful
application delivery.

## Frozen Subject

The matrix runner rechecks these 23 SHA-256 identities before every formal
cell:

| Subject | SHA-256 |
|---|---|
| Core `Stream.cpp` | `341def86cb426dba000459cf582a61597a7e2d82f412099429103edbbe7e4f52` |
| Core `Stream.hpp` | `1830a8c79ea1982dd41c259121636f9156439424a937ba9fb68ec3eda1d29c3f` |
| TimelineTrace.cpp | `7463209d47b209abb7563b9157ce33b7ab414719f97137a558e717b549ad4bf2` |
| TimelineTrace.hpp | `c320167ef449add0fa653c803453496b22aa54c53a59abe934571371b03ad3f1` |
| native binding source | `adcc05a8d5a0263c296f1f423a82e23a6fbf2e6e67bff1602b24005931f517f0` |
| Python wrapper | `bf824053fe7b4175c45e16207c99517c45fe8b352d411e94a1c13d562de29f15` |
| built Python binding | `09f429a482750389ea4549718652bbcf2139c8b71b77e7ed214323d82dcafad4` |
| UAV helper cpp | `14dc51725c1b96061de595359201f41b87f7a7cf385b2a6a022c1c7bc77c435e` |
| UAV helper hpp | `ecb718a403b241e9c3c9e1b9e90283f81f9f34f8fe970b088634552b13c425ba` |
| UAV node source | `041452f6364b2603d4e993fbcc45408e37bfc374f3609027be31583fa10cabbf` |
| built UAV node | `9a255e6ba9b32567aff78a8a5566c400269c8b963174e9e03b5f32fd6f65c9af` |
| built NDNSF library | `b82e398aaace00137a761eb1fc74f1f324dbf9d86433c05a9312c4c3ed50e447` |
| built Controller | `d841d7fd3f7ce30da15109d9e325ba5c9400721539842ce38fa53cec855387b3` |
| cell runner | `01dd1808f251cb293bf0598cb526cd09dd1f7a75e227371d2c6941d1888a0b0c` |
| matrix runner | `9bbddd48dc67e12117bc6ff6d44f350c8ec73bdaf938d4e667efffa561f7b7b8` |
| analyzer | `e29cf5364e9928a495f4ce2643312ca4a932da9ba1acef14d9f1191fb5efa776` |
| UAV policy | `ea1ebb814dfa147b16dcb4b1b0878908a14a0ee93838de817201a8fd80787745` |
| trust-any | `78173dfb1d0636dff3bd1c20e273d4ae401fd1d5062b2c529c88755b0e8245bb` |
| trust schema | `ed0fcc780756ba7ed4171530c74df2a74fc60eb740b2c5fed4e402ee5d61614c` |
| frozen spec/thresholds | `aca4e81cc117dc9372944007434ac1203cae93f7c718fd6c86616e508097f852` |
| experiment plan | `8a2338e274d571c5c442ce12da572cd4503db71803b1ee5af7c6b40cbc6f72d5` |
| workload contract | `b4e00807a69fe4cb604313447c4e44a14f2f53d1a49ea88b3e90da9fb92a9188` |
| evidence contract | `fcd1376f7877d93c419141069fb97f8323b17a816997c17f70f172410fc729d0` |

Environment recorded in the formal manifest:

- git HEAD `751384c50cb424bfaa39aefefee207b37afb4306`;
- GCC 9.4.0; Boost 1.71; ndn-cxx 0.9.0;
- NFD/nfdc `24.07-14-g2b43d675`;
- MiniNDN 0.7.0;
- Python 3.8.10;
- Linux `5.15.0-139-generic`;
- `tc` iproute2 `ss200127`.

## Historical and Ownership Gate

Spec 127/128 spec/result roots and Spec 145 promoted result hashes still match
T001 exactly. No historical runner was invoked. No MiniNDN, UAV stream node,
Controller, or competing Spec 144 matrix process remained after preflight.
The formal runner has one global lock across output roots and one immutable
invocation receipt per cell.

**Decision**: T006 PASS. One fresh 32-cell formal campaign may now start.
