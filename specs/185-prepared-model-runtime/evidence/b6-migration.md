# B6 Native Caller Migration Evidence

## Scope and current boundary

Task: T011 Native Caller Migration and Compatibility Registry.  The batch base
is `98a48dad`; the v12 candidate is `CLOSED_FOR_VALIDATION` after the installed
consumer, C++ selectors, and minimum unary/stream process attempts were
recorded against one binary identity.  B7 owns full native process
qualification; this record does not promote a compatibility check to full
qualification.

The maintained public C++ route is `Runtime::open` → `User::prepare` (or
`prepareAsync`) → `PreparedModel` request/conversation, with Provider-only
applications using `Runtime::open(ProviderConfig)` → `Provider::serve`.
`caller-matrix.md` is the source inventory for the retained legacy and
delegated Python/Tiger entries.

## Review trace

- T011 static review v5: snapshot `.codex-tmp/spec185-t011-review-v5`, base
  `98a48dad`, diff SHA256
  `98069748a87c7f27646b3dda7d5ade2d856146e93230e841ba52e38ee199adf0`, paths
  SHA256 `7778b1c99fd7063fb36fa5c6e559a83ba1d498bf998bc5482868036bdff99be8`;
  official `review-agent`: `STATIC_PASS`, no P0/P1/P2/P3.
- B6 composition review on the same snapshot: `B6_COMPOSITION_FAIL`, P2 for
  stream oracle conversion errors escaping the stable marker and for this
  evidence path being referenced before it existed.  Both findings are being
  repaired in the next frozen snapshot; no build or runtime result is claimed
  here yet.
- T011 static review v9: snapshot
  `.codex-tmp/spec185-t011-review-v9-20260914`, base `98a48dad`, diff SHA256
  `fe09e18fefeeae41b79d3ed5ffde55a35418eeeb997432aa34f820039677170d`;
  official `review-agent`: `STATIC_FAIL`, P1.  A readiness retry was checked
  after `cancelled`, so cancellation could keep rearming timers until the
  request deadline.  No build or runtime result was claimed for this snapshot.
- T011 static repair review v10: snapshot
  `.codex-tmp/spec185-t011-review-v10-20260914`, base `98a48dad`, diff SHA256
  `1218d18117ab65e99e1604013eae1a49eb88e61c8159384a14582f4215d09fee`, paths
  SHA256 `10e4bd8f5c4a8f1e33d611350c54b2d6ed56fbe875bb1a6a579475be9cf47804`;
  official `review-agent`: `STATIC_PASS`, no P0/P1/P2/P3, followed by
  `B6_COMPOSITION_PASS`.  The retry entry fence, terminal cancellation
  closure, timer-fired clearing and same-attempt checks were reviewed.  The
  five lanes are statically covered; compile-link, runtime-test and sanitizer
  remain unobserved by this review.
- T011 static repair review v12: snapshot
  `.codex-tmp/spec185-t011-review-v12-20260914`, base `98a48dad`, diff SHA256
  `f00f6d841334df21ff08e9ccced945963bc2799e44f36b92aa9946d2771db091`, paths
  SHA256 `a4cb7a803fa72817a2c5e6b8036ca2dc99cc59464ca2f4eaee4aa9ba650f40e4`;
  official `review-agent`: `STATIC_PASS`, no P0/P1/P2/P3. The explicit
  `<cstdlib>` declaration, paired PIB/TPM validation, external KeyChain
  identity reuse, memory fallback, exception cleanup, and v10 retry/cancel
  invariants were reviewed. Compile-link, runtime-test and sanitizer remain
  unobserved by this review.
- B6 composition review on v12: `B6_COMPOSITION_PASS`, same snapshot and
  hashes above. The full T011 caller route, external KeyChain identity path,
  permission bootstrap/readiness retry, stream/replacement mapping, public
  headers, pkg-config/Waf closure, installed consumer and caller matrix were
  reviewed with no P0-P3 finding. Compile-link, runtime-test and sanitizer
  remained unobserved until the batch validation below.

## Five-lane status before batch validation

| Lane | Covered scope | Current result |
| --- | --- | --- |
| production entry/callers | `DI_NativeRequester`, `DI_PreparedModel`, `DI_PreparedProvider`, public umbrellas, caller matrix | `STATIC_PASS`; unary/stream process route `RC=0` |
| implementation/wire | provider selection, stream replacement, conversation generation identity, terminal/error mapping | `STATIC_PASS`; composition and minimum process markers pass |
| test/harness/oracle | `Spec185Compatibility`, installed caller source/script, C++ route markers | compile/link/runtime pass for B6 selectors and unary/stream |
| build/source closure | Waf example/test registration, short header install, pkg-config consumer boundary | 359/359 build and external consumer pass |
| migration/evidence | caller matrix, API catalog/exposure/public contract, this record | `CLOSED_FOR_VALIDATION`; full qualification remains B7 |

## Dynamic and closure record

The first process attempts were not protocol passes.  Unary v2 and v3 both
reached the requester after controller, authority, and provider startup, but
ended at the production authorization boundary:

```text
NDNSF_NAC_BOOTSTRAP_PENDING role=user
Reject request without decryption readiness serviceName=/Inference/NativeUnary
NATIVE_REQUEST_ROUTE=Runtime.open->User.prepare->PreparedModel.request
NATIVE_REQUEST_STAGE_FAILED code=NATIVE_REQUEST_BEGIN_FAILED boundary=begin
```

