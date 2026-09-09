# R10-B57 Native V3 Placement Lifetime Recheck

**Date**: 2026-09-09
**Batch**: R10-B57
**Source baseline**: `669f5f27aa3e0ed6804735f1674291df501fc349` plus the fixture-only change recorded in `tasks.md`
**Scope**: Keep the externally supplied `DummyClientFace` alive until a detached native requester operation releases its `ServiceUser`.

## Initial failure boundary

The pre-repair Spec182 unit run aborted with `RC=139` after `226/227` test cases. The isolated
`Spec182V3Placement/PublicClientCommitsSignedOfferAndIgnoresLateTerminalCallbacks` selector
alternated between a memory fault and a pass. The retained GDB trace identifies the first
boundary as `ServiceUser::~ServiceUser` → `ndn::Scheduler::~Scheduler` →
`boost::asio::detail::epoll_reactor::cancel_timer` while the caller was destroying its external
`DummyClientFace`. The detached `NativeInferenceClient` worker still owned the `ServiceUser`,
whose destructor could therefore race the Face scheduler/reactor teardown.

Raw boundaries retained in the workspace:

- `.codex-tmp/spec182-unit-rerun-final.log`
- `.codex-tmp/spec182-v3-placement-segv-r1.log`
- `.codex-tmp/spec182-v3-placement-segv-r2.log`
- `.codex-tmp/spec182-v3-placement-segv-r3.log`
- `.codex-tmp/spec182-v3-placement-gdb.log`

## Change and review

The C++ fixture now allocates `DummyClientFace` in a `shared_ptr` and stores that owner in the
nested `User` (`tests/unit-tests/di-native-v3-placement.t.cpp:541-547,631-640`). All existing
Face calls use the same heap object. Production `ServiceUser`, `NativeInferenceClient`, callback,
executor, and `close()` semantics were not changed.

Review trace:

- Skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- Official skill SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Baseline/diff: `669f5f27aa3e0ed6804735f1674291df501fc349`; fixture-only diff above
- Static result: No P1/P2/P3 finding after checking declaration order, ownership transfer,
  every `face` call, selector registration, and absence of production ownership changes.

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | covered | `runPublicClientScenario`; `NativeInferenceClient` construction at `di-native-v3-placement.t.cpp:735`; selector at `:1036` | `rg -n "PublicClientCommits|runPublicClientScenario|NativeInferenceClient" tests/unit-tests/di-native-v3-placement.t.cpp` | Existing native requester path is unchanged; the fixture now owns its external Face for the full operation lifetime |
| `implementation and wire` | covered | `test::LocalServiceUser`; `DummyClientFace`; `User::faceOwner` | `sed -n '541,640p' tests/unit-tests/di-native-v3-placement.t.cpp`; `rg -n "face\.|getIoContext|attachLocalMockPubSub" tests/unit-tests/di-native-v3-placement.t.cpp` | Ownership-only test change; no protocol or production wire change |
| `test/harness/oracle` | covered | `Spec182V3Placement/PublicClientCommitsSignedOfferAndIgnoresLateTerminalCallbacks`; three-scenario fixture loop | `tests/unit-tests/di-native-v3-placement.t.cpp`; isolated selector repeated 10 times; full `Spec182*` selector | All 10 isolated runs passed with 42/42 assertions; full Spec182 unit run passed 249/249 cases and 6781/6781 assertions |
| `build/source closure` | covered | `tests/wscript` `unit-tests` `ant_glob('unit-tests/**/*.cpp')`; `build-nac182/unit-tests`; `integration-tests` target | `rg -n "unit-tests.*ant_glob|integration-tests" tests/wscript`; Waf builds with `WAFLOCK=.lock-waf`; `sha256sum` on outputs | Canonical unit build and integration build completed from the same Waf tree; output hashes recorded below |
| `migration/evidence` | N/A | No compatibility or caller migration change in this fixture repair | `git diff -- tests/unit-tests/di-native-v3-placement.t.cpp` | No migration path changed; prior failure logs remain linked and no T016 status is promoted |

