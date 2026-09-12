# Spec185 B2E — Extension Registration and Cooperative Control

**Date**: 2026-09-12  
**Task**: T016  
**Base**: `76e656c4` (B1 checkpoint)  
**Batch**: B2E  
**Status**: PASS for the bounded B2E exit

## Scope and design binding

T016 implements C-08 `CD08`, `CD09`, `F16`, `FN08`, `FLOW03`, and `PO08`:

- `ExtensionControl` carries a request-scoped deadline and cancellation callback;
  Qwen, YOLO, and placement loops check it at their inner scan boundaries.
- Cooperative splitter and placement ports carry exact `NativeStrategyIdentity`
  values. The planner checks identity, ACK closure, and cancellation before
  selection, artifact preparation, and sealing/publication.
- Adapter, placement, and runner registries reject implicit duplicate writes,
  permit explicit replacement only before `freeze()`, and provide read-only
  concurrent lookup after freeze. Runner creation returns independent instances.
- The old `NativeModelSplitStrategy`/`NativePlacementStrategy` API remains an
  advanced compatibility path; the cooperative planner does not silently accept
  a legacy placement vtable.

## Review trace

The official read-only skill was `/home/tianxing/.codex/skills/review-agent/SKILL.md`
(SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`).
The reviewer received complete frozen copies of all 18 changed paths, including
untracked fixtures and the installed consumer:

- v9: `.codex-tmp/spec185-t016-review-v9` — `STATIC_PASS`;
  `B2E_COMPOSITION_PASS`.
- v10b: `.codex-tmp/spec185-t016-review-v10b` — `STATIC_PASS` after wrapping
  the two `BOOST_CHECK` type expressions; diff SHA-256
  `7df53a5dc861bbee28052e32b01930c4138847c29c185dbf893ae4a89fa88828`.
- v11: `.codex-tmp/spec185-t016-review-v11` — `STATIC_PASS` after supplying
  the explicit two-role Qwen tensor-degree vector.
- v12: `.codex-tmp/spec185-t016-review-v12` — `STATIC_PASS`; the reviewer
  confirmed the fixture graph digest helper matches the production canonical
  identity. The v9 composition pass remained valid.

The reviewed combination was the complete catalog → cooperative planner →
control fence → splitter → placement → ACK/identity validation → artifact/seal
fence → grant/projection → `FrozenSelection` path. All five review lanes were
covered:

| Lane | Actual coverage |
| --- | --- |
| production entry/callers | `NativeRequestCatalog`, `planNativeRequestCooperative`, `NativeStrategyPorts`, `NativeAdapterRegistry`, `NativePlacementStrategyRegistry`, and `RegistryNativeModelRunnerFactory` |
| implementation and wire | `NativeStrategyIdentity`, `ExtensionControl`, ACK-closed digest checks, cancellation/deadline fences, registry freeze/replace, and legacy/cooperative type boundary |
| test/harness/oracle | C++ `tests/unit-tests/di-extension-contract.t.cpp`, registered by `tests/wscript`; standalone `extension-header-consumer.cpp` and `run-spec185-extension-consumer.sh` |
| build/source closure | full DI source closure and `spec185-extension-registry` target in `tests/wscript`; normal and clang/TSan candidates built from the existing Waf trees |
| migration/evidence | `extensions.hpp` installed-prefix include boundary, pkg-config consumer, review snapshots, this record, and T016 in `tasks.md` |

## Build and runtime evidence

The first normal compile reached link and caught a test macro parsing error. The
second selector exposed an invalid default three-role tensor-degree fixture, and
the next selector exposed the expected graph identity check because the fixture
used an arbitrary digest. Each boundary was recorded in `docs/failure-log.md`,
fixed in the C++ fixture, and re-reviewed before retrying.

Final normal build (system-first PATH, `-j4`) used the existing
`build-spec185-b0c-normal` tree:

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-spec185-b0c-normal \
  ./waf -o build-spec185-b0c-normal build -j4 \
  --targets=spec185-extension-registry
```

It completed successfully in 16.058 s. Binary identities:

```text
spec185-extension-registry  ff57dd6cea6056df02b632a6f69bb636953c7708412ae50830f17e53d43d3fdf
libndnsf-distributed-inference.so  a773e964a2264193768e61c21f3c3b20281a06f6fcc44edf8e6294666444197c
```

The final normal selector was:

```text
./build-spec185-b0c-normal/spec185-extension-registry \
  --run_test=Spec185ExtensionRegistry --log_level=test_suite
```

Result: **10/10 C++ cases passed; no errors detected**. Log:
`.codex-tmp/spec185-b2e/normal-runtime-graph-fix.log` (SHA-256
`470c7df9ed65e9b9d63545158ac17fa847e9b9b83a6085e6c7e5dcdd19cfab13`).

The separate clang/TSan tree `build-spec185-b0c-tsan-clang` was built with
`CC=/usr/bin/clang`, `CXX=/usr/bin/clang++`, the same `-j4` boundary, and the
same selector was run twice with
`TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`. Both runs were
**10/10 with no TSan diagnostic**. Logs:

- build: `.codex-tmp/spec185-b2e/tsan-build.log`, SHA-256
  `460329e320f76cd5d0ce51179a413661dabdab0364c9f05b0a948afa7d355035`;
- run 1: `.codex-tmp/spec185-b2e/tsan-runtime-1.log`, SHA-256
  `600e34e3e5cab8b2568fac011c210ab604ce2391971c36b488409ebf8b5fa617`;
- run 2: `.codex-tmp/spec185-b2e/tsan-runtime-2.log`, SHA-256
  `fe8e84119bf2113ddb76d4f564308fe93d2c0fb2f7a080105e0756879313ee91`.

The installed boundary used `/tmp/spec185-b2e-extension-prefix`: the verified
B1 dependency closure was reused, the current DI library and the T016 public
header closure were overlaid, and the current DI library hash matched the
normal candidate. With the required system-first PATH, the checked-in script
passed its pkg-config, source-tree include leak, `readelf`, `ldd`, compile,
and execution checks:

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  tests/installed-api/run-spec185-extension-consumer.sh \
  /tmp/spec185-b2e-extension-prefix /tmp/spec185-b2e/installed-consumer-system
Spec185ExtensionConsumer PASS
```

Log: `.codex-tmp/spec185-b2e/installed-consumer-system.log` (SHA-256
`66171b28495965db59f2681d703b62e4760da784ea255577754481b4a0899b98`). An
ambient-PATH attempt selected Linuxbrew `ld` and failed to resolve system
protobuf/Boost/OpenSSL/ONNX Runtime symbols; that harness boundary is retained
in `.codex-tmp/spec185-b2e/installed-consumer-r2.log` and is not counted as a
product failure. A full Waf install still reaches the repository's unrelated
Python editable-install hook; this result therefore qualifies the installed C++
consumer boundary, not full-tree packaging.

## Five-lane and miss retrospective

- **static**: PASS. Four frozen reviews covered complete production/test/build
  diffs; the only findings were repaired fixture or review-snapshot issues.
- **compile-link**: PASS after the initial `BOOST_CHECK` macro error and the
  subsequent fresh normal/TSan builds. The source closure links the complete DI
  target and the standalone header consumer.
- **runtime-test**: PASS for the 10-case C++ selector, two repeated TSan runs,
  and the installed-prefix header/library consumer. The selector exercises
  cancellation, deadline, identity, ACK/publication fence, registry freeze,
  replacement, runner isolation, and concurrent lookup.
- **unobserved**: real remote preparation/fetch, public Runtime prepare/request,
  conversation persistence, Provider assembly, full process qualification,
  Python binding behavior, full Waf packaging, and Tiger/SIF execution remain
  later B2–B9 exits. This B2E record does not infer those capabilities.

## Closure decision

B2E has a stable observable exit: cooperative extension registration and control
are compiled, exercised natively, checked under TSan, and consumable through the
installed public header boundary. T016 may be marked `PASS`; the next dependency
assigned task is T003/B2 preparation. No SIF/Tiger job or remote experiment was
started.

## Design/API synchronization

`python3 Design/build-api-reference.py --changed-only` completed with 315 source
files and 17,199 declarations; the generated current DI reference now includes
`ExtensionControl`, cooperative splitter/placement ports, placement registry,
and `planNativeRequestCooperative`. `python3 Design/build-behavior-coverage.py`
completed with 5,999 function entries. These are source-bound references only;
target references and the double-PDF delivery remain T014/B9 work.