The retained roots are `.codex-tmp/spec185-b6/unary-v2/` and
`.codex-tmp/spec185-b6/unary-v3/`; launcher records are
`unary-v2.log/.rc` and `unary-v3.log/.rc`.  This exposed the missing
Runtime-owned controller permission fetch and pre-begin readiness wait.  The
v10 candidate adds that production wiring and a bounded non-blocking retry;
the next step is one `-j4` build of the affected Core/DI closure, followed by
the compatibility selector, installed-prefix consumer, ELF/no-Python checks,
and a fresh minimum unary/stream process attempt.  A missing authority/provider
or startup failure remains `UNQUALIFIED` and is not converted to a protocol
PASS.

The first post-repair unary attempt (`unary-v4`) reached the same native route
and started Controller, authority, and Provider, but the permission response
could not be decrypted: the Controller encrypted to the requester certificate
from the process PIB while Runtime had created a different in-memory
certificate. It ended with `NATIVE_REQUEST_BOOTSTRAP_TIMEOUT` after the
permission bootstrap deadline; no ACK or Selection was observed. The raw root
is `.codex-tmp/spec185-b6/unary-v4/`. The v12 candidate makes Runtime reuse a
configured `NDN_CLIENT_PIB`/`NDN_CLIENT_TPM` pair (and rejects a partial pair),
retaining the memory fallback for isolated callers. A fresh build and process
attempt are required; this boundary is not a protocol result.

The next launcher attempt (`unary-v5`) stopped before any Controller or native
process was started. Its command constructed an invalid repository-relative
NAC-ABE `LD_LIBRARY_PATH` entry, so Python wrapper import loaded the candidate
Core library and failed on missing `ndn::nacabe::Consumer::clearCache`. The raw
root `.codex-tmp/spec185-b6/unary-v5/` is retained. This is a launcher
environment boundary, not a native protocol result; the retry uses the actual
NAC-ABE install prefix recorded by the build preflight.

## Batch validation and closure

The v12 candidate was rebuilt in the existing `build-spec185-b0c-normal` tree
with the system-first compiler and `-j4`. The command and environment are
retained in `.codex-tmp/spec185-b6/process-deps-build-v7.log`; it built 359/359
tasks in 56.675 seconds with return code 0. The vmstat record shows no
sustained swap-in/out. Candidate identities are:

| Artifact | SHA256 |
| --- | --- |
| `libndnsf-distributed-inference.so` | `af713523d3e3b723c6751760c5f37a6499f60be50ac56f1b5b27245b10b5f490` |
| `libndn-service-framework.so` | `b80b4603a059c43f4c2eb653b72d392f541fb5878757e0840d3080b9c8671e37` |
| `examples/DI_NativeRequester` | `a3c4ac16dbd616869b03e52f69261946251d89d628ce1e972a01a53a7f7e0648` |

The C++ `Spec185Compatibility` selector (`compat-v2`) and in-tree caller
consumer (`consumer-v2`) both returned `RC=0`; the external installed-prefix
consumer (`installed-consumer-v12`) compiled through pkg-config, passed its
ELF/loader checks, and returned `Spec185CallerConsumer PASS`. The three public
example `--help` checks also returned `RC=0`. ELF records in
`.codex-tmp/spec185-b6/elf-v12/` show no Python NEEDED entry and no runtime
source-tree library leak; the `nm` symbol inventories are in
`.codex-tmp/spec185-b6/nm-v12/`.

Fresh cross-process C++ requester runs use the same candidate and the actual
NAC-ABE install prefix. `unary-v6` returned `RC=0`, emitted
`NATIVE_NUMERICAL_ORACLE_PASS tensor=predictions values=3` and
`NATIVE_REQUEST_SUCCEEDED`; its Provider log contains
`NDNSF_DI_GRANT_VERIFICATION` with `status=VERIFIED` and execution evidence
`runnerKind=onnxruntime-cpu`, `realCompute=true`, `cpuFallbackUsed=false`.
`stream-v6` returned `RC=0`, emitted eight stream events,
`NATIVE_STREAM_ORACLE_PASS tokens=8 events=8`, and
`NATIVE_REQUEST_SUCCEEDED`; Provider traces include the matching stream events,
execution completion, and response dispatch. Raw roots remain
`.codex-tmp/spec185-b6/unary-v6/` and `.codex-tmp/spec185-b6/stream-v6/`.

The retained old/new comparison is marker- and failure-semantic based. The
legacy Spec182 cross-process baseline in
`specs/182-native-di-python-bindings/evidence/r11-b10-g13-current-cross-process-chain-20260910.md`
uses the same numerical/stream success markers and Provider verification
receipt. The new public route preserves those success markers while making
the route explicit (`Runtime.open -> User.prepare -> PreparedModel.request`)
and maps bootstrap failures to `NATIVE_REQUEST_STAGE_FAILED` with a concrete
boundary (`begin` or `bootstrap`); no failed attempt emitted a success marker.
The legacy C4/C5 callers remain retained in the matrix for B7's independent
old/new process qualification and are not silently marked migrated.

Batch growth decision: `STOP_GROWTH`.

Closure decision: `CLOSED_FOR_VALIDATION`; the T011 exit is satisfied by the
static/composition gates, installed C++ consumer, compatibility selector,
public examples, fresh unary/stream process oracles, matrix coverage, and
ELF/no-Python checks. Full failure, cancellation, revoke, continuation and
replacement qualification remains B7/T013 and is not claimed here.

Batch growth decision: `STOP_GROWTH`.

Closure decision: `OPEN` until the post-repair composition review and the
recorded batch validation satisfy the T011 exit.  T011 remains `[ ]` in
`tasks.md` until then.
