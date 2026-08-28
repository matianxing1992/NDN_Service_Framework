# Traceability: Unified Named UAV Video

**Covered requirements**: FR-001, FR-002, FR-003, FR-004, FR-005, FR-006, FR-007, FR-008, FR-009, FR-010, FR-011, FR-012, FR-013, FR-014, FR-015, FR-016, FR-017, FR-018, FR-019, FR-020, FR-021, FR-022, FR-023, FR-024, FR-025, FR-026, FR-027, FR-028, FR-029, FR-030, FR-031.

## Intent To Evidence

| Source intent | Story | Requirements/invariants | Design authority | Tasks | Acceptance evidence | Outcomes |
|---|---|---|---|---|---|---|
| One live H.264 stream should serve viewing and recording instead of creating two data formats | US1 | FR-001–FR-006, FR-012, FR-014; SI-001, SI-002 | `plan.md` data flow; contract §§1–3 | T001–T003 | C++/Python exact-wire fixtures; live-only/recording-only/combined MiniNDN | SC-001, SC-004, SC-008 |
| Recording should retain the same object with different lifecycle and permissions | US1, US3 | FR-005, FR-007, FR-009–FR-017; SI-003, SI-005 | contract §§3–5, 8 | T004, T006 | manifest/grant negatives; partial commit, queue/storage failure, restart tests | SC-002–SC-005 |
| Historical playback must not use a second fetch/decrypt/decoder protocol | US2 | FR-006–FR-013, FR-018–FR-021; SI-001, SI-003, SI-004 | contract §6 and removal inventory | T005, T006 | restart replay; name/wire/decoded digest equality; static removal scan | SC-002, SC-003, SC-006 |
| Mapping prefetch may be one-way only when names lead payload production sufficiently | US1 | FR-023; SI-001 | `plan.md` Prefetch Latency Contract; research Decisions 2–3; contract §7 | T001, T002, T007 | Provider Interest-before-production events and matched MiniNDN pairs | SC-009 |
| Data-in-Data may be considered only if necessary and after security review | US1 | FR-024 | research Decision 2; contract §7 | T001, T007 conditional amendment gate | inner/outer security, wire-cap, cache/amplification and matched latency evidence if activated | SC-010 |
| Late recording must not miss Mapping or begin at an undecodable H.264 boundary | US1, US3 | FR-025 | research Decision 6; contract §§2–3 | T002, T003, T006, T007 | atomic snapshot/feed handoff and late safe-join restart replay | SC-011 |
| Durable replay must validate the unchanged original signature after certificate rotation | US1, US2 | FR-026 | research Decision 10; contract §§4, 6 | T004, T005, T007 | archived chain/time negatives and rotation replay | SC-012 |
| Optimization must be based on truthful stage attribution rather than a misleading aggregate | All | FR-027–FR-030 | `plan.md` Performance Attribution Contract; research Decision 11; contract §9 | T001–T003, T005, T007 | deterministic timeline negatives; sampled `NDN_LOG` stage distributions; tracing-off/on and candidate matched pairs | SC-013–SC-016 |
| Network evidence must be fresh, bounded, and honest | All | FR-021, FR-022 | `quickstart.md` §§3–6 | T007 | unique candidate result directories and 60-second terminal artifacts | SC-007, SC-009, SC-010 |
| An operator must actually see decoded video, not merely a backend count | US1, US3 | FR-031 | `quickstart.md` visible GTK presentation gate | T007 | real GTK MiniNDN smoke, GUI-owned counter, render-gate log and screenshot | SC-017 |

## Requirement Coverage

| Requirement range | Implementing tasks | Primary gate |
|---|---|---|
| FR-001–FR-006 | T001–T003, T005 | exact live/stored signed-wire equality and one encode/encryption |
| FR-007–FR-013 | T001, T004, T005 | manifest/checkpoint/key-grant security and restart replay |
| FR-014–FR-017 | T002–T004, T006 | nonblocking bounded retention and explicit lifecycle/gaps |
| FR-018–FR-020 | T003, T005, T006 | canonical-only writes/replay and duplicate-path removal |
| FR-021–FR-022 | T001–T007 | deterministic suite plus frozen MiniNDN acceptance |
| FR-023–FR-024 | T001, T002, T007 | names-only lead gate; conditional versioned inline amendment |
| FR-025 | T002, T003, T006, T007 | atomic late attachment and decoder-safe replay start |
| FR-026 | T004, T005, T007 | original signer trust evidence across restart/rotation |
| FR-027–FR-030 | T001–T003, T005, T007 | truthful correlated stages, clock boundary, sampled NDN_LOG overhead, matched optimization attribution |
| FR-031 | T007 | visible GTK image, GUI-owned decoded count, render-gate log and screenshot |

No task lacks a source requirement. No functional requirement lacks an implementation and acceptance task.
