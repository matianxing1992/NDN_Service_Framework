# B187-LOCAL-YOLO Evidence

## 2026-09-16 candidate update

Candidate SIF `e6cef05a949c3b865b35424ddb486bee05ea8a0023dd9ba4f7f5eda556541657`
was built with Apptainer 1.5.3 and the clean source revision `a0740640`. The
container C++ unit smoke `unit-r2` passed (four DI/YOLO native test sources,
`DI_CPP_UNIT_SMOKE_PASS`, 52.56s); the native ORT/DI YOLO smoke `yolo-r2`
passed three runs with 50 rows and maximum absolute error `0.000534058`
(`YOLO_CPU_NATIVE_RUNNER_PASS`, 9.76s). The first `unit-r1` and `yolo-r1`
runs failed at the isolated HOME boundary (`/home/tianxing/.ndn`, `rc=134`);
their records remain in `.codex-tmp/spec187-clean-restart/`. These are
candidate-local C++/model results only. A candidate-bound requester config and
input, two real through-MiniNDN runs, and the host-gate/pair mutation checks
are still missing, so this evidence does not close T002 or T003.

**Tasks**: T002 C++ YOLO selector and MiniNDN caller wiring; T003 local pair gate
**Batch**: B187-LOCAL-YOLO
**Status**: PARTIAL
**Base**: `6bade78d`

## Scope and five lanes

- **production/callers**: `SPEC187_NATIVE_MODE=1` makes the maintained
  MiniNDN runner start the registered C++ `NativeRequesterThroughMiniNdn`
  selector as the User process. The child inherits the node-scoped
  `NDN_CLIENT_TRANSPORT`; Python remains responsible for MiniNDN/NFD,
  Controller/Repo/Provider process startup and bounded cleanup.
- **implementation**: `tests/wscript` registers `spec187-yolo-minindn` with
  the complete DI integration source closure. The selector calls
  `Runtime::open -> user().prepare -> PreparedModel::request`, checks non-empty
  plan/model/result identities, correlates C++ ACK/Selection/Provider stage
  records by request, attempt and plan (including monotonic epoch order),
  writes the result, and emits `SPEC187_NATIVE_REQUEST_PASS`.
- **state/lifecycle**: the runner validates selector, config, input and output
  paths before `start_network()`; an existing output is rejected before any
  MiniNDN side effect. The C++ selector has a 60-second request/drain bound.
- **build/source closure**: the normal affected-target build linked
  `spec187-yolo-minindn` from `build-spec185-b0c-normal` with system-first
  `/usr/bin/g++ -B/usr/bin` and `-j4`.
- **tests/evidence**: focused Python checks and the C++ served-provider native
  fixture pass. The true C++ User-through-MiniNDN case has not run because no
  Spec187 native requester config/input pair or regular base SIF is available.

## Verification

Commands from the repository root:

```text
python3 -m py_compile Experiments/NDNSF_DI_YoloAckDriven_Minindn.py Experiments/TigerCluster/tests/test_spec187_yolo.py
pytest -q Experiments/TigerCluster/tests/test_spec187_yolo.py Experiments/TigerCluster/tests/test_sif_app.py -k 'spec187 or publish_rejects_changed_base' --disable-warnings --maxfail=1
PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin CXX=/usr/bin/g++ CC=/usr/bin/gcc ./waf -o build-spec185-b0c-normal build --targets=spec187-yolo-minindn -j4
```

Results: **2 focused Python tests passed**; after one compile-time correction,
the affected target compile/link returned `0` in **1m6.118s** with `-j4`.
The immutable review snapshot
`.codex-tmp/spec187-stage-evidence-review-20260915-r7/` has diff SHA256
`49625e235a1b2a5cd19b9f188e353ac27812c4dd002448cee899b6f18a3ce996`; the
official read-only `review-agent` returned **STATIC_PASS**. The review required
correlated stage records rather than a marker-only terminal check.

The C++ selector's independent served-provider case ran with the host NAC-ABE
library preloaded and returned `0` in about 7.11s with
`*** No errors detected`; raw log is
`.codex-tmp/spec187-t002-selector-r3.log` (rc `.codex-tmp/spec187-t002-selector-r3.rc`
is `0`). The first unpreloaded attempt
stopped at the dynamic loader boundary (`undefined symbol
AttributeAuthority::getPublicParametersVersion`, rc `127`), recorded in
`.codex-tmp/spec187-t002-selector-r1.log`; it is a host ABI closure issue,
not a protocol result.

The new through-MiniNDN selector was also run without its required external
inputs. It failed closed in about 200 microseconds with the expected missing
`SPEC187_NATIVE_REQUEST_CONFIG` assertion; see
`.codex-tmp/spec187-t002-native-selector-missing-input-r2.log` (observed rc
`201` in the paired `.rc` file).

The selector's stage oracle now requires these correlated records from the
same request/attempt/plan: `NDNSF_DI_NATIVE_ACK_CLOSED`,
`NDNSF_DI_NATIVE_SELECTION_COMMITTED`,
`NDNSF_DI_NATIVE_SELECTION_ACCEPTED`,
`NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED`, and the existing
`NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED` with `executionCompleted=true`.
`ACK_CLOSED` is intentionally pre-plan and carries `ackDigest`; the other
records carry `planDigest` and `epochMs`.

## Remaining boundary

T002 remains **PARTIAL** until a candidate-bound native requester config,
input and output root are supplied and the C++ User process completes through
the actual MiniNDN/NFD network. T003 remains **WAITING_EXTERNAL_INPUT** until
the regular base SIF, host-gate manifest and convergence PASS are available;
no SIF build, two-run local gate or TigerCluster promotion was started.

External-input recheck at 2026-09-15 17:48 -05:00 found no regular `.sif` under
the repository or `/home/tianxing/NDN`; the only matches were pytest fixtures
and Apptainer probe images, none of which satisfy the Spec187 base contract.
