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
candidate-local C++/model results only. The host through-MiniNDN pair was later
retested twice after input segmentation; the SIF/APP host-gate and pair
mutation checks remain open, so this evidence does not close T001, T003 or T004.

**Tasks**: T002 C++ YOLO selector and MiniNDN caller wiring; T003 local pair gate
**Batch**: B187-LOCAL-YOLO
**Status**: PARTIAL
**Base**: local checkpoint `6be4968a`; candidate SIF provenance remains
`a0740640`.

## 2026-09-16 local native request segmentation and through-MiniNDN runs

The previous local run proved the actual blocker: the 6.55 MB request input was
published as one Data packet and exceeded the 8,800-byte transport limit. The
request path now keeps the small-input binding unchanged and, for the large
case, publishes 4,096-byte encrypted chunks with NDN segment components and a
`FinalBlockId`. The Provider fetches the base name with `CanBePrefix`, validates
segment ordering, final-block and size limits, decrypts each segment with its
request/segment binding, and dispatches only after the complete envelope has
been assembled. The 6,555,271-byte YOLO input was observed as **1,601
segments**; no single Data packet exceeded the configured chunk bound.

The C++ selector now parses the provider's
`NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED` JSON semantically, so the test accepts
the current `boost::property_tree` scalar serialization (quoted or native
number/boolean) while still requiring the exact request ID, attempt `1`, plan
digest and `executionCompleted=true`. The MiniNDN runner passes
`--color_output=no` to the Boost.Test selector so the strict terminal marker is
not hidden behind an ANSI prefix.

Two fresh host MiniNDN runs used the same candidate-bound native config/input,
provider binary, authority inputs and model package. Both completed the C++
User request through ACK close, Selection commit/accept, provider execution and
terminal response:

| Run | Terminal result | Input segmentation | Native result | Elapsed | Raw log |
| --- | --- | --- | --- | --- | --- |
| `r27` | `SPEC180_CASE_RESULT status=PASS case=Y-A` | 6,555,271 bytes / 4,096-byte chunks / 1,601 segments | 7,267 bytes | 40.17s | `.codex-tmp/spec187-local-yolo-r27-selector-no-color.log` |
| `r28` | `SPEC180_CASE_RESULT status=PASS case=Y-A` | same candidate and input contract | 7,267 bytes | 39.62s | `.codex-tmp/spec187-local-yolo-r28-fixed-key.log` |

The r27 output root is `results/spec187-local-yolo-r27/`; r28 is
`results/spec187-local-yolo-r28/`. Each contains `subcase-result.json` with
`status=PASS`, `terminalResponse=true`, and the C++ selector's
`SPEC187_NATIVE_REQUEST_PASS`. Provider logs contain correlated
`NDNSF_DI_NATIVE_SELECTION_ACCEPTED`,
`NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED` (`onnxruntime-cpu`,
`executionCompleted=true`) and
`NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED` records. The earlier r21
single-Data failure, r23-r26 integration failures, and r27 selector-result
false negative remain preserved under `.codex-tmp/`; the r28 first preflight
attempt also remains recorded as `REQUEST_ENVELOPE_KEY_UNAVAILABLE` before it
was corrected to reuse the candidate key.

The test-only JSON matcher and runner flag each passed the official read-only
`review-agent` static gate (`STATIC_PASS`). The affected selector was rebuilt
with `-j4` in 30.668s, and `spec180_native_build.py build --jobs 4 --binding
auto` regenerated and verified the native identity manifest with
`SPEC180_NATIVE_IDENTITY_OK`.

This is a **local host MiniNDN through-chain PASS** for the YOLO case. It is not
yet a SIF/APP or TigerCluster qualification: T001's host-gate/pair closure,
pre-pack ordering and candidate runtime execution remain open, so T003 and
T004 stay partial/waiting and no external promotion is claimed.

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
- **tests/evidence**: focused Python checks, the C++ served-provider native
  fixture and two host C++ User-through-MiniNDN runs pass. The SIF/APP-bound
  case and regular-base host-gate qualification remain unobserved.

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

The host C++ User process now completes twice through the actual MiniNDN/NFD
network with candidate-bound config/input and the segmented request path. T002
still depends on T001's formal candidate-closure dependency in the active task
graph. T003 remains **PARTIAL** until the regular base SIF/APP host-gate and
same-pair SIF execution are accepted; T004 remains **WAITING_EXTERNAL_INPUT**.
No TigerCluster promotion was started.

External-input recheck at 2026-09-15 17:48 -05:00 found no regular `.sif` under
the repository or `/home/tianxing/NDN`; the only matches were pytest fixtures
and Apptainer probe images, none of which satisfy the Spec187 base contract.
