# B8 Python binding acceptance

**Task**: T012 Thin Python Prepared Model Facade
**Batch**: B8
**Status**: `PASS`
**Date**: 2026-09-15

## Scope and boundary

B8 validates the Python boundary only. The pybind module binds the existing C++
Runtime, User, PreparedModel, RequestHandle, EventReader, Conversation and
Provider owners; `_async.py` only adapts native completion and reader callbacks
to asyncio. No Python assertion is used as native protocol or model-behavior
qualification. T013 remains the C++ process qualification authority.

The inherited Spec182 binding suite was updated where its requester assertion
still required the retired hand-built `nativeRequestRuntimeFromJson` executable
path. The canonical requester route is now
`Runtime::open -> User::prepare -> User::request(PreparedModel)`; the older
`PreparedModel::request` spelling remains a deprecated compatibility wrapper.
This ownership correction does not weaken the native runtime parser binding or
catalog validation.

## Static review

The final review uses the official skill at
`/home/tianxing/.codex/skills/review-agent/SKILL.md` against immutable snapshot
`.codex-tmp/spec185-t012-review-v19-20260915` (base
`23811d8619b109bc428f6e84cdabb05ad836089c`, 11 files, manifest
`manifest.sha256`). The review sequence retained three failed snapshots: v15
found the process-global exception-type pointer (P2), v17 found the captured
translator lambda could not satisfy pybind11's function-pointer type (P1),
and v18 found the first teardown fixture used an unbound handle constructor
(P1). v19 returned `STATIC_PASS` with no P0-P3 findings; the final translator
is non-capturing and resolves the current interpreter's module type for each
translation.

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | covered | `bindDistributedInference`, `Runtime`, `User`, `PreparedModel`, `api/_async.py`, `DI_NativeRequester` | `rg` symbol/caller trace; public import and requester route checks | native owner remains the entry; no Python planning owner |
| `implementation and wire` | covered | `di_bindings.cpp`; `PreparedModel`/`NativeInferenceClient` deadline bridge; async callback adapters | full snapshot review; kw-only and absolute-deadline source checks | standard exceptions delegate to pybind; structured `DiError` fields preserved |
| `test/harness/oracle` | covered | `test_spec185_prepared_model.py`; inherited `test_spec182_native_bindings.py` | pytest selectors and source assertions; fake callback/loop cancellation cases | wrapper assertions cover mapping and lifecycle boundaries; native behavior remains C++ |
| `build/source closure` | covered | Waf DI target; `pythonWrapper/setup.py` extension | affected-target build and extension link command; candidate `LD_LIBRARY_PATH` closure | DI and extension both linked successfully; no competing build |
| `migration/evidence` | covered | public `api/__init__.py`, `provider_api.py`, requester compatibility test | old-route assertion update and import surface checks | retired requester parser assertion corrected; no silent `_native=None` fallback |

## Compile and runtime evidence

The affected DI target was rebuilt with the system compiler and `-j4`:

```text
build-spec185-b0c-normal
command: PATH=/usr/bin:/bin:/usr/sbin:/sbin CXX=/usr/bin/g++ ./waf build -j4 --out=build-spec185-b0c-normal --targets=ndnsf-distributed-inference
RC=0  ELAPSED=1:24.78  MAXRSS_KB=2324640
```

The extension was then rebuilt in `pythonWrapper` against that candidate and
the matching NAC-ABE prefix. The final rebuild after the translator fix was
`RC=0`; the link line records the candidate DI library and Python extension
output in `.codex-tmp/spec185-t012-extension-build-20260915-r4.log`.

With the candidate DI, NAC-ABE, NDN-SVS and Boost directories first in
`LD_LIBRARY_PATH`, the focused B8 suite passed:

```text
python3 -m pytest -q tests/python/test_spec185_prepared_model.py
9 passed in 0.95s
```

The inherited compatibility suite and the focused B8 suite were then run
together against the same built extension:

```text
python3 -m pytest -q tests/python/test_spec185_prepared_model.py tests/python/test_spec182_native_bindings.py
31 passed in 0.95s
```

The first compatibility run exposed two real boundaries and is retained in
`.codex-tmp/spec185-t012-python-test-spec182-20260915.log`: the custom C++
exception translator swallowed `std::invalid_argument`, producing an opaque
Python `SystemError`, and one inherited source assertion named a requester
route retired by Spec185. The translator now rethrows non-`DiError` exceptions
for pybind11's default translators, and the inherited assertion checks the
current prepared-owner route. The final combined run is `RC=0`; the refreshed
teardown subprocess also exercises `DiError` field translation before exiting.

Because the deadline bridge is C++ support code, the native selectors were
rebuilt in the same `build-spec185-b0c-normal` tree (`RC=0`, 2:24.38,
`MAXRSS_KB=2108120`) and rerun after the wrapper review:

```text
spec185-process: 5 cases, RC=0, ELAPSED=6:49.73, MAXRSS_KB=76772
spec185-prepared-request: 15 cases, RC=0, ELAPSED=1:12.54
spec185-prepared-conversation: 16 cases, RC=0, ELAPSED=1:12.81
```
Each selector emitted `*** No errors detected`; these C++ runs preserve the
native request, conversation, cancellation, deadline and cleanup authority.

The batch composition review used immutable snapshot
`.codex-tmp/spec185-b8-composition-v1-20260915` (base
`23811d8619b109bc428f6e84cdabb05ad836089c`, 17 files) and returned
`B8_COMPOSITION_PASS` with no P0-P3 findings. It confirmed that the Python
surface enters the existing `Runtime -> User -> PreparedModel -> RequestHandle`
owner path, that the complete DI/Core source closure and extension link are
registered, and that the evidence/task state matches the observed tests.

## Closure decision and limits

`STATIC_PASS` plus the affected DI/extension builds, refreshed native
selectors and 31 Python tests close the B8 wrapper boundary. This does not
extend the C++ qualification: protocol, provider, model and conversation
behavior remain evidenced by the C++ selectors and process matrix in
[B7 final convergence](b7-cpp-qualification.md#b7-final-candidate-convergence-20260915).

| Gate | Result |
| --- | --- |
| `static` | PASS, v19 official review; no P0-P3 |
| `compile-link` | PASS, DI target and Python extension, `RC=0`; refreshed C++ selectors also linked |
| `runtime-test` | PASS, 31 Python tests plus refreshed C++ process/request/conversation selectors, all `RC=0` |
| `sanitizer` | `UNOBSERVED` for the Python wrapper; native sanitizer qualification is in B7 |
| `unobserved` | subinterpreter/packaging-wheel stress remains outside B8; native ASan re-run after the deadline support change is not repeated here |