## Build and runtime results

Canonical build environment used the repository system-first toolchain (`/usr/bin/g++ -B/usr/bin`,
`/usr/bin/cmake`, `WAFLOCK=.lock-waf`, system-first `PATH`). The earlier no-lock invocation wrote
to the old `.codex-tmp/spec182-r4-b2/build` tree and is retained as a workflow boundary; it was
not used for the verdict.

| Target | Command | Result |
| --- | --- | --- |
| `unit-tests` | `PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin WAFLOCK=.lock-waf ./waf -o build-nac182 build --targets=unit-tests -j4` | exit `0`, Waf elapsed `262.797s`; log `.codex-tmp/spec182-r10-b57-unit-build-20260909-1742/build-canonical.log`; output SHA-256 `7e4a880533ab95e649b9df72fc6491c1afb018f8a6d2d8755854cf58a39699f2` |
| `integration-tests` | `PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin WAFLOCK=.lock-waf ./waf -o build-nac182 build --targets=integration-tests -j4` | exit `0`, Waf elapsed `87.287s`; log `.codex-tmp/spec182-r10-b57-integration-build-20260909-1830/build.log`; output SHA-256 `b212c6c45c90b7b1c55d484dec442a64f7545e78687f666b9884912cacf97edd` |
| placement selector | `env LD_LIBRARY_PATH=$PWD/build-nac182 build-nac182/unit-tests --run_test=Spec182V3Placement/PublicClientCommitsSignedOfferAndIgnoresLateTerminalCallbacks --report_level=short` repeated 10 times | all exit `0`, each `1` case / `42` assertions; logs `.codex-tmp/spec182-r10-b57-unit-reruns-20260909-1748/r{1..10}.log` |
| Spec182 unit suite | `timeout 240s env LD_LIBRARY_PATH=$PWD/build-nac182 ./build-nac182/unit-tests --run_test='Spec182*' --report_level=short` | exit `0`, `249/249` cases and `6781/6781` assertions; log `.codex-tmp/spec182-r10-b57-unit-reruns-20260909-1748/spec182-full.log` |
| existing integration flow | `timeout 240s env LD_LIBRARY_PATH=$PWD/build-nac182 ./build-nac182/integration-tests --run_test=Spec170NdnsfDiCoreFlow --log_level=test_suite --report_level=short` | exit `0`, `55/55` cases and `978/978` assertions; Waf test elapsed `127.696s`; log `.codex-tmp/spec182-r10-b57-integration-build-20260909-1830/spec170-core-flow.log` |

## Batch retrospective

- `static`: The original static pass did not make detached Face lifetime explicit. The repair
  review now checks fixture ownership, declaration order, every external Face call, and callback
  lifetime before allowing the selector to run.
- `compile/link`: The canonical Waf tree compiled and linked cleanly. The no-lock invocation
  writing to the stale `.codex-tmp/spec182-r4-b2` tree is retained as a tooling/source-identity
  miss; `WAFLOCK=.lock-waf` is the changed build gate for this retry.
- `runtime/test`: The first boundary was the destructor race and SIGSEGV. Ten isolated reruns,
  the full Spec182 unit suite, and the existing integration flow now pass. The test does not prove
  cross-process ownership or T016 qualification.
- `unobserved`: Detached executor behavior outside this fixture, cross-process Face ownership,
  maintained caller migration, no-Python execution, and MiniNDN qualification remain unobserved.

## Closure decision

`CLOSED_FOR_VALIDATION` for the local V3 placement fixture lifetime boundary. The stable exit is
the repeated selector plus the Spec182 unit and existing integration suites under the same
`build-nac182` source/build identity. `T010-B`, `T011-C`, T016, cross-process transport, and
maintained caller/no-Python obligations remain open; this record does not promote any of them to
qualification.
