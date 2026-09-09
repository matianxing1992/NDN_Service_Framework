# R5-B9 Native Conversation Owner Configuration

## Scope and Result

R5-B9 implements the bounded configuration exit for T013-E. The C++ binding now reads an
operator-pinned `ndnsf-di-native-conversation-v1` object, validates owner-only key files and
relative paths, constructs `NativeConversationJournal`/`NativeConversationCoordinator`, and
injects the opaque coordinator into `NativeInferenceClient`. Python retains only the opaque
handle and forwards the nested configuration; it does not read key bytes or create a second
journal. The batch remains `PARTIAL`: real Provider receipt/control, cross-process two-turn,
recovery/replacement, and T016 are deliberately outside this exit.

## Review Trace and Closure Decision

- Review skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- Review skill SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Review baseline: `172e1d1b`
- Diff scope: `_ndnsf.cpp`, `di_bindings.cpp`, `service.py`, `app_sdk/client.py`, native
  conversation selector, binding/source checks, requester configuration contract, execution
  card, plan/tasks and case manifest.
- Coverage queries: `rg -n "nativeInferenceClientConfigured|nativeConversationCoordinatorFromConfig|NativeConversation"`
  across binding/service/client/native headers; `git diff --check`; `validate_design.py`;
  `ldd`/`readelf`/`nm` on the rebuilt extension.
- Findings: one static hardening gap (relative conversation paths were not confined to the
  configuration directory) was found and fixed before the final build; final review found no
  remaining actionable finding. The review also confirmed that the configured client uses the
  six-argument C++ constructor and that no Python planner/journal path was added.
- Closure decision: `OPEN_FOR_NEXT_BATCH`. The stable local exit is owner configuration rejected
  before network side effects or accepted into an opaque native client. Trigger for the next
  batch is the real Provider/cross-process conversation path; T013-B, Provider migration and
  T016 remain excluded.

## Coverage Matrix

| Lane | Status | Evidence |
| --- | --- | --- |
| production entry/callers | covered | `NativeServiceUser::nativeConversationCoordinatorFromConfig`, `ServiceUser.native_conversation_coordinator_from_config`, `APPClient.configure_native_requester_from_config`; queried with the binding/client `rg` commands above |
| implementation and wire | covered | `NativeConversationJournalConfig`, owner identity/service/digest checks, `NativeInferenceClient` conversation constructor and opaque pybind class; source review plus C++ selector |
| test/harness/oracle | covered for local owner boundary; gap for real Provider/cross-process | `Spec182Conversation/ExplicitOwnerConfigurationFencesPathOnlyConstruction`, existing `Spec182Conversation/*`, 15 Python binding/legacy cases; no network harness run |
| build/source closure | covered | shared `ndnsf-distributed-inference` target then `_ndnsf` extension, candidate RPATH/`ldd` and constructor symbols; first stale-library link miss is recorded below |
| migration/evidence | gap for qualification | tasks/plan/execution card/case manifest and this record updated; T013-A/T013-B/T016 and real two-turn qualification remain open |

## Validation

### Focused Native and Python Checks

```text
./build-nac182/unit-tests --run_test=Spec182Conversation/ExplicitOwnerConfigurationFencesPathOnlyConstruction --log_level=test_suite
-> exit 0; 1 case, no errors

./build-nac182/unit-tests --run_test=Spec182Conversation/* --log_level=test_suite
-> exit 0; 6 cases, no errors

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference python3 -m pytest -q \
  tests/python/test_spec182_native_bindings.py tests/python/test_spec182_legacy_exclusion.py
-> exit 0; 15 passed

python3 specs/182-native-di-python-bindings/checklists/validate_design.py
-> exit 0; errors=[]; 17 parent tasks, 39 execution/progress units

python3 -m py_compile pythonWrapper/ndnsf/service.py \
  NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py \
  tests/python/test_spec182_native_bindings.py
-> exit 0
```

### Build Measurement

The shared target was built first with the repository system-first toolchain:

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  WAFLOCK=.lock-waf ./waf build --out=build-nac182 \
  --targets=ndnsf-distributed-inference -j4
-> exit 0; 1m38.315s

PATH=/usr/bin:/bin:/usr/sbin:/sbin CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  WAFLOCK=.lock-waf ./waf build --out=build-nac182 --targets=unit-tests -j4
-> exit 0; 46.323s

NDNSF_LIBRARY_DIR=/home/tianxing/NDN/ndn-service-framework/build-nac182 \
  NDNSF_NAC_ABE_PREFIX=/home/tianxing/NDN/nac-abe-integration-182/install \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  python3 setup.py build_ext --inplace --force --parallel 4
-> exit 0; 9m34.555s
```

`ldd` resolved `libndnsf-distributed-inference.so` and `libndn-service-framework.so` from
`build-nac182`, `libnac-abe.so` from the explicit NAC-ABE prefix, and the extension RUNPATH
contained only those candidate directories plus the fixed SVS path. `nm -D` showed both the
runtime constructor with `NativeConversationCoordinator` and the coordinator journal symbols.

## Misses and Failure Boundaries

- The first extension link used `setup.py` without `NDNSF_LIBRARY_DIR` and stopped at
  `/usr/bin/ld: cannot find -lndnsf-distributed-inference` (exit 1 after 9m14.979s). The source
  compile had completed; the retry used the same `build-nac182` shared target and explicit
  NAC-ABE prefix.
- During the final forced extension compile, one `vmstat` sample recorded swap-out while the
  single `_ndnsf.cpp` translation unit reached about 5.4 GB RSS. Later samples returned to zero
  swap-out and the build completed. Future builds on this host should use `-j2` after any
  sustained swap signal, per `AGENTS.md`; this batch did not start a competing build.
- A direct Python construction probe reached the first external boundary before the new parser:
  `NativeServiceUser` initialization failed because `/run/nfd/nfd.sock` was unavailable. No
  network or conversation request was attempted; the C++ owner selector and source/binding
  checks remain the local evidence for this batch. This is not a Provider qualification result.

## Remaining Work

R5-B9 does not close T011-C or T013-A. The next production batch must exercise a real configured
native requester through Provider receipt/control and the two-turn continuation path, then add
recovery/replacement and no-Python isolation evidence before T014/T015/T016 can proceed.
