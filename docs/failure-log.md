# Failure Log and Evidence Index

## 2026-09-15 — Spec186 r49/r50 drifted from the successful GPU template

- **Symptom:** r49 job `212374` built NAC-ABE, NDN-SVS and NDNSD, then failed the
  explicit NDN-SVS closure check. Diagnostic job `212376` captured `ld` not found
  and `stdlib.h` not found; both came from Waf's `-B/usr` prefix. r50 job
  `212377` changed that flag and reached the same closure check, but it used the
  r38 intermediate base and GCC instead of the retained successful Clang/base
  tuple, so it was cancelled at 13/284 before promotion.
- **Root cause:** a failed candidate definition was being edited as an
  experiment instead of first comparing it with
  `Experiments/TigerCluster/docs/successful-tiger-gpu-template.md`. The
  successful candidate's base SIF (`44b44d...6907b0`) and Clang 10/O0 recipe were
  available in project storage, but the new definitions silently substituted an
  intermediate SIF, GCC and a different toolchain root.
- **Fix:** stop r50, make the successful-candidate template comparison a required
  skill and SIF-build documentation gate, and add static tests for the frozen
  base/compiler/job references. The next candidate must be rendered by
  `prepare-development-handoff.py render` from the canonical template with an
  explicit difference table before `build-local-sif.sh` is called.
- **Lesson:** changing flags until one check passes is not a reproducible build
  method. Historical success is a design reference, while every source/base/app/
  compiler change still requires a new sealed candidate and its own evidence.

## 2026-09-15 — Spec186 r51 template build hit root-mapped APT sandbox

- **Symptom:** exact-template render and preflight passed, but compute job
  `212385` stopped at the first `apt-get update` with `setgroups 65534 failed`
  and Apptainer exit 100.
- **Root cause:** the historical builder assumed a Debian `_apt` privilege
  transition that the Tiger root-mapped account cannot perform. The exact
  historical base also lacks `/usr/bin/clang-10`, `/opt/onnx` and
  `/opt/rust-prefix`; those are builder prerequisites, not runtime guarantees.
- **Correction:** retain r51 as the exact-template failure; probe the base and
  r38 intermediate capabilities (job `212386`/`212387`) before any retry. The
  next candidate must use a separately sealed toolchain base or an explicitly
  recorded compute exception with a validated APT sandbox policy and a
  project-backed Apptainer temporary directory.
- **Lesson:** a successful definition template is reusable only after its base
  capability and builder privilege assumptions are checked on the actual host;
  do not delete apt or copy host compiler files as an unrecorded workaround.

## 2026-09-14 — Spec186 r29 replay source path drift

- **Symptom:** r29 compiled all 284 native targets and both Python extensions, then Apptainer `%post` exited at `cp: cannot stat '/src/ndnsf/packaging/ndnsf-di-container/jobs/spec180'`.
- **Root cause:** the sealed source archive does not contain the compatibility path under `packaging/`; the maintained Spec180 runtime is owned by `Experiments/TigerCluster/jobs/spec180`, even though a host-side compatibility directory exists.
- **Fix:** point the canonical runtime definition at `Experiments/TigerCluster/jobs/spec180`; add a template regression asserting the canonical path and rejecting the stale source path. Rebuild with a new source handoff because the definition input changed.
- **Lesson:** validate every `%post` copy source against the extracted sealed archive, not only the host checkout; compatibility paths are not archive guarantees.

## 2026-09-11 — Proposal Origin expansion build path

首次文档构建驱动将相对输出目录传入改变 cwd 的 latexmk，导致预期 `main.log` 缺失；不是产品协议失败。修复为绝对路径并保留 r2 输出。证据：[proposal expansion](../specs/184-native-di-closure/evidence/proposal-origin-expansion-20260911.md)。

## 2026-09-11 — Proposal application-validation notes layout

The first document check found an overfull generated notes line after the new
DI table used `input/result`. The source now uses `input and result`; rebuilt
notes pass the layout check. Active Context Mode tasks hashes were stale; actual files supplied
the authority fallback. No product test ran. See
[application-validation audit](PAPER/proposal-defense/research-revision-audit.md)
and `.codex-tmp/proposal-app-validation-20260911/notes-first.log`.

## 2026-09-11 — Proposal email-alignment retrieval boundary

Active Context Mode health rejected stale Spec184 plan/tasks hashes; no stale
search result was used as authority. Project health passed and actual files
supplied the fallback. No product test ran. See
[email-alignment audit](PAPER/proposal-defense/research-revision-audit.md)
and `.codex-tmp/proposal-email-alignment-20260911/context-health.json`.
The first wording scan matched a suffix across table cells; the audit records
the word-boundary correction and preserved `verify-first.log`.

## 2026-09-11 — Proposal terminology retrieval guard boundary

Context Mode rejected a historical timeline query before execution because its
source/category did not meet the guard. No returned history was treated as
authority; project health passed and current repository files supplied evidence.
No product test failed. See [terminology audit](PAPER/proposal-defense/research-revision-audit.md)
and `.codex-tmp/proposal-nonce-challenge-20260911/retrieval-boundary.md`.

## 2026-09-11 — Proposal Reason 3 heading-check boundary

Four paper builds passed, but the PDF checker counted an explanatory reference
to `Reason 2:` as a second heading. Matching the actual heading text fixes this
validator boundary. No product test ran. See
[Reason 3 audit](PAPER/proposal-defense/research-revision-audit.md) and
`.codex-tmp/proposal-reason3-20260911/first-check.md`.

## 2026-09-11 — Proposal DNMP bibliography mirror boundary

The first document validation rejected root/English-entry text inequality:
the DNMP DOI was added to the root `ref.bib` but not its independent `en/ch`
copies. Synchronizing the three entries and rebuilding fixes the metadata
boundary. Earlier slide overflow and Chinese sparse pagination were also
corrected; no product test or experiment ran. See
[DNMP throughline review](PAPER/proposal-defense/dnmp-throughline-review-20260911.md)
and `.codex-tmp/proposal-dnmp-throughline-20260911/validation-first-pass.log`.

## 2026-09-11 — Proposal scope checkpoint hook boundary

The default commit hook rejected repository-wide historical assistant references
before creating a commit. The hook and maintained Spec182 evidence document
`NDNSF_LOCAL_CHECKPOINT=1` for local documentation checkpoints; that mode retains
prohibited-path checks. No hook is changed and no `--no-verify` is used.
See [scope review](PAPER/proposal-defense/invocation-scope-review-20260911.md)
and `.codex-tmp/proposal-invocation-scope-20260911/checkpoint-hook-default.log`.

## 2026-09-11 — Proposal document build invocation boundary

The sentence-review build first stopped before LaTeX execution because its
output directory was created relative to the document cwd while redirection
used the repository's absolute temporary path. Recreating explicit absolute
output directories resolved this invocation error. No product or protocol test
ran at that boundary. The source patch also initially rejected a mismatched
Chinese context line without applying edits; exact-context retry succeeded.
The final PPTX attempt rejected a `final-build` leaf name under the converter's
existing safe-directory rule before clearing/generating anything; the retry
uses the permitted `ndnsf-final-build` name without weakening the guard.
See [document review](PAPER/proposal-defense/sentence-review-20260911.md) and
`.codex-tmp/proposal-sentence-review-20260911/` for the subsequent document checks.

## 2026-09-11 — Spec182 G8 broad selector verbose-log timeout boundary

After the `729555fa` client-close registry checkpoint, the full `*Spec182*/*`
C++ selector was run with `--log_level=test_suite` to retain per-case timing.
The 180-second command timeout expired while executing the longer
`Spec182OnnxWorkerProtocol` frame cases; no failed assertion or protocol error
was emitted before the boundary. Raw output is retained at
`.codex-tmp/spec182-g49-close-registry-20260911/spec182-full.log` with
`spec182-full.rc=124`.

This is a verbose-observer timeout, not a qualification result. The next retry
uses the same fresh binary with `--report_level=no --log_level=message` so the
test process is not slowed by per-case logging; the client suite and targeted
close regression already pass independently.

## 2026-09-11 — Spec182 R11-B9-G8 client-close pending-operation registry compile boundary

The first `-j3` rebuild for the C++ client-close regression reached the final
unit-test translation unit but stopped at the test fixture: `adapters->find()`
returns `shared_ptr<const NativeModelAdapter>`, so the new test attempted a
`dynamic_pointer_cast` that would cast away constness. No production binary or
runtime path was reached. The raw compiler output is retained at
`.codex-tmp/spec182-g49-close-registry-20260911/build-j3.log`.

The fixture now retains the concrete adapter it registers, removing the
constness violation. The retry used the same system-first toolchain and `-j3`,
completed all 190 unit-test build tasks in 57.934 seconds, and showed zero
sustained swap-in/out in the recorded `vmstat` samples. The affected C++ client
suite then passed; this was a test-only first-boundary failure and does not
promote T010/T016 or the remaining native qualification gates.

## 2026-09-11 — Spec182 R11-B8-G48 Qwen native observer type boundary

静态复核发现 Qwen native caller 在 `decode_payload()` 成功返回 JSON array 或 scalar
时仍直接调用 `.get()`。C++ observer 隔离该异常后，terminal 通知可能使 caller 将畸形
事件当作成功。该缺口未触及 Core/Provider；已在
`examples/python/NDNSF-DistributedInference/llm_pipeline/user.py` 增加 mapping 类型门，
并用真实 helper 夹具覆盖 JSON array payload。36 个相关 Python 入口/兼容测试、
`py_compile` 与 `git diff --check` 通过。证据见
[R11-B8-G48](../specs/182-native-di-python-bindings/evidence/r11-b8-g48-qwen-observer-type-guard-20260911.md)。

这是 caller-edge 修复，不是 native request、Provider、跨进程、no-Python 或 T016
资格结果；R11-B8 与 T013/T016/T017 保持原状态。

## 2026-09-11 — Spec182 R11-B8-G47 examples source-closure and `-j3` boundary

The examples-enabled C++ build exposed three target-registration omissions before any
deployment claim was made. The first four-target `-j4` attempt was terminated after available
memory fell to about 0.25 GiB with sustained swap-in/out; this is retained in
`.codex-tmp/spec182-t016-examples-20260911/build.log` and `vmstat-build.log` and is a host
resource boundary. The policy-conforming `-j2` retry completed 277/277 tasks in 31m50.347s,
then the first `di-native-provider-session-smoke` link reported missing canonical ONNX,
`ExecutionLeaseService` and framework publication definitions (`build-j2.log`).

After that closure was repaired, the first `-j3` all-target run reached the
`di-native-plan-schema-smoke` link and reported the same ONNX/framework omission
(`build-all-targets-j3.log`). The next run reached 608/609 and isolated the remaining omission
to `di-native-onnxruntime-smoke` (`build-all-targets-j3-r2.log`). Adding the shared canonical
ONNX source set and candidate framework/ONNX/Protobuf link closure to the three targets made the
bounded retry complete 87/87 in 7m30.258s (`build-onnxruntime-j3-r3.log`). The durable batch
record is [R11-B8-G47](../specs/182-native-di-python-bindings/evidence/r11-b8-g47-examples-source-closure-20260911.md).

The same run also retained a loader identity boundary: with `/usr/local/lib` before the
candidate build root, requester and authority `--help` stopped with old framework undefined
symbols (`DI_NativeRequester-help.log`, `DI_NativeArtifactAuthority-help.log`). Candidate-first
lookup passes `--help`, C++ smoke and Provider `--check-only`; the host build is still rejected
by `verify-runtime-closure.py --reject-prefix /home/tianxing/NDN` as
`RUNTIME_HOST_BOUND_PATH`. Neither condition is counted as a protocol failure, but both are
required gates for the eventual container/multi-machine artifact.

## 2026-09-10 — UAV update slides layout boundary (resolved)

补充模拟到真实 UAV 的差距时，首轮 PDF 构建有纵向溢出；通过缩短文字和调整表格/字号修复。
初始输出 `.codex-tmp/uav-reality-gap/build-1.log` 保留，最终 `build-6.log` 无排版警告，
10 页渲染核对通过。仅文档排版，不是 native/飞行测试结果。
见 [修订与验证记录](NDNSF-UAV/slides/UPDATES_UAV-review.md)。

## 2026-09-11 — Spec182 T016-A broad integration qualification boundary

The fresh current-source `integration-tests --report_level=short` run completed
170 cases with 149 passed, 21 failed, 19 aborted, and 48 failed assertions
(`rc=201`) after 341.73 seconds. The first boundary is
`Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunD2bRequestToFinalResponse`,
where no response publication or observed role output was produced; the same
run then reports failures across the Spec175 tiny-ONNX and Spec170/175 recovery,
streaming, cancellation, and conversation cases. The complete output is
retained at `.codex-tmp/spec182-t016-unit-20260911/integration-tests-final.log`
and the condensed classification at
`.codex-tmp/spec182-t016-unit-20260911/integration-failure-summary.log`.

The active Spec182 selector was run separately from the same fresh binary and
passed 2/2 cases with 21/21 assertions (`rc=0`); raw output is retained at
`.codex-tmp/spec182-t016-unit-20260911/spec182-integration-selector.log`.
Therefore the active Spec182 integration lane is closed for this validation
checkpoint, while the repository-wide integration lane remains unqualified.
The broad failures are not attributed to the delayed-runner capability fix
without a reproducer at that first boundary, and T016 is not promoted.

## 2026-09-11 — Spec182 T016-A delayed-runner capability boundary

After the fixture callback repair, the fresh current-source full unit selector
reduced the failure to one assertion: `NativeProviderRuntimeCarriesOpaqueStateHandleWithoutHostRoundTrip`
reported `sawCompactState=false`; the run had 1,026 cases with 1 failed case
and 1 failed assertion. The raw output is retained at
`.codex-tmp/spec182-t016-unit-20260911/unit-tests-final.log`. Source inspection
found that `NativeProviderRuntime` checked `supportsOpaqueStateHandles()` before
the delayed preparation callback ran, so a callback-created opaque runner was
misclassified and its state was serialized through host tensors. The fix carries
the resolved runner capability in `ProviderRoleResult` and selects the opaque
state path only after worker execution; no callback is invoked early.

## 2026-09-11 — Spec182 T016-A fixture repair compile boundary

The first incremental build after the bounded fixture repair failed while
compiling `distributed-inference-async-runtime.t.cpp`: two coordinator tests
assigned `config.prepareRunner = [runner] ...` before their local runner had
been declared. The raw compiler output is retained at
`.codex-tmp/spec182-t016-unit-20260911/fixture-repair-build.log`. This was a
test-only patch-context mistake; no production source or runtime was reached.
The repair was narrowed to the exact test functions and must pass a fresh
incremental build before any selector result is counted.

## 2026-09-11 — Spec182 T016-A current-source unit fixture contract

The fresh current-source build completed 309/309 with `-j4`, but its
`unit-tests --report_level=short` run exited `rc=201`: 1,026 cases, 11 failed,
8 aborted, and 20 failed assertions. The first boundary remained
`NativeProviderRuntime requires a runner preparation callback` in
`distributed-inference-async-runtime.t.cpp`; the full output is retained at
`.codex-tmp/spec182-t016-unit-20260911/unit-tests.log`. Because the binary was
rebuilt from the current `HEAD`, this is a current C++ fixture/contract failure,
not a stale-artifact result. No T016 status is promoted. The next gate is source
analysis of every `runSamplingEpochs` call and its runner-preparation callback,
followed by a bounded fixture repair and fresh C++ selectors.

## 2026-09-11 — Spec182 T016-A target configuration boundary

The fresh current-source configure completed successfully, but the first Waf
build requested example targets while the configuration had not enabled
examples. Waf stopped before compiling with `Could not find a task generator for
the name 'DI_NativeRequester'`; the complete command/output is retained in
`.codex-tmp/spec182-t016-unit-20260911/build.log`. This is a target-selection
configuration boundary, not a C++ compile or runtime result. The retry uses the
configured `unit-tests` and `integration-tests` targets, then a separate
`--with-examples` configuration for the requester/Provider closure.

## 2026-09-11 — Spec182 T016-A stale unit binary boundary

The first full C++ unit qualification attempt used the existing
`build-nac182/unit-tests` binary. It exited `rc=201` after about 330 seconds
with 11 failed cases and 8 aborted cases; the first failures were
`NativeProviderRuntime requires a runner preparation callback` in conversation
and opaque-state tests. The raw run is retained at
`.codex-tmp/spec182-t016-unit-20260911/`. The binary predated the current
fixture repair even though the source tree had no C++ diff after the native
checkpoint, so this result is a stale-artifact boundary rather than a current
protocol result. The changed gate is a fresh current-source build in a new
output directory before any T016 status decision; no task is promoted by the
failed run.

## 2026-09-10 — Spec182 R11-B8-G40 network integration retry boundary

The first normal run of the G40 network integration exited 1 after the injected
pre-start cases, with only a `BrokenPipeError` from the fake `srun` output
fixture. No NFD or application process was started and no product assertion was
reported. The complete xtrace is retained at
`.codex-tmp/spec182-r11-b8-g40-network-failure-20260910/network-xtrace.log`.
A bounded xtrace rerun and a fresh normal rerun both reached
`NETWORK_SCRIPT_PASS`; the result is recorded in
[R11-B8-G40 evidence](../specs/182-native-di-python-bindings/evidence/r11-b8-g40-scratch-symlink-boundary-20260910.md).
The transient fixture retry is not counted as a product failure or qualification result.

## 2026-09-10 — Proposal DNMP reference download format boundary

The direct download of the DNMP reference returned HTML rather than PDF;
`pdftotext` therefore rejected the document format. The received bytes are
preserved at `.codex-tmp/proposal-dnmp-example-20260910/dnmp.pdf` (HTML despite
the suffix). The browser's indexed primary-paper text was used instead,
including Sections 2.4, 3.2 and 3.3. No product build or protocol test failed.
The source and claim corrections are recorded in
[the proposal audit](PAPER/proposal-defense/dnmp-comparison-review-20260910.md).

## 2026-09-10 — Spec182 R11-B8-G12 host identity selector compile boundary

The first fresh `unit-tests` build for the Provider identity binding batch
rejected the test-only alias `constexpr char[] = HOST_PROVIDER_NAME` because
the compiler cannot infer an array size from another array. The production
change was not reached as a runtime failure; the raw compiler output is
retained at `.codex-tmp/spec182-r11-b8-g12-provider-identity-20260910/build.log`.
The alias was changed to a `const char*` with the same literal contents before
the bounded retry. No test result from this failed build is counted as PASS.

## 2026-09-10 — Spec182 R11-B8 prepared-role fixture checker boundary

The bounded C++ fixture repair first ran cppcheck against
`tests/unit-tests/distributed-inference-async-runtime.t.cpp`. cppcheck stopped
with its internal `AST broken: endless recursion from 'config'`
`internalAstError` at the existing aggregate configuration construction. This
is a checker/parser boundary, not a compiler or production-runtime failure.
The changed gate was therefore the system-first fresh `unit-tests` build plus
the C++ `Spec182EpochText`, `Spec182StreamAcceptance`, and
`Spec182GenerationOptions` selectors. The build completed 190/190 steps and all
three selectors exited 0; durable details are in
[R11-B8 fixture evidence](../specs/182-native-di-python-bindings/evidence/r11-b8-cpp-fixture-20260910.md).
The broad `Spec182*` selector remains outside this bounded result.

The follow-up fresh complete C++ `Spec182*` selector then passed 256 cases and
7077 assertions with exit 0 after the fixture repair. This resolves the recorded
runner-callback fixture boundary for the unit suite; it does not change the
maintained-caller, no-Python, dependency-closure, or T016/T017 status.

## 2026-09-10 — Spec182 R11-B1-PY native binding dependency and ownership boundaries

The first Python extension rebuild was invoked from the wrong working directory and
stopped before configuration (`pythonWrapper/build-nac182` did not exist); the raw
diagnostic is `.codex-tmp/spec182-r11-b8-authority/extension-build.log`. The next
attempt used the host `/usr/local` NAC-ABE prefix, but `ldd -r` exposed the first
native boundary as unresolved `ndn::nacabe::Consumer::clearCache` and related symbols
because that library did not match the current `build-nac182` framework; the raw
diagnostic is `.codex-tmp/spec182-r11-b8-authority/extension-build-2.log`. A subsequent
rebuild against the stale DI library had the same class of unresolved native symbols
(`extension-build-3.log`). The native DI library was rebuilt with the matching
`/home/tianxing/NDN/nac-abe-integration-182/install` dependency, then the extension was
rebuilt and imported successfully (`extension-build-4.log`). These attempts are
dependency/build-boundary failures, not product behavior results; the corrected
ownership boundary and validation are recorded in
[R11-B1-PY evidence](../specs/182-native-di-python-bindings/evidence/r11-b1-py-native-authority-20260910.md).

## 2026-09-10 — Spec182 R11-B6 native replacement validation boundaries

The first replacement harness attempt could not find Provider A's public key because the
encoded PIB key names were compared as raw bytes. The driver stopped with `StopIteration` while
decoding the Name blobs; the raw attempt is retained under
`/tmp/spec182-r11-b6-replacement-1789043701/`. The repair decodes the PIB Name and selects the
candidate URI, then reruns with independent A/B identities.

The next harness attempt reached Provider B's real `attempt-2` execution but asserted a text
form that did not match the JSON log (`"attemptEpoch":"2"` and `attempt-2`). The raw diagnostic
is `.codex-tmp/spec182-r11-b6-build/replacement-2.log`; the assertion now accepts the actual
attempt marker and still requires B grant verification, CPU ONNX execution, and the stream oracle.

The no-backup branch initially expected `NATIVE_STREAM_FAILED`, but the native requester
correctly reports its first boundary as
`NATIVE_REQUEST_STAGE_FAILED boundary=ACK_CLOSED ... DI_NATIVE_NO_ADMITTED_PROVIDER`. The
expected marker was corrected in the driver; raw output is
`.codex-tmp/spec182-r11-b6-build/no-backup.log` and the checked negative run is recorded in
[R11-B6 evidence](../specs/182-native-di-python-bindings/evidence/r11-b6-native-replacement-20260910.md).

The broad C++ `Spec182*` selector remains red in six existing sampling/epoch-text fixtures. All
six throw `std::invalid_argument: NativeProviderRuntime requires a runner preparation callback`
before the relevant assertions; raw output is `.codex-tmp/spec182-r11-b6-build/unit-selector.log`.
The focused `Spec182StreamAcceptance` selector and the
`Spec170NdnsfDiCoreFlow/Spec182*` integration selector pass, so this is preserved as a fixture
boundary rather than attributed to native replacement.

## 2026-09-10 — Spec182 R11-B5 Provider restart recovery boundary

After the first C++ `FULL_CONTEXT` turn persisted the requester checkpoint, the Provider was
terminated with SIGKILL and restarted from the same configuration. The restarted Provider had no
durable KV state and rejected the resumed role with `PROVIDER_CONVERSATION_STATE_MISSING`; the
requester returned `NATIVE_STREAM_FAILED` without a second success/checkpoint. The restart log
contained no execution-evidence or stream-event marker, so no duplicate prefix was executed or
published. The first harness attempt exposed an overly narrow expected error marker and was
repaired before the checked run. Raw output and the durable result are recorded in
[R11-B5 evidence](../specs/182-native-di-python-bindings/evidence/r11-b5-native-recovery-20260910.md).
This is the designed safe-rejection boundary, not proof of durable Provider KV recovery or whole
Spec qualification.

## 2026-09-10 — Spec182 R11-B4 repeated-request artifact identity boundary

The fresh-generation retry completed the first native continuation turn and accepted the
second turn's new generation identity, then Provider assembly stopped at
`DI_CANONICAL_ROOT_DIGEST_MISMATCH`. The requester publisher reused one stable
`assignedArtifact` name across requests while each canonical root carried a request-scoped
source publication name; Provider's process-wide artifact cache consequently returned the
first root for the second request. Raw output is retained under
`/tmp/spec182-r11-b4-fresh-generation/`. The C++ publisher now includes the canonical
manifest digest in the stable identity. A fresh rebuild and rerun completed both
`FULL_CONTEXT` and new-generation `APPEND_DELTA` under `/tmp/spec182-r11-b4-final-checked/`; the bounded resolution is recorded in
[R11-B4 evidence](../specs/182-native-di-python-bindings/evidence/r11-b4-native-continuation-20260910.md).
This was a native cross-request binding boundary, not a Python wrapper or qualification result.

## 2026-09-10 — Spec182 R11-B4 continuation fresh-state identity boundary

The Provider control replay repair allowed the first C++ `FULL_CONTEXT` turn to complete,
persist its native checkpoint, and emit the stream oracle/success markers. The second
process then failed at the native coordinator contract
`native conversation continuation requires a fresh stateful request` because the fixture
reused the first turn's `generationId`. The raw run is retained under
`/tmp/spec182-r11-b4-ackfix/`. The retry must create a distinct second request/generation
identity while retaining the committed parent checkpoint; this is a native harness
contract boundary, not a Python binding or qualification result.

## 2026-09-10 — Spec182 R11-B4 conversation COMMIT acknowledgement boundary

The traced C++ continuation process received stream cursors `1..8` and terminal cursor
`9`, entered the conversation commit phase, and showed the Provider decrypting the valid
requester `COMMIT` control. The requester then failed with
`NATIVE_CONVERSATION_COMMIT_ACK_INCOMPLETE`. Provider output contains a rapidly growing
sequence of duplicate `/ndnsf-di/conversation/commit` publications because its polling
loop reprocessed historical controls. Raw output is retained under
`/tmp/spec182-r11-b4-trace/`. The next repair is a C++ per-sequence control replay guard,
followed by a fresh native rebuild and the same two-round continuation/wrong-parent run.
This remains an integration boundary and is not a Python binding or qualification result.

## 2026-09-10 — Spec182 R11-B4 continuation completion boundary

The terminal-role checkpoint finalization repair executed the additional state-only ONNX
Runtime epoch and the Provider emitted stream cursors `1..8` plus cursor `9`. The requester
accepted the encrypted Response but stayed pending until its 30-second request budget expired;
no checkpoint or success marker was written. Raw output is retained under
`/tmp/spec182-r11-b4-finalize/`. The next retry must distinguish a missing terminal stream
cursor from a missing conversation COMMIT acknowledgement, using C++ trace only. This remains
an integration boundary and is not a Python binding or qualification result.

## 2026-09-10 — Spec182 R11-B4 continuation lineage boundary

After the deferred decode-state promotion repair, a fresh independent C++ continuation
probe reached real Provider ONNX execution and published an authenticated receipt. The
requester then stopped at `conversation receipt lineage mismatch`: the Provider identity
included input prefix token `[3]`, while the fixture's FULL_CONTEXT canonical prefix was
empty. The raw run is retained under `/tmp/spec182-r11-b3-probe-vyoahjfs/`. The fixture is
being corrected to declare the exact input prefix before the next retry. This remains a
development boundary and is not a Python binding or qualification result.

## 2026-09-10 — Spec182 R11-B4 continuation prefix-count boundary

The corrected FULL_CONTEXT prefix `[3]` allowed the independent C++ process to reach the
receipt scope checks, but the requester rejected `prefixTokenCount`: Provider receipt and
checkpoint values differ despite the prefix digest being accepted. Raw output is retained
under `/tmp/spec182-r11-b4-probe-current/`. A temporary diagnostic will expose the exact
wire values before the native count contract is repaired. This is still a C++ continuation
boundary, not a Python binding or qualification result.

## 2026-09-10 — Spec182 R11-B4 deferred decode-state promotion boundary

The first independent C++ continuation probe reached real Provider ONNX execution, then
failed at `PROVIDER_CONVERSATION_PROMOTION_STAGE_FAILED`. The streamed requester observed
the resulting gap timeout and no conversation journal checkpoint was committed. The first
boundary was a C++ ownership mismatch: conversation turns defer decode-state commit, while
the CPU promotion branch only searched committed state; candidate-only cleanup also did not
match the existing erase key. Raw output is retained under
`/tmp/spec182-r11-b3-probe-9fwp2_mg/`. The retry adds exact candidate lookup and candidate-aware
cleanup, then must rebuild and rerun the C++ two-round continuation plus wrong-parent negative.
This is not evidence against the Python binding, and no qualification pass is claimed.

## 2026-09-10 — Proposal local checkpoint hook boundary

The document checkpoint was rejected by the pre-commit hook's full-index assistant
reference scan. The hook and previous records explicitly support
`NDNSF_LOCAL_CHECKPOINT=1` for local checkpoints; retry uses that entry and retains
the prohibited-path check. No hook is changed and no remote operation is requested.
Diagnostic: `.codex-tmp/proposal-two-designs-20260910/checkpoint-hook.log`.
Scope and checks: [proposal audit](PAPER/proposal-defense/research-revision-audit.md).

## 2026-09-10 — Proposal PPTX relative-path resolution boundary

The two-design authorization revision compiled successfully, but PPTX conversion
stopped at `pdftohtml`: the converter resolves input paths relative to its own
slides directory, so a repository-relative `--pdf` duplicated that directory.
No PPTX was generated. The retry uses absolute PDF and notes paths and a fresh
private build directory; no product runtime was involved. Raw failure:
`.codex-tmp/proposal-two-designs-20260910/pptx-build.log`.
Durable follow-up: [proposal audit](PAPER/proposal-defense/research-revision-audit.md).
Resolved with absolute input paths: `pptx-retry.log` records 791/791 source spans
assigned once and 38 notes pages; LibreOffice re-export and rendered review passed.

## 2026-09-10 — Spec182 R10-B84 native request-scope wire compatibility boundary

After adding a fresh per-client owner scope to the production C++ request identity, the
shared-library `Spec170NdnsfDiCoreFlow/Spec182*` selector rebuilt successfully but exited
`201`: all nine C++ cases received an ACK and then stopped before collaboration (`collaborationCalls=0`),
with stream gaps, unary timeouts, or `DI_NATIVE_NO_ADMITTED_PROVIDER`. The raw output is retained at
`.codex-tmp/spec182-r10-b84b-request-id-integration.log`. The C++ unit identity selector still passed,
so this is a native Core/Provider name-contract regression, not evidence of Python binding failure or
qualification. The next retry must preserve the failure, locate the first parser/filter boundary, and
prove the repaired request identity through the same C++ integration selector.

## 2026-09-10 — Spec182 R10-B83 integration target selection boundary

The first post-loader integration rebuild used the current Waf cache's default output tree and
stopped immediately with `Could not find a task generator for the name 'integration-tests'`
(exit `1`). No compiler or test task ran; the selected cache was the old
`.codex-tmp/spec182-r10-b78-provider/build` tree. Re-running with the explicit current output
directory `.codex-tmp/spec182-r4-b2/build` rebuilt all 118 integration tasks successfully, and
the native `Spec170NdnsfDiCoreFlow/Spec182*` selection passed 9 C++ cases with no errors. The raw
failed command and successful retry are retained under
`.codex-tmp/spec182-r10-b83-conversation-loader/`; this was a Waf output/cache boundary, not a
native protocol failure.

## 2026-09-09 — Proposal rendering dependency boundary

The two-reason authorization revision compiled successfully, but the first rendered
review stopped at `import fitz` (`ModuleNotFoundError`) before opening any PDF.
A temporary pip environment could not resolve its package host; that install was
stopped. The previous document run retained CPython 3.8 packages under
`.codex-tmp/proposal-research-revision-20260909/pydeps`. A Python 3.10 import probe
failed on the incompatible lxml extension; system Python 3.8 plus a command-local
PYTHONPATH imported all document dependencies and completed rendering and PPTX generation.
This was a document-tool prerequisite, not a native build or protocol failure.
See [revision audit](PAPER/proposal-defense/research-revision-audit.md) and
`.codex-tmp/proposal-two-reasons-20260909/render-dependencies.log`, `render.log`,
and `pptx-build.log`. No product dependency or qualification status was changed.

## 2026-09-09 — Spec182 R10-B72 Provider plan service-selection boundary

The first metadata-only Provider `--check-only` probe used the executable default service
`/AI/YOLO/2x2Inference` while the reused four-role bundle plan declares
`/Inference/NativeTracer`; the parser stopped before role registration with
`native execution plan has no service` (exit `2`). The raw output is retained under
`.codex-tmp/spec182-r10-b72-provider-check-20260909/check-only.log`. Supplying the declared
service name reached `NDNSF_DI_NATIVE_PROVIDER_PLAN_READY` and
`NDNSF_DI_NATIVE_PROVIDER_CHECK_OK` (exit `0`). This is a command/configuration boundary,
not a Provider protocol or qualification result; see
[R10-B72 evidence](../specs/182-native-di-python-bindings/evidence/r10-b72-provider-plan-check-20260909.md).

## 2026-09-09 — Spec182 R10-B70 local checkpoint hook boundary

The first checkpoint commit attempt for the R10-B68 evidence was rejected before commit by
the repository pre-commit hook's full-index development-assistant text scan. The reported
matches were pre-existing `.specify/memory` references to `AGENTS/CLAUDE`; no R10-B68 file or
product source was implicated. The hook explicitly supports `NDNSF_LOCAL_CHECKPOINT=1` for
local checkpoints, so the same three explicit Spec paths were staged and committed with that
flag. This is a repository hook boundary, not a product, protocol, or qualification result;
the raw terminal output remains in the session and the retry checkpoint is
`13d0c1b5`.

## 2026-09-09 — Spec182 R10-B67 Python extension loader boundary

The first post-mapping extension test invocation loaded `/usr/local/lib/libnac-abe.so`
through the candidate build tree and stopped before any Python test body with the missing
`ndn::nacabe::Consumer::clearCache` symbol (exit `1`). Re-running with the explicit
`/home/tianxing/NDN/nac-abe-integration-182/install/lib` runtime prefix reached the candidate
extension, then stopped before test execution because the stale
`.codex-tmp/spec182-r4-b2/build/libndnsf-distributed-inference.so` did not yet export
`NativeInferenceHandle::applicationRequestId()` (exit `1`). No native request, Core packet,
or Qwen behavior was observed. Raw output and return code are retained under
`.codex-tmp/spec182-r10-b67-binding-boundary-20260909/`.

The next retry gate is to rebuild the candidate `ndnsf-distributed-inference` shared library
from the same source checkpoint as `unit-tests`, then run `ldd`/`nm` with the explicit NAC-ABE
prefix before interpreting Python binding results. This is a dependency/source-closure issue,
not a protocol result.

## 2026-09-09 — Spec182 R10-B47 PO-001 owner/runner retry boundaries and pass

The first root-enabled retry of the T016 owner reached MiniNDN setup but failed before a
business process because the command-local system-first `PATH` omitted `/usr/local/bin/infoconv`;
the owner recorded `MININDN_OWNER_FAILED:JSONDecodeError` in
`.codex-tmp/spec182-t016-r10-b46-owner5`'s predecessor run. Adding the installed runtime tool
path exposed the next runner contract boundary: the historical PO-001 manifest used process role
`native-di-integration`, which is outside the runner's declared role vocabulary. A corrected
transient manifest bound the same process to role `requester`, then failed its artifact digest
preflight because the current incremental `integration-tests` binary had changed.

Those failures are preserved in the fresh run directories
`.codex-tmp/spec182-t016-r10-b46-owner/`, `owner3/`, and `owner4/`; none was classified as a
protocol result. After recomputing only the executable artifact hash and retaining the explicit
role binding, `sudo -n env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin python3
Experiments/NDNSF_DI_NativeClosure_Minindn.py ... --execute-owner` completed PO-001 in
`.codex-tmp/spec182-t016-r10-b46-owner5/`: owner namespaces and NFD sockets were live, the
staged native process returned `0` in `8001ms`, the business marker
`SPEC182_NATIVE_DI_REQUEST_RESULT_OK` was present, and the collector reported complete identity,
process-tree, namespace, exec-map, endpoint, business-oracle and cleanup evidence with no
integrity/policy violations.

This is a real isolated native-process PO-001 observation for the existing in-process
requester/Provider fixture. It does not prove independent requester/Provider transport, I02-I08,
PO-002-PO-014, maintained caller/no-Python migration or full T016 qualification; those remain
open. See [R10-B47 evidence](../specs/182-native-di-python-bindings/evidence/r10-b47-t016-po001-owner-pass-20260909.md).

## 2026-09-09 — Spec182 R10-B26 missing `REPO_REF` negative recheck

The old R10-B6 missing-object failure was a test-boundary observation: the fixture's fixed
three-second pump ended before the production fetch exhausted its default 30-second budget. The
current source already scopes `NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS=1000` with RAII for this
negative case. A fresh run of
`.codex-tmp/spec182-r4-b2/build/integration-tests --run_test=Spec170NativePostSelection/ProductionIngressRejectsMissingNativeRepositoryReference`
entered the real Provider handler and exited `0` in 1.946850 seconds with the expected fetch
failure, no runner input, no successful Response, and no test errors. The raw retry is retained
under `.codex-tmp/spec182-r10-b26-missing-repo-ref-20260909/`.

This closes the local missing-object negative recheck while preserving the original R10-B6
failure as history. Cross-process transport, maintained callers and T016 qualification remain
unobserved.

## 2026-09-09 — Spec182 R10-B18 runner dynamic-ELF and trace-integrity boundaries

Three exploratory runner probes exposed sequential harness boundaries before the final retry:

- `.codex-tmp/spec182-runner-probe-20260909051337/` exited before bwrap because the minimal
  runner environment had no `PATH` and the manifest used relative `bwrap`/`strace` names.
- `.codex-tmp/spec182-runner-probe2-20260909051429/` reached bwrap but dynamic `/bin/true`
  failed `execve` with `ENOENT`; loader and libc were staged only below `/probe-root`, while ELF
  absolute interpreter/DT_NEEDED paths were not mounted.
- `.codex-tmp/spec182-runner-probe3-20260909051640/` executed `/bin/true` with return code 0,
  but the collector marked normal strace `<unfinished ...>`/`<... resumed>` pairs as
  `TRACE_UNPAIRED`, producing `UNQUALIFIED` before evaluation.

These are runner tool/observation boundaries, not DI protocol results. The retained retry
`.codex-tmp/spec182-runner-probe4-20260909051727/` passed dynamic execution (`returncode=0`) and
reported a complete trace after absolute shared-library mounts and per-PID unfinished/resumed
pairing were repaired. The result remains `UNQUALIFIED` because this probe intentionally lacks
business evidence.

## 2026-09-09 — Spec182 R10-B17 owner invocation preflight boundary

The first explicit R10-B17 owner invocation used the frozen registration manifest without a
`campaignCase` selector. The owner rejected it with exit `2` / `campaignCase is required` before
MiniNDN, NFD, or any namespace was started. This is an operator invocation boundary, not a
protocol or qualification result.

Raw stdout/stderr are retained as `.codex-tmp/spec182-t016-r10-b17-20260909050906.stdout` and
`.codex-tmp/spec182-t016-r10-b17-20260909050906.stderr`; the output directory contains only the
explicit `UNQUALIFIED` result. The retry must use a fresh manifest copy with `campaignCase=I01`.

## 2026-09-09 — Spec182 T016 MiniNDN owner preflight recheck

A fresh T016 campaign preflight was run after starting the local NFD. The socket check and
`nfdc status report` both succeeded (`/run/nfd/nfd.sock`, NFD 24.07-14-g2b43d675), but
`Experiments/NDNSF_DI_NativeClosure_Minindn.py` still exited `2` before starting any business
process or namespace run with `MININDN_NODE_CONTEXT_NOT_PROVIDED` for `campaignCase=I01`.
`ip netns list` contained no campaign namespace, and the owner supplied no node/netns metadata
for `tests/standalone/run-spec182-native-closure.py`; therefore this is an owner-preflight
boundary, not a protocol, requester, Provider, or qualification result.

The complete raw retry is retained under `.codex-tmp/spec182-t016-r5/`, with the NFD startup
record under `.codex-tmp/spec182-t016-preflight-20260909/`. NFD availability changes the prior
socket blocker, but does not close the missing MiniNDN node-context requirement. T016 remains
`UNQUALIFIED` and must be retried only after a real isolated node/netns/socket context is created.

## 2026-09-09 — Spec182 full integration stale recipe-oracle boundary

The first full `integration-tests --log_level=test_suite` run after the R10-B11 checkpoint
completed the suite but exited `201` with four failures in
`Spec175NativeAssembly/{AssignmentBoundRootSourceAndCachePath,RegisteredOneProviderAssemblyLoadsOrt,RegisteredTwoProviderAssemblyLoadsOrt,RegisteredFourProviderAssemblyLoadsOrt}`.
Each failed before ORT execution with `DI_NATIVE_ONNX_RECIPE`: the integration fixture's
`recipeDigestFor` helper sorted input/output names while the production
`canonicalNativeOnnxRecipeJson` binds those names to contract order. This is a stale test
oracle boundary, not a protocol result. The raw complete run and vmstat are retained under
`.codex-tmp/spec182-t016-r3/`; the unit suite in the same checkpoint passed.

The helper was corrected to preserve contract order and the stale graph digest/adapter identity
in the same fixture was aligned with the current source identity. A system-first `-j2` rebuild
completed successfully and the affected `Spec175NativeAssembly/*` suite passed 7/7; the complete
integration suite is still a separate retry. The lower concurrency was selected because the full
unit/integration attempts produced sustained swap-in on this host.

## 2026-09-09 — Spec182 R10-B9 requester REPO_REF oracle boundary

The first R10-B9 repository-reference selector failed in its test oracle: the
observer built a `std::string` from `begin()` and `end()` iterators obtained from
two temporary `RequestMessage` payload buffers.  That undefined test behavior
first appeared as a missing observation and then as an empty-JSON planning
error, even though the request wire had been produced.  The raw attempts are
retained under `.codex-tmp/spec182-r10-b9/` (`selector-r2.log` and
`selector-r3.log`).  The oracle now copies one payload buffer before inspecting
it; the existing inline selector and the new `REPO_REF` selector both pass.

A separate comma-separated Boost.Test filter attempt returned the setup error
“no test cases matching filter”; it was a selector syntax boundary, not a
product or protocol result.  The final evidence uses separate selector
commands and records the corrected test-harness boundary.

## 2026-09-09 — Spec182 R10-B2 facade test-double boundary

The first R10-B2 focused Python run failed in the new native reference facade
test because its fake `NativeApplicationInput` did not model the C++ class's
default empty `payload` and `options` fields. This was a test-harness boundary,
not a native or protocol failure. The fake was corrected, the same suite was
rerun with 17 passing cases, and the raw retry output is retained at
`.codex-tmp/spec182-r10-b2/app_sdk.log`; the batch evidence records the first
failure boundary and the changed test-double check.

## 2026-09-09 — Proposal shared bibliography build boundary

The first EN/CH research-revision build reached BibTeX but failed to resolve
`./research-references.bib` from the isolated output directory. No protocol or
experiment was run. The source now uses an unqualified bibliography basename;
the alternate language build directories link to the same shared bibliography.
Initial logs are retained at `.codex-tmp/proposal-research-revision-20260909/en-build.log`
and `ch-build.log`; later attempts use separately named logs. This document-only
boundary is tracked in [the revision audit](PAPER/proposal-defense/research-revision-audit.md).

The EN/CH second build passed. A later multi-hunk wording patch was rejected
because a paragraph fragment was supplied as a whole-line match; it changed
nothing and was reapplied with the exact complete table row. A broad read-only
result-file discovery also hit protected runtime-state directories; no permission
changes were made. The audit uses named evidence paths, and marks the older DI
raw-run provenance as not re-established rather than assuming a result from that search.

The initial `review_rendered.py` invocation failed at import because the default
Python lacked PyMuPDF (`fitz`). This is a rendering-tool environment boundary;
the PDF build had succeeded. The dependency-verified interpreter is used for
rendering and PPTX generation; no research result is inferred from this check.

The first PPTX export was stopped by the generator's approved-build-root guard
before conversion. The supplied `.codex-tmp/.../ndnsf-pptx-build` path was not
an allowed reset target; no output was cleared. Retry uses a newly allocated
`/tmp/ndnsf-*` build directory. The first visual review also caught an obsolete
three-seed chart selected for a ten-seed caption; it is replaced with a table
derived from the exact ten-seed aggregates before delivery.

The second PPTX invocation reached the next safety guard: an existing `mktemp`
directory lacks the generator's ownership marker. It was not cleared. The next
invocation uses a nonexistent `ndnsf-build` child inside that dedicated temporary
directory, allowing the generator to establish its own marker safely.

The third PPTX attempt completed conversion but failed before publication because
the existing notes parser did not accept generated one-line `slideentry` blocks.
The generated notes are adjusted to the parser's documented multiline form;
the protected staged output was not published as a completed deck.

Presenter-notes PDF compilation then exposed PDF-extracted Unicode ligatures and
the mathematical ell character that pdfLaTeX did not accept. The generator now
normalizes ligatures and spells out ell in notes. This affects derived presenter
text only, not the source slide formula or experimental values. Original failure
log: `.codex-tmp/proposal-research-revision-20260909/notes-build.log`.

## 2026-09-09 — Official defense criteria document extraction

Web DOCX parsing was unsupported; urllib plus ZIP/XML extraction succeeded.
A supplementary HTML extraction failed before fetching because `bs4` was absent.
The standard-library HTMLParser fallback succeeded without installing dependencies.
Graduate Catalog retrieval still produced no valid requirements text (web 403);
the report explicitly leaves the complete catalog rules unverified.
A progress-record patch with an unmatched failure-log context was rejected without
changes; after reading the exact lines, the corrected patch was applied separately.
This is a document-tool boundary, not research or protocol evidence. See
`.codex-tmp/proposal-reviewed-20260909/official-criteria/extraction-boundary.md` and
[source-based review](PAPER/proposal-defense/review.md).

## 2026-09-08 — Spec182 R7-B1 replacement expectation boundary

The first `Spec182R4B6RealProviderConversationReplacement` run built successfully but exited
201 at the assertion that a one-Provider fixture would complete a successful replacement. Its
first runtime boundary was the recovery `ACK_CLOSED` planning step:
`NATIVE_REQUEST_STAGE_FAILED` with cause `DI_NATIVE_NO_ADMITTED_PROVIDER`; the failed Provider
was excluded from the recovery admission set, so no alternate Provider could be selected.
This was a test expectation boundary, not permission to retry the same Provider or publish a
partial checkpoint. The raw failure is retained at
`.codex-tmp/spec182-r7-b1-r4b6-replacement-20260908/replacement.log` with
`replacement.rc=201`.

The test was corrected to assert the contract's bounded single-Provider negative: one Provider
collaboration call, two ACK calls, `NATIVE_REQUEST_STAGE_FAILED`/`ACK_CLOSED`, and no conversation
checkpoint. The final selector passed. A successful alternate-Provider recovery still needs a
separate multi-Provider harness and remains open under T011-C/T016. See
[R7-B1 evidence](../specs/182-native-di-python-bindings/evidence/r7-b1-r4b6-replacement-20260908.md).

## Proposal authorization documentation patch validation

首次 apply_patch 因同一 patch 重复声明 main.tex 被结构验证拒绝，未写入目标文件。
将同一文件的修改合并为一个操作后重试；不是构建、测试或协议失败。
见 [revision evidence](PAPER/proposal-defense/authorization-revision.md#editing-boundary)。

正文首次 latexmk exit0 但新增文献引用仍 undefined，不能视为通过；
重试使用独立辅助目录和禁用旧 latexmk 配置，日志及原因见上述 revision evidence。

## 2026-09-08 — Authorization comparison documentation checkpoint

论文比较文档首次 `git add` 被既有 `docs/*` ignore 规则拒绝，随后仅对用户要求的
`authorization-design-comparison.md` 使用显式 `git add -f`。首次 commit 被 pre-commit
扫描全索引中的历史助手引用拒绝；边界为提交钩子，不是文档检查失败。
核对 `.git/hooks/pre-commit` 后采用既有 `NDNSF_LOCAL_CHECKPOINT=1` 本地模式，
仍执行禁止路径检查。[材料与核对记录](PAPER/named-data-network-service-framework-paper/authorization-design-comparison.md#review-and-validation-boundary)。

## 2026-09-08 — Spec182 R5-B7 placement selector transient SIGSEGV

R5-B7 首次运行完整 `Spec182V3Placement/*` 选择器时，测试进程在
`PublicClientCommitsSignedOfferAndIgnoresLateTerminalCallbacks`、
`tests/unit-tests/di-native-v3-placement.t.cpp:1026` 处发生 SIGSEGV，约 0.85s
后 exit201。首个边界是测试进程内存访问错误；没有进入可据此判断的 Provider、协议或
qualification 结果。原始输出保留在
[r0 placement log](../.codex-tmp/spec182-r5-b7-placement-suite-20260908-r0/placement.log)。

随后隔离该 selector（r1）和再次运行完整 9-case selector（r2）均 exit0、无错误；记录见
`.codex-tmp/spec182-r5-b7-placement-suite-20260908-r1/` 与
`.codex-tmp/spec182-r5-b7-placement-suite-20260908-r2/`。因此本次按瞬时测试边界保留，
不将重试通过升级为真实 Provider 或跨进程资格结论；若再次出现，需先比较新的原始运行
目录和首个失败边界，再决定是否修复测试或产品代码。

## 2026-09-08 — Spec182 R4-B5 public conversation build invocation

首次重建命令将 `PATH=...` 置于 `/usr/bin/time` 后，`time` 把它当作待执行程序，
在编译器启动前返回 `exit127`。原始 [r2日志](../.codex-tmp/spec182-r4-b5-public/build-r2.log)
保留；这是执行器命令语法边界，不是源码编译或测试结果。重试使用 command-local
PATH 并保持 `-j4`。

## 2026-09-08 — Spec182 integration source closure and grant publication freshness

集成目标首次重新链接在源文件编译完成后 exit1：`tests/wscript` 的手工
`di_integration_sources` 漏掉会话、请求准备、request envelope、catalog 和授权等当前
DI core TU，首个边界是链接器 undefined reference，不是协议或 Provider 运行结果。
原始日志见 [build-integration-r2.log](../.codex-tmp/spec182-r4-b4-current/build-integration-r2.log)。
改为与生产库相同的 DI core/adapters glob 后，`integration-tests -j4` exit0（52.565s）。

随后 `Spec182GrantClientFlow/*` 首次运行 exit201：Core 发布器传给
`publishSignedAppData` 的 ndn-cxx 默认 freshness 为 0，被已有正 freshness 门拒绝；首个
运行边界是 `signed APP Data freshness must be positive`，不是签名或解包失败。原始日志见
[integration-spec182-grant.log](../.codex-tmp/spec182-r4-b4-current/integration-spec182-grant.log)。
固定授权发布 freshness 为 60000ms 后，DI 集成目标增量构建 exit0（20.599s），
`Spec182GrantClientFlow/*` 2 cases exit0；结果见
[integration-spec182-grant-r2.log](../.codex-tmp/spec182-r4-b4-current/integration-spec182-grant-r2.log)。
随后加入正 freshness 回归断言，增量构建再次 exit0（20.546s），同一 2 cases 再次 exit0，
详见 [integration-spec182-grant-r3.log](../.codex-tmp/spec182-r4-b4-current/integration-spec182-grant-r3.log)。
当前集成目标没有公开两轮会话选择器，因此 T011-C/T016 仍保持 `PARTIAL`。

## 2026-09-08 — Spec182 R4-B4 conversation Unicode canonical boundary

首次R4-B4增量构建使用旧`-j2`并成功完成；随后`Spec182Conversation*`运行9 cases，
8项失败。首个代码边界是`NativeCanonicalJson`使用`ensure_ascii=true`，与旧Python
checkpoint/transcript的`ensure_ascii=False`不一致；中文service wire先被canonical
检查拒绝，receipt digest随后连带失败。该次不是Provider网络或协议资格结果。
原始选择器输出见本次会话命令结果；修复已改为保留UTF-8，需按主机默认`-j4`重建并复验。
详细进度见[R4-B4](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md)。
修复后`Spec182Conversation*`按`-j4`增量检查，9 cases/exit0；该结果只覆盖会话组件focused边界。
随后共享回归`Spec182CanonicalJson*`、`Spec182Conversation*`、`Spec182EpochText*`、
`Spec182StreamAcceptance*`、`Spec182Sampling*`共25 cases按`-j4`运行exit0。
移除未使用的会话 ready 常量后按同一 system toolchain/`-j4` 重建 DI shared library exit0，
再运行共享25 cases与`Spec182ProviderHost*` 6 cases均exit0；这些仍是本地 focused/regression
结果，不代表真实两轮跨进程或 T016 qualification。

## 2026-09-08 — Spec182 progress audit checkpoint hook

审计文档checkpoint首次提交exit1：pre-commit扫描全索引中的既有助手引用。
已检查钩子提供的`NDNSF_LOCAL_CHECKPOINT=1`本地模式，仍保留禁止路径检查；
这是提交边界，不是产品测试失败。首边界节录保存在
[commit-failure.txt](../.codex-tmp/spec182-progress-audit-20260908/commit-failure.txt)，
审计结论见[R4-B4](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md#progress-and-feasibility-audit)。

## 2026-09-08 — Spec182 conversation oracle canonical mutation

R4-B4 离线oracle author首次 exit1：负例使用默认 json.dumps，旧参考先以
ConversationCheckpointInvalid("checkpoint is not canonical")拒绝，未到摘要篡改检查。
改用参考 canonical payload 后篡改字段，接收该参考的两类拒绝异常；不是原生运行失败。
详细边界及重试结果见[R4-B4](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md)。
修正后生成/确定性重现及每例错key/篡改检查PASS，author边界RESOLVED。

## 2026-09-08 — Spec182 epoch text test optional payload

R4-B3 增量构建 exit1：新增 C++ 测试直接 parse optional finalPayload，类型不匹配。
生产 coordinator 编译完成，测试尚未执行。增加 has_value 必需断言后解引用；
保留[原始编译日志](../.codex-tmp/spec182-r4-b3/build.log)，同树增量续建到 r2 日志。
进度见[R4-B3](../specs/182-native-di-python-bindings/evidence/r4-b3-epoch-text-20260908.md)。
修复后 unit/DI库增量构建及24 cases/411 assertions PASS；本地测试类型问题 RESOLVED。

## 2026-09-08 — Spec182 CLI probe output-path correction

R4-B2 CLI smoke首次使用build/DI_NativeRequester，实际Waf输出为build/examples/DI_NativeRequester；
探针在进程创建前FileNotFoundError，未执行产品。按build.log Linking行修正探针路径，
不修改或重编代码。原始[路径诊断](../.codex-tmp/spec182-r4-b2-r3/cli-path-error.json)；
续检查结果见R4-B2的cli-record.json。本条不改变已通过的C++测试结果。
修正后CLI help/ldd exit0，实际加载本次DI库，local probe RESOLVED。

## 2026-09-08 — Spec182 stream replacement fixture topology identity

R4-B2新ABI build PASS/879.773s，generation options 2 cases/21 assertions PASS；
stream 5/7 cases、104/106 assertions PASS，两个replacement case在构造Provider B
测试offer时抛topology Provider mismatch。fixture仅改了offer.provider，嵌套topology
仍指向Provider A；此边界发生在新offer签名前，不是Provider重算或恢复协议失败。
保留[stream原始日志](../.codex-tmp/spec182-r4-b2/stream.log)和同目录build/options/stream
record。下一步修fixture绑定后增量编译并重试两个失败case，不放宽生产身份校验。
进度见[R4-B2](../specs/182-native-di-python-bindings/evidence/r4-b2-stream-production-20260908.md)。
r2修复及r3补验均PASS；最终stream 7/7 cases/190 assertions，两个SDK恢复wire PASS，
本地fixture问题RESOLVED，不扩大为真实Provider重算资格。

## 2026-09-08 — Spec182 stale dependent projection oracle

r5 build PASS /30.971s；共享测试130/131通过、2963/2975断言通过，1个投影 oracle
用例12断言失败，无 SIGSEGV。placement-v3 oracle 已改为 intent identity，但依赖它的
projection-oracle.json 未重新生成。核对既有 SDK author 从 placement core_digest
独立计算 plan/dataflow 后重生成；不从 C++ 输出抄期望值，不重编无源码变化的二进制。
[原始失败](../.codex-tmp/spec182-r3-b1-r5/focused.log)，单例重试与后续 oracle 见 R3-B1。
后续 r6 失败单例282/282断言PASS；累计131 DI/13 Core cases及三个独立oracle已通过。
本地批次修复结果见 [Final Local Result](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md#final-local-result)；不授予T016或真实网络资格。

## 2026-09-08 — Spec182 client fixture task capability

r4 build PASS /46.092s；隔离 client 空 ACK/取消例 exit201，requestWire 报
`NATIVE_REQUEST_CONTRACT_INVALID`。fixture adapter 明确只声明 `task`，新用例却请求
`inference`，被正确拒绝。修用例 task 字段，不放宽生产 capability 检查。原始
[诊断](../.codex-tmp/spec182-r3-b1-r4/client-diagnostic.log)；同见 R3-B1。

## 2026-09-08 — Spec182 request projection identity and lifecycle tests

R3-B1 r3 build PASS /126.721s；首轮 focused process 在5.548s 后 SIGSEGV。
此前已有明确失败：NativeMerge 签名 fixture 的旧 model identity 被拒绝，projection builder
仍比较 source content 与新 intent digest，两个新 client 用例提前终止。不能把末尾崩溃
归为某个已通过协议结果。原始 [focused log](../.codex-tmp/spec182-r3-b1-r3/focused.log)
与 focused-record.json 保留；先修正 identity consumer/fixture，并隔离重跑客户端失败
取得结构化原因。检查测试异步回调捕获所有权，避免 fatal assert 后悬空 fixture。
见 [R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。

## 2026-09-08 — Spec182 client fixture crypto declarations

R3-B1 r2 build exit 1 / 232.870s：client 新测试直接调用 EVP，却只间接获得 EVP_PKEY
前置声明，缺 `<openssl/evp.h>`。另在测试前静态发现同例旧 adapter.inspect 返回空模型，
无法抵达 Core；改为真实 NativeCatalogModelAdapter。保留
[r2 log](../.codex-tmp/spec182-r3-b1-r2/build.log)，修复后同树增量 r3。无测试结果。
同见 [R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。

## 2026-09-08 — Spec182 requester lifecycle test options

R3-B1 fresh build exit 1 / 506.101s，首个编译错误位于新增 V3 lifecycle test：
`NativeInferenceClient::request` 要求显式 options，第五个参数遗漏；同类 client
空 ACK/取消用例也需修正。原始 [build log](../.codex-tmp/spec182-r3-b1/build.log)
与 [record](../.codex-tmp/spec182-r3-b1/build-record.json) 保留。未链接完成、未运行测试。
修复后在同一构建树增量续建，新日志使用 r2；详见
[R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。

## 2026-09-08 — Spec182 published manifest issuer binding

CLI/bootstrap 静态审查发现：canonical publication 产生新 manifest，而固定 issuer
allowlist 仅含原 catalog manifest，会在真实 grant 请求处拒绝。已补显式受信 source
policy，经 publication hash 与 model/source/initializer/profile 绑定后签发新 manifest
grant；不扩充任意 manifest 权限。源码与负例未执行，验收保持 PARTIAL；见
[R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。

## 2026-09-08 — Spec182 requester Core buffer conversion

R3-B1 限定 `-fsyntax-only` 首次 exit 1（7.278s）：NativeRequestPlanner 将
std::vector<uint8_t> 直接赋给 ndn::Buffer，两个 Core assignment 字段编译失败。
尚未生成二进制或运行测试。保留
[原始诊断](../.codex-tmp/spec182-r3-b1-syntax/NativeRequestPlanner.log)；
改显式 iterator 拷贝并检查 Begin/Response 同类边界，后续独立 r2 目录重试。
批次状态及命令见 [R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。
后续 r2 四源码语法检查全部 exit 0（27.272s）；只关闭此类型错误，不代表链接或请求验收。

## 2026-09-08 — Spec182 requester wire and sealer identity mismatch

R3-B1 静态接线审计发现 SDK V2 model_identity_hash 为 ModelRef intent digest，
现有 V3 sealCore 却比较 descriptor.contentDigest；不能把兼容请求改为 content hash
绕过此边界。需区分 source content 与 request intent，并同步 artifacts/core 绑定。
同时 Core cancelStreamRequest 不移除 pending collaboration，RL-3 必须补清理接线。
这是代码审计发现，未运行请求、构建或实验；证据与未执行项见
[R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。
后续源码已区分 V3 intent/source content，重生成当前 SDK V3 对照数据；Core 已补
CancelCollaboration 的 pending 清理接口及用例。尚未构建/测试、尚未完成默认 requester
接线，本项保留未验收。

## 2026-09-08 — Spec182 group projection fixture const proposal

R2-B6 首轮 native build exit 1，新增 Provider 交换分配负/正例修改了 fixture 的
const proposal，编译器拒绝 swap/restore；产品两个 cpp 已编译，测试尚未执行。
改为可修改的测试局部 proposal，保留[原始 build](../.codex-tmp/spec182-r2-b6/build.log)，
新 r2 目录增量重试，见 [R2-B6](../specs/182-native-di-python-bindings/evidence/r2-b6-group-projection-20260908.md)。

## 2026-09-08 — Spec182 grant oracle backend argument

R2-B4 native 增量 build 和 37 C++ cases PASS；离线 Python oracle 在 P-256
fixture 的 `ec.derive_private_key` 缺旧版 cryptography 所需 backend 参数时失败，
尚不能授予完整 oracle PASS。保留[首边界](../.codex-tmp/spec182-r2-b4/oracle-error.txt)，
补显式 default_backend 后仅重跑 oracle，不重复 native build；见
[R2-B4](../specs/182-native-di-python-bindings/evidence/r2-b4-grant-production-audit-20260908.md)。

## 2026-09-08 — Spec182 production grant implementation gap

源码审计确认 NativeArtifactPolicyAuthority 仅有 IssuePort 验证包装，未实现 requester
签名/策略/recipient encryption/authority 签名；client 也缺答复认证和真实 Core 发布
生命周期。现有 wrong-recipient 正例只证明转发，不能证明授权。T005-A/B 保持 PARTIAL，
R2-B4 修复批次已登记，见 [源码边界与接续](../specs/182-native-di-python-bindings/evidence/r2-b4-grant-production-audit-20260908.md)。
本项为静态发现的实施缺口，不是运行失败或协议拒绝结果；未启动重建/实验。

## 2026-09-08 — Spec182 state mapping template call

随后 r2 build PASS、70/71 cases PASS。真实 causal ONNX 的 Concat 重复引用同一输入，
source inspector 将 operand 次数照搬为规划消费者次数，触发图重复消费者拒绝。
仅对规划 edge consumer 去重；原始 node inputs、metadata 与 digest 域保留重复次数。
真实源补 2 个 consumer/7 次引用断言，r2 原日志保留，r3 增量验证；同见 R2-B2。

R2-B2 首轮增量构建 exit 1，首边界是 NativeCanonicalRolePreparer::bindStateContracts
泛型 lambda 内 JSON size.get 缺 dependent-template 关键字，尚未进入测试。
补 template 并静态复核，保留原始日志，后续使用独立 r2 目录；见
[R2-B2](../specs/182-native-di-python-bindings/evidence/r2-b2-state-source-binding-20260908.md)。

## 2026-09-08 — Design diagram width

后续 R3 版面已通过，API 门检测两份并发修改的 Tiger supporting docs 与快照
不一致；按现有规则重采样并重建，未放宽源码漂移检查。原始 run 同见下列图解证据。

新增 DI 类图两侧弯曲返回箭头扩大 TikZ 边界，双 PDF 构建成功但版面门发现
overfull 70.85669pt 后退出 1。改为页宽内折线路径；原始构建和后续检查见
[diagram evidence](../specs/182-native-di-python-bindings/evidence/design-diagrams-20260908.md)。

## 2026-09-08 — Spec182 native Merge sealing fixture

R1-B3 incremental build 29.838s PASS，67/68 cases PASS。新 mixed-role fixture 已完成
publication，但调用 V3 sealer 时漏传 inputs.assemblyByRole，准备角色为空导致
DI_NATIVE_ROLE_BINDING_MISMATCH；不是 publication/签名故障。补原始 prepared roles，
不以发布后角色替代验收输入，原始日志见 [R1-B3](../specs/182-native-di-python-bindings/evidence/r1-b3-native-merge-20260908.md)。
r2 build PASS、67/68 cases PASS：已通过重新封存和 grantView，fixture 又将 protected
epoch 配成无 grant 的明文 policy，finalizeSecurity 正确拒绝。补两角色 protected grant
binding fixture，不削弱生产安全检查；下一轮 r3 使用新日志。

## 2026-09-08 — Cleanup Python path compatibility

旧对象清理脚本在删除前检查使用 Path.is_relative_to；系统 Python 3.8 不支持，
因此尚未生成清单或删除对象。改为 resolve().relative_to() 的包含检查后重试；
pip cache purge 已独立成功。清理清单在 .codex-tmp/cleanup-20260908-object-files.json。

## 2026-09-08 — Spec182 role semantics fixture ingress

R1-B2 build PASS，25/26 cases PASS；新 C++ fixture 只填 result egress 未填 input
ingress，candidate.validate 在语义校验之前拒绝。补齐成对入口/出口并在普通候选
场景同时清空；保留原始日志，见 [R1-B2](../specs/182-native-di-python-bindings/evidence/r1-b2-role-semantics-20260908.md)。

## 2026-09-08 — Spec182 shared workflow validation references

工作流集中到共享 skill 后，validate_design 仍在 feature wrapper 查旧标题/诊断
条款，proof-design 也保留旧 anchor，导致结构检查 exit=1。修复规则读取位置和
真实 anchor，保留原门禁并独立重跑 exit=0。该失败与 YOLO 行为验证无关，原始结果
及修复见 [semantic batch evidence](../specs/182-native-di-python-bindings/evidence/t003-yolo-semantic-batch-20260908.md)。

## 2026-09-08 — Skill validator compatibility field mismatch

工作流修订时，skill-creator quick_validate 拒绝已安装 Spec Kit 技能的既有
`compatibility` frontmatter 字段，退出 1；尚未进入工作流语义检查。
保留安装元数据，改用 YAML/必需字段与定向工作流检查；不影响产品资格。
见 [workflow evidence](../specs/182-native-di-python-bindings/evidence/skill-batch-workflow-20260908.md)。

## 2026-09-08 — Spec182 implicit rank publication mismatch

源码审查发现普通候选省略 rank maps 虽可通过 candidate/role validation，
preparation 和实际 canonical publisher 仍用 map.at 读取 degree，会在发布前失败。
统一 implicit rank-one 语义，覆盖 inline/external 与两种 rank 表达；114 cases、
2248 assertions PASS。此为源码契约缺陷修复，不是完整请求或网络资格结果。
见 [publication evidence](../specs/182-native-di-python-bindings/evidence/t003-preparation-rank-20260908.md)。

## 2026-09-08 — Spec182 ONNX graph fixture control

owned graph r1 build PASS，112/114 cases PASS；两个新 case 的 control 缺必需
requireActive，既有 checkActive 在解析前返回 ASSEMBLY_TIMEOUT。此为 fixture 配置失败，
不是实际耗时超限或协议结果。补回调并保留空回调拒绝负例，r1 原样保留，后续见
[ONNX graph inspection](../specs/182-native-di-python-bindings/evidence/t003-onnx-graph-inspection-20260908.md)。
r2 build PASS，113/114 cases PASS；剩余失败为 test reader 读取不存在的 op_type，
维护 GraphNodeView 字段实际为 operation。修正 reader，保留 r2；独立 r3 build 及
114 cases/2212 assertions PASS。

## 2026-09-08 — Design R3 table layout

后续第三轮版面门通过；补可读目标参考后的 pre-build 门又检测到并行 ONNX 源码漂移，
保持拒绝旧基线，核对差异后重采样。该拒绝证明漂移门生效，不是新源代码失败。
第五轮全量扫描期间 ONNX 头文件再变化；新增显式增量扫描与发布前全集 SHA 检查，
仍保留原始解析和完整校验，不以忽略漂移完成文档。

R3 双 PDF 编译成功，版面门拒绝 AC-13 两个带参数方法名在表格列溢出。
修正标识符断行识别并补回归；首轮原始目录保留，不作为产品或最终 PDF PASS。
见 [R3 evidence](../specs/182-native-di-python-bindings/evidence/design-r3-20260908.md)。

最终独立构建 82/87 页、5 项文档工具回归、源码/API/版面 PASS；原失败轮次保留。
内容修订与全量函数语义审计区分，详见证据中的逐章追踪。

## 2026-09-08 — Spec182 Qwen candidate ingress identity

candidate identity r1 build PASS，86/87 cases PASS；完整 Qwen oracle 发现 native
inputIngressRole/resultEgressRole 额外填首末角色，维护 splitter 两字段为空，导致两条
规范字节/摘要断言失败。按实际候选契约保留空值，真实请求的 ingress/终端检查仍归
后续 sealed plan；不能为通过而改 oracle。r1 保留；r2 build 及 87 cases/1246 assertions
PASS，修复与证据见
[candidate identity](../specs/182-native-di-python-bindings/evidence/t003-candidate-identity-20260908.md)。

## 2026-09-08 — Design chapter semantic review

R2 PDF/源码摘要检查曾通过，但逐章可理解性审阅结果 NEEDS_REVISION。
第 53 章生成/KV/会话仅列 cancel；第 59 章混淆 grant 外部单项输入与内部完整策略物化；
目标历史叙述与 TG 冲突。审阅已完成，修订待办，不把文档问题判为产品运行失败。
见 [chapter audit](../specs/182-native-di-python-bindings/evidence/design-chapter-audit-20260908.md)。

## 2026-09-08 — Spec182 resource repair progress owner

文档检查拒绝 T003-C IN_PROGRESS（T003-A/B 依赖未完成）。本轮实际修复 T003-A
共享资源声明，已将执行状态归回该 owner，下游保持 PARTIAL，不越过依赖门。
原始结果及后续验证见 [resource contract](../specs/182-native-di-python-bindings/evidence/t003-resource-contract-20260908.md)。

## 2026-09-07 — Spec182 YOLO fragment source audit

原生 fragment 哈希缺注册摘要与有序节点，且 backend/安全余量/原子 Merge 与维护
splitter 不同。现已修复，实际 Python 对照及 82 cases/1084 assertions PASS；
完整候选身份仍未闭合。此为源码发现，见 [fragment evidence](../specs/182-native-di-python-bindings/evidence/t003-yolo-fragment-20260907.md)。

## 2026-09-07 — Spec182 descriptor build termination

model-descriptor r1 configure 通过；build 进程终态 rc=143，日志停在 74/173，
没有 compiler error 或 Waf 成功记录，现场无残留 waf/cc1plus。终止来源未确认，
不计代码失败或 OOM。保留 r1 后同一新 ABI tree 的 r2 build PASS（403.580s），
r3 最终增量检查及 81 cases/1067 assertions PASS；见
[descriptor evidence](../specs/182-native-di-python-bindings/evidence/t003-model-descriptor-20260907.md)。

## 2026-09-07 — Design R2 duplicate declaration identity

覆盖检查 r1 拒绝同一函数两处前置声明共享 API ID；修复为每个 ID 一条状态并保留声明位置。
API/还原 r2 PASS；首轮 PDF 构建后的严格检查发现三个长标识符溢出，修正断行后独立重建。
最终 r3 双 PDF（65/69 页）、全部目录/字体/输入身份与源码检查 PASS；失败保留，不计产品资格。
之后并行模型描述符修改再次触发源码漂移；r4 核对新字段/规范身份后仅刷新当前基线，保留前后结果。
最终 r4 当前/目标 66/69 页，工具/API/还原/版面 PASS，检查时无源码漂移。
提交前新增 Python 绑定再次触发漂移；r5 核对绑定并用快照补丁验证未提交绑定身份。
最终 r5 全部文档检查 PASS，852 绑定已覆盖，源码漂移为空；机器记录保存精确采样身份。
普通提交被全索引助手引用钩子拒绝；改用钩子显式提供的 NDNSF_LOCAL_CHECKPOINT=1 本地入口，路径检查保留。
这是文档模型边界，不是产品失败。见 [Design R2](../specs/182-native-di-python-bindings/evidence/design-r2-20260907.md)。

## 2026-09-07 — Spec182 node owner fixture

node/state r1 新 ABI build PASS（410.204s），68/69 cases PASS；空角色负例的
测试 helper 在分配节点时对 roles.size()==0 取模，未进入产品拒绝路径。
保留 r1 日志，修复 helper 后独立 r2 build（21.493s）及 69 cases/972 assertions
PASS；见 [node/state evidence](../specs/182-native-di-python-bindings/evidence/t003-node-state-contracts-20260907.md)。

## 2026-09-07 — Spec182 candidate rank fixture

候选校验 r1 build PASS（41.485s），66/68 cases PASS；两个 V3 case 在共享候选
入口拒绝重复 rank artifact。旧 oracle 给两个 rank 复用同一 artifact，违背维护
SplitCandidate 唯一性约束。保留 r1，修复 SDK/native fixture 后独立 r2 build 与
68 cases/836 assertions PASS；
见 [candidate validation](../specs/182-native-di-python-bindings/evidence/t003-candidate-validation-20260907.md)。

## 2026-09-07 — Design API selector

API 契约首轮渲染使用不存在的 ExecutionLease::release；源码 owner 为 ProviderExecutionLeaseTable。
保留首边界，修正选择器后独立重验；见 [API guide evidence](../specs/182-native-di-python-bindings/evidence/design-api-guide-20260907.md)。
选择器修复后 23 契约/53 签名渲染通过；首轮双份 79 页 PDF 有一个长标识符溢出，已定位并独立修复重建。
连续排版后 63 页；目录页码需第三遍编译才能稳定，已增加构建遍数并另建验证目录。
最终 r4 双份 63 页、58 个目录页码、API 声明/绑定和源码还原检查 PASS；文档失败已修复，不计产品资格。

## 2026-09-07 — Spec182 YOLO tensor edge source audit

NativeGraphSnapshot 缺 tensor edges，YOLO planner 按相邻节点合成依赖及节点数预算，
与维护 Python splitter 不一致。另发现 Qwen fragment 额外哈希与 backend family 差异。
上述局部问题已修复，67 cases/818 assertions PASS；T003-A/B 过早 DONE 已撤回 PARTIAL，
完整候选契约仍待闭合。此为源码发现，
不是网络运行结果。见 [tensor edge audit](../specs/182-native-di-python-bindings/evidence/t003-yolo-tensor-edges-20260907.md)。

## 2026-09-07 — Spec182 publication fixture name

publication recertification r1 新 ABI -j4 build PASS（302.273s）；定向测试在构造
稳定工件名字的 fixture 失败：角色自带前导 /，拼接后含 //，被既有 NDN name 校验拒绝。
修正 fixture 名字，保留 r1 失败后独立 r2 build 与 58 cases/710 assertions PASS；见
[publication recertification](../specs/182-native-di-python-bindings/evidence/t008-publication-recertification-20260907.md)。

## 2026-09-07 — Spec182 graph identity source audit

r2 首次定向运行在篡改 graph 的负例断言失败：产品正确抛 invalid_argument，fixture
期待 runtime_error；真实不同图摘要的 SDK core 对照已通过。保留 r2 日志后修正期待，
r3 build 与 58 cases/594 assertions PASS；未更改产品拒绝逻辑。

维护中的 YOLO binding 区分 planning graph 与 canonical ONNX graph，但原生准备/封存
强制两者相等。现显式绑定两种身份并增加不同 digest 的 SDK oracle；此为源码发现。
发布前后 manifest/source 身份转换仍未接通，见
[graph identity evidence](../specs/182-native-di-python-bindings/evidence/t008-graph-identity-spaces-20260907.md)。

## 2026-09-07 — Design source patch whitespace

设计入 Git 的首轮 staged diff 检查因补丁空白上下文行返回 rc=2；改用零上下文补丁，94 文件还原与 staged diff 重验 PASS。
见 [design evidence](../specs/182-native-di-python-bindings/evidence/design-pdf-baseline-20260907.md#git-patch-whitespace-check)。

## 2026-09-07 — Full Design PDF TeX syntax

完整中文设计 r3 因 Repo 标识符裸下划线触发 `Missing $ inserted`，编译 rc=1。
修复后 r4 双份 35 页编译成功，但 abort 标识符仍有 24.15834pt 横向溢出；继续修复排版。
r5 修复完成，双份 35 页编译、无警告/溢出/缺字、文本一致与版面检查 PASS。
原始失败目录与修复边界见 [design evidence](../specs/182-native-di-python-bindings/evidence/design-pdf-baseline-20260907.md)。

## 2026-09-07 — Spec182 publication artifact identity audit

首轮定向验证另发现旧 sealer fixture 的 1 MiB 不满足真实 Qwen candidate 的 2253 MiB
最低预算，在 publication 前拒绝；保留 r1 build/focused 日志，修正 fixture 后独立 r2
build 与 58-case 定向检查 PASS；未降低产品校验。

旧 ensureArtifacts 接收简化 proposal，只检查返回摘要格式与 role cover，未匹配选定工件。
现迁移完整 V3 输入并逐项检查 artifact digest；此为源码发现，无失败运行。
真实 catalog/Repo 与 requester 主链仍未接通，见
[publication evidence](../specs/182-native-di-python-bindings/evidence/t008-v3-artifact-publication-20260907.md)。

## 2026-09-07 — Design PDF long-symbol overflow

Design R0 首次双 PDF 编译成功，但长测试符号导致第 7 页两个 Overfull hbox；
属文档排版失败，原始构建保留。改用允许断行的内联符号后 r2 双份 7 页 PDF 检查 PASS，
见 [design evidence](../specs/182-native-di-python-bindings/evidence/design-pdf-baseline-20260907.md)。

## 2026-09-07 — Spec182 inspection source audit

inspectModel 合成 catalog source name 并丢失原请求完整 model descriptor，缺实际来源证据。
已改 inspection owner 返回来源/manifest，补齐模型与角色绑定；真实 catalog/Repo 接线仍待完成。
这是源码审计，无失败运行；见 [inspection evidence](../specs/182-native-di-python-bindings/evidence/t008-inspection-roles-20260907.md)。

## 2026-09-07 — Spec182 V3 placement test macro

r1 build 在新测试 BOOST_CHECK 的 std::map 模板参数逗号处失败，尚未运行策略。
修复后 r2 build PASS，最终 r4 build/34-case 定向检查 PASS；见 [V3 placement evidence](../specs/182-native-di-python-bindings/evidence/t003-v3-placement-20260907.md)。

## 2026-09-07 — Spec182 signed offer fixture import

authoring 导入 app_sdk.provider 时缺 py_repoclient 搜索路径，尚未执行签名验证。
补齐仓库 Python 路径后重试；见 [core offer evidence](../specs/182-native-di-python-bindings/evidence/t008-core-offer-admission-20260907.md)。

## 2026-09-07 — Spec182 offer admission source audit

NativeOfferAdmission 从本地 policy 合成 Provider 能力，未消费 ACK payload 或验证
offer 自身签名。T008-B 保持 PARTIAL；先补数据解码，再接 Core provenance 与 policy-bound
signature。此为源码审计，无失败运行；见 [offer evidence](../specs/182-native-di-python-bindings/evidence/t008-observed-offer-20260907.md)。

## 2026-09-07 — Spec182 typed shape Provider consumers

r2 构建在 NativeProviderHandler.cpp 的 YOLO 文本 metadata 与 decode tensor identity
两个旧字符串消费者失败。已改显式文本转换及带类型的身份编码；原始 r2 build.log 保留。
见 [typed shape evidence](../specs/182-native-di-python-bindings/evidence/t004-typed-shape-20260907.md)。

## 2026-09-07 — Spec182 typed shape consumer compilation

shape variant 的新 ABI 构建 r1 在 NativeYoloMergeRunner.cpp:335 失败：旧 metadata
字符串拼接未处理整数/符号类型。已定位文本 runner 配置边界，显式转换后独立 r2 重试。
见 [typed shape evidence](../specs/182-native-di-python-bindings/evidence/t004-typed-shape-20260907.md)。

## 2026-09-07 — Spec182 canonical float overflow probe

typed canonical JSON r1 的 DBL_MAX 对照失败：stream 解析溢出后饱和值误判为回转成功，
输出 2e+308。已定位为 helper 缺少 failbit 检查；保留 oracle，修复后独立 r2 重试。
见 [canonical JSON evidence](../specs/182-native-di-python-bindings/evidence/t004-canonical-json-20260907.md)。

## 2026-09-07 — Spec182 T003 placement repair fixture failures

r1 定向测试 exit 201：sealer fixture 依赖无关 residency 排序；新双角色 fixture 少传一个
tensor degree，在 splitter 构造阶段拒绝。已定位 fixture 首边界，修复输入后在独立 r2 重试。
原始 `.codex-tmp/spec182-t003-placement-r1/` 保留；见
[placement repair](../specs/182-native-di-python-bindings/evidence/t003-role-placement-20260907.md)。

## 2026-09-07 — Spec182 T003 placement source audit reopened

NativePlanning.cpp::propose 将全部角色分配给一个 Provider，并按 residency 集合大小
代替目标工件命中排序。既有 rank-one 局部 PASS 未覆盖该缺陷；T003-C 及相关依赖 DONE
已回退 PARTIAL。此为源码审计，没有新的失败运行；下一步修复逐角色独立 Provider 分配。
同时 T004 真实工件/grant 输入修复已完成定向 33-case 验证，完整 wire 仍未完成。
见 [binding repair and placement audit](../specs/182-native-di-python-bindings/evidence/t004-artifact-grant-bindings-20260907.md)。

## 2026-09-07 — Spec182 T004 native Selection wire incompatible

最小 native codec 诊断中，NativePlanSealer::encode 接受字段并输出 386 字节，
生产 nativeSelectionProjectionV3FromJson 在第一 schema 门拒绝。首边界是 DI wire
构造，不是 Core 网络或运行环境。T004-A/父 T004 重开，禁止把旧 7-case PASS 当完整
Selection 证明；修复完整 canonical wire 和真实工件/grant 绑定后再继续 T010。
原始 `.codex-tmp/spec182-t004-wire-audit-r1/` 保留；见
[A8-01 evidence](../specs/182-native-di-python-bindings/evidence/t004-wire-reopened-20260907.md)。

## 2026-09-07 — Spec182 T010-A unit selector rejected

两个 Boost.Test suite 误用逗号组合，exit200，`no test cases matching filter or all
test cases were disabled`；未执行用例，不是 requester 行为失败。原始
`.codex-tmp/spec182-t010a-deadline-r1/client-state.log` 保留，改为分别执行单 suite。
见 [deadline evidence](../specs/182-native-di-python-bindings/evidence/t010-a-deadline-20260907.md)。

## 2026-09-07 — T002-A L0 consumer aborts: freeze() rejects an empty native adapter registry
- **Area**: spec182 T002-A Installed Library Boundary
- **Symptom**: installed-library consumer
  `tests/standalone/spec182-installed-consumer.cpp` (T001-C-frozen L0 carrier)
  ran against the staged prefix and aborted before printing its OK marker:
  `terminate called after throwing an instance of 'std::invalid_argument'`,
  `what(): native adapter registry is empty`, RUN_RC=134 (core dumped).
- **Root cause**: intra-spec182 contradiction. Commit `3afa7492`
  (2026-09-07 01:50, “spec182: add native assembly grants preparation and
  bindings”) *introduced* `NativeAdapterRegistry::freeze()` together with an
  empty-registry precondition, eight minutes before commit `aba90194`
  (01:58) froze `spec182-installed-consumer.cpp` as the L0 carrier, whose
  probe semantics require an empty registry to be freezable
  (`frozen()==true`, `find("missing")==nullptr`). No production caller
  depends on the throw today; the only prior freeze caller
  (`tests/unit-tests/di-native-preparation.t.cpp:51-52`) registers an
  adapter first. At the T002-A stage the installed library deliberately has
  no concrete adapter class yet (T003-A/B add them later), so rejecting the
  empty state makes the installable boundary unusable by any consumer.
- **Fix**: `freeze()` now latches unconditionally; the “at least one
  adapter” precondition, where a caller needs it (e.g. a provider), is
  enforced at the use site by a `find()`-null check, not by the registry
  latch. Evidence records the re-run under the frozen L0 command.
- **Ref**: NDNSF commit `3afa7492` / `aba90194`; run dir
  `.codex-tmp/spec182-t002a-l0-r1/` retained.
- **Lesson**: a frozen executable carrier and its own feature's earlier
  implementation commits can disagree; the later process freeze wins, and
  every frozen carrier must actually be executed once before closure.

## 2026-09-07 — Spec182 tokenizer bridge toolchain unavailable

尝试在本机对固定 Rust tokenizer bridge 做 release 构建时，首边界为
`cargo: command not found`（exit127）；未进入 Cargo 解析、编译或链接，不能把
bridge/ABI 记为通过。保留原始目录 `.codex-tmp/spec182-tokenizer-r1/`；现有
C++ tokenizer 包装和静态检查继续作为源码证据，实际 bridge 构建需在安装了
Rust1.90/Cargo 的同源工具链上重跑。

同一诊断目录的首次手工 Boost.Test 链接遗漏 `-pthread`，`libcrypto` 因此出现
`pthread_*` 未解析；补齐线程库后同一 3 个负例用例 **3/3 PASS**。该次失败是
命令边界，不是 tokenizer 实现或测试失败，原始摘要见
`.codex-tmp/spec182-tokenizer-r1/link-r1.json`。

## 2026-09-07 — Spec182 skill validator schema mismatch

Spark执行包检查时，通用skill quick_validate拒绝两个既有Spec Kit入口的顶层`compatibility`字段；首边界为校验器schema，不是任务执行或native产品失败。保留原格式，YAML/必需字段/profile路由检查PASS；仓库code-design通用校验PASS。实际检查和fallback见[执行包记录](../specs/182-native-di-python-bindings/evidence/spark-execution-preparation.md#validation)。未启动产品构建/测试。

## 2026-09-07 — Spec182 installable DI library build blocked at NAC-ABE ABI

构建 `ndnsf-distributed-inference` 在既有 `ndn-service-framework` 编译边界失败，exit1；`ServiceUser.cpp`/`ServiceProvider.cpp` 调用 `getPublicParamsDataName`、`getPublicParamsDigest`、`clearCache`、`refreshPublicParameters`、`refreshDecryptionKey`，当前安装的 NAC-ABE 头文件没有这些成员。该失败发生在 DI 新对象编译前，不能归因于 Spec182 代码，也不能把本次构建当作库或产品 PASS。原始命令/首边界记录在 `.codex-tmp/spec182-native-build-20260907-r1/`；下一步先固定匹配的 NAC-ABE 头/库工具链，再重跑同一目标。

## 2026-09-06 — Spec182 typed-complex reference conversion

扩展initializer参考提取R1在complex64-typed失败：ONNX1.17 `_to_array`先组合complex值，再以float storage dtype调用np.asarray，抛`TypeError: can't convert complex to float`，exit1。此前模型full checker已通过，尚未产生identity；不是C++算法失败。raw `.codex-tmp/spec182-t001-identity-extended-r1/boundary.json`。下一R2分别提取其他表示并将typed-complex单独记为旧转换缺陷，不把它标成已支持稳定oracle或降低普通numeric验收范围。

## 2026-09-06 — Spec182 native reuse review boundaries

审计发现现有NativeEpochCoordinator将完整decode作为稳定stream前缀，且C++采样的Top-P截断归一化、重复token惩罚与Python reference不一致。固定tokenizers0.20.3/现有byte-fallback fixture诊断exit0：`好`的prefix为`�→��→好`；seed8的Top-P例和重复token Greedy例，Python返回0、native源代码推导为1。首边界是文本提交/采样算法，不是网络或授权失败；未执行native产品。raw `.codex-tmp/spec182-native-reuse-review-20260906-r1/`；完整输入/hash/源码/边界见[native reuse review](../specs/182-native-di-python-bindings/evidence/native-reuse-review-20260906.md)。A7-08/A7-09 OPEN，T001/O-004先冻结处置，T007/T011修复后由T016验收，不将reference诊断计为产品PASS。

## 2026-09-06 — Spec182 legacy ONNX initializer identity boundary

T001独立reference探针（ONNX1.17.0/NumPy1.24.4，未修改graph.py）确认：BFLOAT16 raw_data两次计算的content digest均不等于声明权重位模式，而typed表示正确；STRING相同model digest在两个独立进程产生不同initializer content digest。首边界是旧numpy_helper/object-array归一化，不是网络、授权或原生装配结果。普通12种数值类型的24个raw/typed向量通过。raw `.codex-tmp/spec182-t001-identity-r1/diagnostics.json`，脱敏[durable evidence](../specs/182-native-di-python-bindings/evidence/identity-reference-20260906.json)。O-002/O-004继续冻结稳定身份与兼容处置；不把错误摘要写为正确oracle、不以未运行的C++测试关闭此缺陷。

## 2026-09-06 — Spec182 tokenizer toolchain download transport

R2改为rustc/cargo/rust-std最小组件，首个rustc归档仍在Python3.8 urllib TLS读取阶段以同样错误exit1；保留`rust-r2/boundary.json`与部分归档。R3只切换Node22 HTTPS传输，保持官方来源、TLS验证及SHA256检查，最多一次有界重试；依赖设计与其他T001工作继续，不把下载失败升格为产品阻塞。

R3已恢复：Node22 HTTPS成功下载rustc/cargo/rust-std三组件，官方SHA256全部匹配，exit0。失败边界为旧Python传输路径，未观察到tokenizer代码问题；hash与后续探针结果记录在同一dependency design。

T001依赖可行性所需Rust1.90.0归档下载R1 exit1，在TLS body读取阶段报`DECRYPTION_FAILED_OR_BAD_RECORD_MAC`，尚未校验/解压/安装，更未编译或执行tokenizer。保留部分归档与`.codex-tmp/spec182-t001-dependencies/rust-r1/boundary.json`；新R2目录有限重试并验证官方SHA256，不使用部分文件。ONNX依赖构建独立，不因下载失败重跑。设计及进度见[dependency design](../specs/182-native-di-python-bindings/contracts/native-dependency-design.md)。

## 2026-09-06 — Delivery-only scope correction

用户明确指出本轮任务仅为交付，编译与测试由另一台机器负责。此前本机ABI消费者验证属于超出范围的扩展；立即停止R4 owned构建进程组2531869，不再启动NDNSD构建、unit/integration、Python扩展验证或MiniNDN。R1/R2中断及R3普通Provider构建记录保留，不能外推完整验证PASS。后续构建/测试均TRANSFERRED，不作为交付阻塞项；当前源码包/definition/依赖锁/skills与GitHub发布已完成，见source handoff。

## 2026-09-06 — Waf interrupted signature persistence

R2日志证明R1超时后Waf未保存task signatures，实际从1/318重新编译，不能称为仅续编剩余对象。停止重复R2（SIGINT exit68，78.516s）并确认编译子进程退出，保留同一raw root的`build-r2/boundary.json`。R3以原配置、`-j2`、3600秒上限只选择缺失的`di-native-provider`及必要依赖；R1在同一fresh树已经成功链接的Core/unit/integration/应用保留逐目标证据。最终需补齐全部交付目标并运行测试，不将任何中断轮次记PASS。

## 2026-09-06 — Fresh ABI build runner time limit

新SVS/NDNSD闭包消费者fresh build R1在299/318触发执行器1800秒上限：exit124，1800.081s，`TIMEOUT_AT_RUNNER_BOUNDARY`。没有compiler error，Core/unit/integration及部分应用已链接，仍有native-provider对象未完成；不能把未完整构建记PASS或解释为协议失败。独立验证树 `/home/tianxing/NDN/ndnsf-svs-abi-20260906` 下 `.codex-tmp/svs-abi-20260906-r1/build-r1/` 保留receipt、boundary和完整log；确认无遗留编译进程后以 `build-r2`、3600秒有限上限继续同一fresh `build-abi`，配置和`-j2`不变。

## 2026-09-06 — Source handoff tooling resolved

最终交付工具/模板/旧builder fixture统一R5 **28/28 PASS**；五项共享skill与接收说明链接/语法通过。真实四库包生成、搬迁后verify、固定base摘要及definition render PASS。R1/R2的生成物/离线VERSION.info、canonical workload与历史host-gate fixture边界均已定位并修复，原日志保留。详见 [source handoff checkpoint](../Experiments/TigerCluster/docs/source-handoff.md#checkpoint)。NDNSF新SVS/NDNSD ABI闭包的fresh编译与运行验证仍待完成，不宣称SIF或Tiger PASS。

## 2026-09-06 — SVS offline metadata and transitive ABI closure

真实归档R2在 `LOCAL_SIF_DEPENDENCY_SOURCE_MISSING:VERSION.info` 停止（exit1），raw `.codex-tmp/source-handoff-20260906/package-r2.log`。SVS的该文件由Waf生成且被Git忽略，本机残留版本仍指向旧commit。改为从固定源码的VERSION/GIT_TAG_PREFIX和git describe生成归档内元数据，记录派生来源，不修改源checkout。同时静态查到NDNSD自己构造SVSPubSub，旧NDNSD二进制也是ABI消费者；将其干净源码及pkg-config路径修复纳入锁定和fresh重建，不复用base中的旧库。

旧build-record fixture R2进一步在 `SPEC175_WORKLOAD_NOT_SEALED:Experiments/TigerCluster/jobs/spec175/workload.json` 拒绝，raw `.codex-tmp/source-handoff-20260906/source-handoff-build-record-r2.log`：兼容alias与canonical路径不一致。fixture按canonical封装，真实sealer同时保留legacy镜像路径及canonical身份，生产门保持不变。

## 2026-09-06 — Source archive cleanliness boundary

真实交付包R1在 `HANDOFF_SOURCE_UNTRACKED:examples/example-trust-anchor.cert` 拒绝，未创建bundle：开发依赖checkout带有未跟踪生成物，HEAD与tracked clean不足以证明归档内容。保留原目录，改为三库全部使用精确commit的全新detached checkout准备R2，不放宽sealer的未跟踪源码检查。这是输入来源失败，尚未运行容器编译。

## 2026-09-06 — Source handoff tool fixtures

交付工具统一检查R1为19 PASS / 5 FAIL，原始 `.codex-tmp/source-handoff-20260906/tool-checks-r1/output.log` 保留。五项旧 `test_build_local_sif_record.py` 都在 `HOST_GATE_WORKLOAD_SEED_MISMATCH` 提前退出：fixture引用历史真实G3清单，但运行时校验当前workload。未到达所测definition/label/source边界，不是新SIF构建失败。修复测试为独立临时fixture，保留生产门与负例；不更新历史资格清单冒充当前结果。

模板首轮静态检查曾因开头注释被既有boundary parser识别成第三stage而失败；说明移入builder头之后。随后静态复审发现wheel锁只检查非空会漏掉离线python-ndn依赖，现要求五个固定wheel输入并补缺项拒绝。模板原始R1/R2日志由本轮 [source handoff](../Experiments/TigerCluster/docs/source-handoff.md) 记录；这些静态/fixture失败均未执行Apptainer。

## 2026-09-06 — Merge validation resolved

静态修复后 full unit 759/759、GDB full integration 154/154、current Python 2171 passed /22 skipped；MiniNDN用户撤销、仅新增授权、Provider撤销全部 PASS。PATH启动失败在独立 R2 修复，最初日志不覆盖。Provider场景主动 SIGINT 后重启的旧进程 exit -2，其余应用 exit0。完整身份和范围见 `specs/182-native-di-python-bindings/evidence/merge-validation-20260906.json`；不能把 current Python 范围或三个网络场景外推为历史全套/181最终qualification。

## 2026-09-06 — MiniNDN launcher PATH boundary

合并验证 `minindn-user-revocation-r1` 在 0.644 s 退出1，首边界为 Mininet 启动器找不到 `ifconfig`；编译 PATH `/usr/bin:/bin:/usr/local/bin` 遗漏系统网络工具目录。拓扑/协议尚未运行，不能解释为撤销失败。系统 `/usr/sbin/ifconfig` 已确认存在；MiniNDN root PATH 增加 `/usr/sbin:/sbin`，保留编译器绝对路径约束。原始 `.codex-tmp/merge-20260906/minindn-user-revocation-r1/output.log` 保留；下一轮使用新目录。

## 2026-09-06 — Full integration after static fences

`integration-static-r1` 仍运行时已发现两个首边界：`Spec175NativeTinyOnnxExpiredDeadlineCleansProviderState` 的 provider failure count 不符，以及 `ProductionNativeHandlersRunD2h212ToCompleteOracleResponse` 缺一个角色/最终 oracle。保留本次完整 GDB 日志，先对照新增排队 fence 与原始 deadline/cleanup 状态更新，不能直接放宽 timeout 或删除拒绝断言。定向生命周期、证书撤销及 Controller45项通过不能替代该全模块结果。

## 2026-09-06 — Static repair cross-review lifetime finding

Build static-review R1 主动 SIGINT（exit68，154.920 s）。交叉复审发现新增 handler `!current()` 分支在 Provider 析构排空队列时仍向 Face 投递裸 `this` 的失败回调；析构后 dispatch 存在 UAF。停止当前构建，先在投递前和回调内检查共享 stopping token，并补 queued-handler 析构回归；不得把中断视为构建通过。原始 `.codex-tmp/merge-20260906/build-static-review-r1/` 保留。

## 2026-09-06 — Static review and full-module recount

NFD 定向 R1 进一步定位：提供私有 NFD 后，首边界变为 PUBPARAMS readiness timeout（10.418 s），因为 Controller 主 Face 是 DummyClientFace，不能与独立真实 probe Face 经 NFD 往返。不是仅缺守护进程。12处 policy fixture 启动改为已有 test-access 调用真实 `registerInterestHandlers()`，不设置 ready、不替换策略/签名处理；完整 `start()` 仍由13项 standalone readiness 和真实 NFD 测试覆盖。原始 `.codex-tmp/merge-20260906/integration-nfd-preflight-r1/`，NFD `/tmp/ndnsf-int-ntroyrk7/`，owned child cleanup=0。

Python import isolation R1 **43 passed / 3 skipped**（12.630 s），CLI loader 恢复 `sys.path` 后与 YOLO 联合执行通过；包含路径保持的新回归。

完整 module 日志纠正先前只统计部分 suite 的摘要：integration R3 为 **140/154 PASS、14 failed**，R4 为 **142/154 PASS、12 aborted**。其中 11 项是 Controller fixture 的 Face 尝试连接不存在的私有 NFD socket；另 1 项 certificate revocation 的 Controller 大写 digest 与 User/Provider 小写 digest 不匹配，不能归类为时间波动。原始目录 `integration-r3`、`integration-r4` 保留。MiniNDN 前先修复并重新验证。

当前 Python R1：2158 passed / 8 failed / 22 skipped；八项 YOLO 导入失败，独立 YOLO 19 项通过，正在检查联合执行时的 lazy import 首异常。原始 `.codex-tmp/merge-20260906/python-current-r1/output.log` 保留。Context Mode 的 `session-events` timeline 恢复查询被 guard 拒绝，继续以仓库、原始日志为准。

静态审查发现 User identity prefix retry 捕获局部 `onFail` 引用，构造结束后的失败回调存在 use-after-free；修复后须覆盖延迟重试。

## 2026-09-06 — D2h predecessor boundary / integration R3

定向 R2 使用了 Boost.Test 不接受的逗号连接完整路径，exit 200，未执行协议用例；原始 `d2h-regression-r2` 保留，改用同一 suite 下的 `ProductionNativeHandlersRunD2h*` selector 重试。

完整 R3：90/92 PASS，exit 201，183.621 s，无崩溃。Trace R1 首次失败为 `NDNSF_DATA_V1 HMAC verification failed`：compact segment 的认证预算原由 capability 生成，接收方错误地用可更严格的 Selection edge deadline 恢复 AAD。恢复 capability 的原始认证字段，同时保留 edge 对实际取数的 deadline 限制；D2h 121/212 既有测试正好覆盖两者不同的情况。

R3 已通过原崩溃的 targeted-stream 阶段；D2h 121/212 仍分别只观察到第一阶段 1/2 个角色，未得到完整 oracle response。按后继准入、依赖取数、scope-key 解密顺序使用独立 trace 定位；不扩大超时或删除 oracle 断言。原始 `.codex-tmp/merge-20260906/integration-r3/output.log`。

## 2026-09-06 — Owner wheel closure

R2 的临时环境来源断言仍失败，说明共享第三方 site 的环境不能依赖 pip 默认同版本判定。安装使用 `--ignore-installed` 强制这组本地产物进入临时 venv，并保留具体越界模块路径诊断；不卸载或替换主机包。

全量 Python 首次缺 `conversation.py`；补 SDK wheel 所有权后 wheel-closure R1 进入已声明 cryptography 依赖缺失边界。安装测试原来 `--no-deps` 且空 venv；改为离线复用测试主机第三方依赖，同时强制每个已导入 DI 模块来自临时 venv，保留 wheel 文件不碰撞与卸载后不可导入的断言。原始 `.codex-tmp/merge-20260906/wheel-closure-r1/output.log`。

## 2026-09-06 — Full Python diagnostic R2

后续 current-fixes R1（7 failed / 123 passed）与 R2（5 failed / 53 passed）首边界已定位为旧 fixture 和本机新导出模型与远端固定 registry 的身份差异；显式本地临时 registry 保留严格 hash 校验，修复依据见 [resolution design](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

3079 passed / 128 failed / 39 skipped，exit 1；旧实验目录/冻结基线缺失、显式 native binary/model 输入未设置，以及当前 facade、clock、build-prefix 测试夹具漂移。按既有 `run_spec175_python_gate.py` 的历史诊断与当前兼容性门分工，保留全量失败，不改旧 hash，不重跑历史 SIF/Tiger；当前合并覆盖的 Core/Repo/UAV/DI 测试另行明确选择并修复。原始 `.codex-tmp/merge-20260906/python-r2/output.log`。

## 2026-09-06 — Provider detached fetch lifetime / integration R2

GDB 捕获旧 Provider assignment worker 在销毁后调用 `Face::getIoContext()`；不是当前 targeted-stream 用例的独立失败。改为 Provider 所有的有界 fetch pool，关闭时取消等待、join，排队回调先检查共享关闭标志。原始 `.codex-tmp/merge-20260906/integration-r2/output.log`，完整记录见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-07 — Spec182 T006-C: worker digest gate rejects every valid sha256 digest

- **Area**: spec182 T006-C Bounded Native Worker
- **Symptom**: Spec182OnnxWorkerProtocol suite — 16 failures across
  `MetadataAcceptsCanonicalEnvelopeRoundtrip`,
  `MetadataRejectsDigestFormatAndPayloadMismatch`, and
  `SubprocessUnregisteredThenRegisteredMatchesInProcess`. The roundtrip case
  showed `check.ok` false with `failureCode = DI_NATIVE_ONNX_WORKER_METADATA`,
  failureMessage `request metadata recipeDigest is invalid`; the child worker
  rejected the same envelope that the in-process validate call produced.
- **Root cause**: `isSha256Digest()` in
  `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.cpp`
  required `value.size() == 66` and only checked 7+64 = 71 bytes would be
  valid; every canonical digest (`sha256:` prefix + 64 hex) is 71 bytes, so
  the format gate rejected every digest before the payload-digest compare
  could ever run, and the parsed certified slice was never populated.
  Symptom cluster was one root cause: (a) envelope gate METADATA instead of
  ok; (b) digest-payload mismatch classified METADATA instead of RECIPE;
  (c) the real worker child (stale binary, pre-fix digest gate) rejecting
  the envelope with a METADATA error frame.
- **Fix**: size gate corrected to 71 with a comment
  (`"sha256:" (7) + 64 hex`). Diagnostic staging was added and later removed
  from the failing test; after the fix all 21 Spec182OnnxWorkerProtocol cases
  pass against a rebuilt worker binary.
- **Ref**: re-run command
  `./build-nac182/unit-tests --run_test=Spec182OnnxWorkerProtocol
  --log_level=test_suite`; run dirs retained under the suite output.
- **Lesson**: a format gate with a wrong length constant fails every valid
  input silently as "invalid", so an always-rejecting validator can look
  like a roundtrip/envelope bug; assert length against `prefix + N hex`
  rather than a magic total.

## 2026-09-07 — Spec182 T006-C: spec181-assembly-parity fails full rebuild on missing onnxruntime include

- **Area**: spec182 T006-C Bounded Native Worker (full-build regression path)
- **Symptom**: the first full `./waf build` after editing tests/wscript
  recompiled all 635 tasks; `spec181-assembly-parity` (tests/wscript) failed
  compiling `NativeOnnxRecipeAssembler.cpp` with
  `onnxruntime_cxx_api.h: No such file or directory`, because its `use=`
  closure was `... ONNX ...` without `ONNXRUNTIME` while its compile line
  carried `-DNDNSF_DI_ENABLE_ONNXRUNTIME_CPP` and the ONNX prefix include
  (`repo/.codex-tmp/spec182-t001-dependencies/onnx-install/include`) only
  ships ONNX 1.17, not the runtime headers (`/opt/onnxruntime/include`).
- **Root cause**: the spec181-era target predates the spec182 unified
  runtime closure; every sibling DI target
  (`spec181-protected-runtime-closure`, `spec182-installed-consumer`,
  unit-tests) already lists `ONNXRUNTIME`. The target had not rebuilt since
  the runtime include became mandatory, so the failure surfaced only when a
  wscript change forced a full re-signature.
- **Fix**: add `ONNXRUNTIME` to the `spec181-assembly-parity` `use=` string
  in tests/wscript (closure drift, no behavior change). Full build green in
  11m51s at `-j2`.
- **Ref**: re-run command `./waf -o build-nac182 build -j2`.
- **Lesson**: waf content signatures mean an obsolete target stays green
  until any wscript change forces a full re-signature; after configuring on
  the unified dependency closure, audit remaining targets that compile
  adapter sources without `ONNXRUNTIME`.

## 2026-09-06 — Python collection and generation fixture R2

Python collection 缺 Repo binding 和三个既有辅助源脚本；integration 多 Provider generation fixture 的新 input endpoint digest 与旧常量冲突。分别补构建闭合/输入脚本和独立 endpoint identity，保留首边界证据。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — Real-NFD readiness collector R1

实际两轮启动/独立 Face round trip 已发生；旧 collector 要求 challenge Data 名后还有 `/`，与新 exact reply 不符，exit 1。修正 exact token 匹配并重跑，不以 collector failure 推断协议结果。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Controller readiness / versioned NAC parameters

Readiness R1 在 PUBPARAMS 首边界超时，exit 124。旧随机后缀探针与新 NAC 固定 generation 名称不兼容；分离 Authority fresh challenge 和当前版本参数验证，不回退 NAC 的版本真实性约束。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — Integration fixture build R4

D2a fixture 修订时保留了该 case 未定义的 `selectionObserved` 标志，编译拒绝；删除无关赋值后 R5 重建。Unit R2 已 751/751 PASS，不能替代 integration。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Native executable link and full integration R1

Native target 缺 RuntimeStatusStore source；全量 integration R1 60/92 PASS 后以 139 退出，包含 targeted stream memory fault 和旧 wrapper 导入。先补 target source、重建同源 binding 并用 GDB 定位崩溃。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged legacy ingress integration R1

完整 suite 中旧 ingress 流程报告空 assignment、错误 role 和 D2b 未完成；保留原始 R1 后定向检查 Selection 首边界。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged full unit R1

747/751 PASS，4 failed（2 aborted），exit 201。空 tensor overflow 检查除零、manifest size 和选中 Provider 加密输入流程失败；逐项定位，不把旧失败当作允许项。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged SVS catch-up arguments

NDNSF build R1 捕获混合调用：catch-up 方法名已恢复，但自动合并保留旧方法的 bool 参数尾部。恢复调用方已有的数量和毫秒年龄实参；保持现有接收与权限语义。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — NAC installed pkg-config paths

首次 CMake 配置的 `.pc` 在 GNUInstallDirs 之前生成，include/lib 错指 prefix 根。显式 Waf prefix 不足以保证 Python 绑定使用新依赖；调整 NAC 初始化顺序并验证 fresh-config 导出。运行代码未变，完整 case 结果保留。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)。

## 2026-09-06 — NAC AttributeAuthority test identity isolation

依赖 full test 41/46 PASS；五项 fixture 默认 Face 读入主机旧 PIB/TPM，尚未测试授权行为即签名失败。显式传入已有内存 KeyChain，保留断言并重跑完整 suite。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged Context Mode fixture contract

合并 guard 后 42 PASS / 3 FAIL，首次边界为旧 Claude fixture 的 platform/registry 配置；更新有效 fixture，保留缺 hook/缺 flag 的失败断言。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)。

## 2026-09-06 — Integration dependency preflight R2

Boost 路径修复后，NAC test link 仍调用 Linuxbrew ld，系统 OpenSSL/dl 符号解析失败。固定系统工具链再构建；原始 R2 日志和首边界见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#build-preflight-r2)。

## 2026-09-06 — Integration dependency preflight R1

NAC-ABE test link 选中缺失的 local Boost 1.82 库；NDNSF configure 拒绝尚未安装的目标 NAC prefix。均为构建前置失败，不是协议结果。先固定系统 Boost 并完成依赖测试安装，再重跑 NDNSF configure。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#build-preflight-r1)。

This is the repository-level index for failed, blocked, and `UNQUALIFIED`
attempts. It is an engineering memory, not a replacement for the active Spec,
the source tree, or a raw run directory.

## Read-first rule

At the start of every substantial task, read the newest entry in this file and
open its durable evidence record. If the entry names a raw log under the
ignored workspace temporary directory, inspect that log with targeted
`rg`/`tail` queries before choosing the next command. Then read the applicable
documents in
[`architecture-reading-guide.md`](architecture-reading-guide.md).

A failed preflight, startup barrier, or evidence collector is not a protocol
result. Do not retry a later gate, reuse a candidate, or claim a PASS until the
failure's controlling boundary and invalidation effect are understood.

## Current failure index

**Experimental consolidation closure (2026-09-06): development checks PASS.**
原生合并 `c770f18b` 的unit759/759、integration154/154、current Python
2171 passed/22 skipped和三个MiniNDN场景均PASS；新目录关联工具162 passed/
3 skipped，新Tiger工具58/58 PASS。下方collector RED由精确证据核对和进程组
清理修复关闭；原始失败保留。Tiger Local R8和B003运行验收仍未关闭，用户已暂停实验。
见 [integration closure](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)
及 [Tiger baseline](../Experiments/TigerCluster/docs/two-node-baseline.md)。

**Tiger baseline collector review R1 (2026-09-06): expected regression RED.**
新增ACK/provider/request/selection与wrong-root边界证据负例在旧collector上
19 failed / 13 passed / 20 deselected，0.23s，证明它会接受不完整或矛盾证据。
这不是网络结果；保留 `.codex-tmp/merge-20260906/tiger-baseline-collector-red-r1/`。
修复后相关unit必须通过，B003实际运行仍未完成；见
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md)。

**Tiger two-node baseline Local R8 (2026-09-06): wrong-root boundary mismatch.**
All normal service, permission-rejection and cleanup checks passed. The wrong-root
child rejected PUBPARAMS authentication with abort134 before permission delivery.
Preserve this FAIL; classify only this exact isolated authentication abort in the
next run, rejecting unrelated crashes/timeouts. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R7 (2026-09-06): observer/lifecycle FAIL.**
The service returned correct ECHO, but generic V2 binding leaves authentication
metadata unset. Replaced the unavailable-field assertion with observed native
ACK/Selection plus an actual wrong-root rejection obligation. The old Controller
wrapper cannot join its infinite native loop; use the existing C++ executable.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R6 (2026-09-06): permission bootstrap FAIL.**
Raw signed roundtrips, signature negatives and PUBPARAMS succeeded. Controller
lacked public target certificates in its PIB and refused permission encryption;
added public-only imports with ndn-cxx readback and unchanged private-key checks.
Also isolated session state and corrected TERM ordering for FUSE-backed containers.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R5 (2026-09-06): pre-NFD import FAIL.**
The image exposes UnixFace in stream_socket, not stream_face. Runtime preflight
stopped both workers before NFD startup; corrected the actual module path.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R4 (2026-09-06): probe import FAIL.**
Multiline NFD/route configuration succeeded on both local instances. The probe
used KeychainSqlite instead of the image's KeychainSqlite3; corrected the symbol
and moved full application imports into pre-NFD inspection. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R3 (2026-09-06): NFD management startup FAIL.**
Both NFDs aborted during internal FIB registration (10021). Review found compact
INFO list entries could not preserve privilege/policy nodes; restored multiline
INFO generation before retry. No protocol result; owned processes reaped. See
the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R2 (2026-09-06): identity setup FAIL.**
ndnsec refused root certificate installation into a nonexistent role-local root
identity. The validator already loads the public root file; removed the redundant
PIB installation. No NFD started; private state was cleaned. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R1 (2026-09-06): host preflight FAIL.**
The local Python lacks str.removeprefix; version parsing stopped before identity
or NFD startup. Replaced it with an explicit prefix check and slice. Raw output
and subsequent attempts are indexed in the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger review sync R1 (2026-09-06): 58/58 tool checks PASS.**
Adopted the reviewed supervisor fixture expectation for the existing
collector-before-terminal-validation order, retaining FAILED/cleanup checks,
and restored sys.path after profile-test import. The 51 prior checks plus
7 collector positive/negative checks pass; the migration R1 assertion failure
is closed. No production runtime or cluster was run. See [review sync evidence](../specs/182-native-di-python-bindings/evidence/tiger-directory-migration-20260906.md#review-sync-r1).

**Tiger directory migration unit R1 (2026-09-06): 50 PASS / 1 FAIL.**
The supervisor no-result unit expects TERMINAL_RESULT_MISSING but receives
CollectionError from the collector that now runs first. All 64 moved files
retain their original bytes and modes. The same named test with identical
bytes in a separate physical pre-migration layout reproduces the same failure;
this remains a baseline tool/test issue, not a relocation regression.
No SIF or Tiger execution occurred. See [migration evidence](../specs/182-native-di-python-bindings/evidence/tiger-directory-migration-20260906.md).

**Spec181 delivery-tool counterfactual R1 (2026-09-06): expected semantic RED.**
The 40-check baseline passes. Removing only the exit-code rejection makes the
same named regression fail with DID NOT RAISE, proving it detects false PASS
despite a nonzero child exit. Preserve the mutant and restore the production
check before final validation. Real T008 qualification and sealing remain open.
See [delivery tool evidence](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t009-delivery-tool-20260906.md).

**Spec181 T008 exporter adoption R1 (2026-09-06): focused PASS.**
The isolated exporter/adapter/numerical run passes 41 checks with no skips.
Explicit checkpoint input, actual 32/640 ONNX export, registered signatures,
640 CPU ORT versus PyTorch oracle, and production User negative branches are
covered. Preserve 22 fixed-shape export warnings; candidate local-delivery
tool closure and complete qualification remain open. See
[exporter adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#exporter-adoption-r1).

**Spec181 T008 input closure R3 (2026-09-06): focused PASS.**
The isolated inventory/supervisor run passes 92 checks after the actual RED.
Checkpoint files and registry-referenced public keys are bound; changed inputs
are rejected before children. The shared wrapper output-source dependency is
included with explicit CLI precedence and missing-input rejection. Three
registered Ed25519 public keys pass digest/identity checks. Full qualification
remains open. See [R3 closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r3-and-public-material).

**Spec181 T008 input identity R2 (2026-09-06): missing shared CLI dependency.**
All new input checks pass; the isolated two-file run has 89 PASS and one FAIL.
The existing streamed-generation wrapper regression exposes an unadopted
runner-owned output-directory option. Preserve its CLI failure, adopt only
the output-source/default rejection behavior, and retain the existing test.
See [R2 boundary](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r2-and-shared-wrapper-dependency).

**Spec181 T008 input identity R1 (2026-09-06): semantic RED.**
Fourteen focused checks expose unbound checkpoint and registry public-key
files; five existing drift checks pass. The actual gate attempts its child
entry after either new input changes, caught before a real child launch.
Repair the inventory input owner; no full suite or network case ran. See
[input identity R1](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r1).

**Spec181 T008 assembly closure R4/R1 (2026-09-06): focused PASS.**
The corrected shared-source registration builds both complete C++ test targets.
Only the three named assembly cases ran: 3/3 cases and 138/138 assertions pass
through actual CPU ORT load/warmup with bound Provider/model/plan identities.
The complete suite remains blocked on remaining source/input closure. See
[assembly closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#assembly-test-build-and-focused-closure).

**Spec181 T008 full test build R3 (2026-09-06): source-registration link failure.**
The migrated assembly test compiles, but its shared preparation implementation
was added to grant_sources instead of di_integration_sources. Move that
registration; no suite ran. See [assembly migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#assembly-test-api-migration-plan).

**Spec181 T008 full test build R2 (2026-09-06): stale assembly-test API.**
The ProtectedRuntime migration compiles; integration compilation now stops at
ndnsf-di-native-assembly.t.cpp:341, which calls unavailable runtimeMetricsSnapshot().
No complete suite ran. Preserve real ORT load/execution proof while migrating
the check to the current runner contract. See [full build R2](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r2).

**Spec181 T008 adoption C R1 (2026-09-06): unadopted GPU-source dependency.**
Fifteen checks stop at fixture compilation on the absent CudaDeviceIdentity header; one
source assertion exposes the old GPU metadata path. Inspection confirms this
draft targets uncommitted CUDA/profile changes. Preserve it for T010/T011
preparation, retain CPU-only local claims, and carry the concrete GPU evidence
gap into delivery. No production source was changed. See [Batch C](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-c).

**Spec181 T008 test adoption B R2 (2026-09-06): focused PASS.**
All 19 application/Merge checks pass without skips using explicit signed
canonical inputs. The stale timeout assertion follows the existing request
budget contract; no production deadline changed. Five local test/tool
dependencies remain. See [Batch B](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-b).

**Spec181 T008 test adoption B R1 (2026-09-06): stale source assertion.**
Eighteen checks pass; one expects a historical hard-coded User no-progress
timeout. Inspect its current parameter source before migrating the assertion.
No network attempt occurred. See [Batch B](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-b).

**Spec181 T008 runtime test migration R2 (2026-09-06): focused PASS.**
The final two-file target builds and passes 30 cases / 225 assertions.
Real verified grants now cover publish/fetch rejection and host/device lease
cleanup; credential-free checks remain fail-closed. Full-suite build and
source closure remain open. See [runtime test migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r1-and-runtime-test-migration).

**Spec181 T008 full test build R1 (2026-09-06): stale test API.**
Compilation stops at the old ProtectedRuntime revoke/revoked calls; no full
suite executed. Keep revocation transferred, migrate current fail-closed
tests, and retain dataflow/zeroization assertions using real BoundGrantFixture.
See [runtime test migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r1-and-runtime-test-migration).

**Spec181 T008 test adoption R3 (2026-09-06): focused PASS.**
The final four-file projection passes all 32 checks without skips after
removing the rejected temporary-path fallback. Source/test bytes match the
isolated projection; the complete C++ test-target build continues separately.
See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 adoption checkpoint (2026-09-06): commit hook blocked.**
The staged role-assembly test retained a development-temporary-path fallback.
No commit was created. Remove that fallback, keep explicit input validation,
and rerun the focused checks before committing. See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 test adoption R2 (2026-09-06): focused PASS.**
All 32 checks pass without skips using explicit canonical-package/registry
inputs. The four adopted files cover ACK provenance, truthful negative
verdicts, certified role assembly, and input/terminal ownership. Fifteen
other draft-test dependencies remain to review before complete qualification.
See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 test adoption R1 (2026-09-06): 31 PASS, one input failure.**
The role-assembly regression's hard-coded isolated registry lacks its
catalogue-authority public-key file. Failure precedes assembly at signature
preflight. Bind the test to the explicit package and registry already used by
R19, retaining signature verification. See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 case-configuration R2 (2026-09-06): focused PASS.**
All 76 inventory/supervisor checks pass, including actual per-case child
configuration and pre-launch input-drift rejection. The complete T008 gate
still needs test-source/build closure and final case inputs; R19 remains the
completed T005 subject. See [R2 review](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#focused-configuration-r2-and-review).

**Spec181 T008 case-configuration R1 (2026-09-06): focused RED.**
Eleven assertions expose missing declaration validation, unbound case-policy
bytes, and absent per-case child configuration; four existing input-drift
checks pass. The repair stays in the local inventory/supervisor boundary.
See [configuration preflight](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#focused-configuration-r1).

**Spec181 formal Y-N R19 (2026-09-06): PASS; T008 preflight remains open.**
The maintained CLI completes all seven subcases, including three real grant
variants, on ce6a4ba0. All 63 application child exits are collected; source and
input identities remain unchanged, and no recorded PID or NFD remains.
See [R19 qualification](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-y-n-matrix-current.md#current-r19-qualification).
The next complete local gate needs case-specific configuration binding and
test-source closure; see [T008 preflight](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md).

**Spec181 exact-wire native R1 (2026-09-06): PASS.**
Committed ce6a4ba0 passes maintained native build and independent verify.
Both focused regressions also pass against the refreshed Core (21 and 270
assertions). The affected 12-dimension convergence review restores A05 PASS;
proceed to a fresh R19 matrix, retaining all earlier failures. See
[native review](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#native-identity-r1-and-convergence-review).

**Spec181 exact-tensor R6 (2026-09-06): focused PASS.**
Nine real DI/Provider/IMS cases pass 270 assertions: 1.4 MB compact transfer
fits signed packets (maximum 8,477 bytes), legacy format reconstructs, and
signature/commitment/context/bounds plus authenticated inner HMAC/index/legacy
binding mutations reject at their named boundaries. Commit the shared repair,
refresh full native identity and re-audit before a new formal matrix. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r5-and-authenticated-inner-rejections-r6).

**Spec181 exact-tensor R4 (2026-09-06): legacy fixture exceeds packet limit.**
Large compact transfer and four real verifier rejections pass (5/6 cases).
The legacy fixture's 7,000-byte payload segment plus old metadata reaches
10,555 signed bytes; Core correctly rejects before decoding. Use small legacy
segments to test compatibility, retaining the unchanged 1.4 MB compact case.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r3-and-negative-probe-r4).

**Spec181 exact-tensor R2 (2026-09-06): test bridge correction required.**
Compact production transfer reconstructs the 1,400,017-byte object and all
signed packets fit; 412/413 assertions pass. The sole failure counts 404
packets versus 202 because both fixture peer bridges and manual bridges run.
Disconnect fixture peer bridges before custom forwarding; keep the same
packet-count, content and size assertions. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-probe-r2).

**Spec181 exact-tensor R1 (2026-09-06): semantic RED at publication.**
The real DI publishOutput of a 1,400,017-byte tensor creates a 17,546-byte
signed manifest and correctly fails the repaired Core limit. Build passed;
the failure is the remaining codec boundary. Apply compact exact encoding
with authenticated reconstruction and legacy compatibility before retry.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-regression-r1).

**Spec181 exact-wire Core R2 (2026-09-06): focused PASS.**
Initial inventory rendering separately rejected the new evidence file's missing
layer header; add the explicit scoped header before rerunning that document check.
The production Core rebuild and unchanged Provider/IMS regression pass all
21 assertions: 8,799/8,800-byte signed packets are readable; 8,801-byte packets
reject and a late batch size failure exposes no earlier item. DI compact
representation/consumer repair remains BLOCK before native refresh and matrix.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#core-repair-r2).

**Spec181 exact-wire Core R1 (2026-09-06): semantic RED reproduced.**
The real Provider/IMS pull regression passes 18/21 assertions but accepts an
8,801-byte signed Data and leaves an earlier batch item readable after a late
oversize item. Adopt full signed-wire prevalidation for the whole batch, then
repeat the unchanged test in a fresh run. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#core-regression-r1).

**Spec181 R18 diagnosis (2026-09-06): BLOCK at NDN Data wire size.**
The first actual send boundary is now identified: BackboneNeck's exact
MANIFEST Data encodes to 19,658 / 10,883 bytes and a SEG Data to 14,191,
above ndn-cxx's 8,800-byte limit (169 event-loop exceptions). The exact
NDNSF-DI filter exists; downstream deadlines are consequences. Review the
existing compact transport changes and pre-publication size guard as a
bounded shared unit, retaining signature/content commitments. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18).

**Spec181 T005 formal R18 (2026-09-06): BLOCK at exact dependency transfer.**
BackboneNeck now executes with actual CPU ONNX evidence and completes its
role. DetectShard0/1 cannot fetch its exact tensor manifests; Merge then
times out on their outputs. Inspect publication, cache response and routing
before another run. Source/input identities remain unchanged and NFDs exit.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18).

**Spec181 backend native refresh R1 (2026-09-06): PASS.**
Committed 214df1d6 completes maintained native build and independent verify;
both report native identity OK. The tested registration change and retained
device/error contracts pass affected convergence review. Resume a new formal
matrix; focused CPU checks do not establish matrix qualification. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-native-identity-r1).

**Spec181 backend repair R4 (2026-09-06): focused PASS.**
The maintained native Provider rebuilds (35.522 s); five real executable
checks pass (1.02 s). Legacy/public CPU names load and warm a real ONNX
model; unknown names and invalid execution-provider metadata still reject.
Commit only registration blocks and tests, then refresh native identity and
re-audit before the next matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-repair-r4).

**Spec181 backend probe R3 (2026-09-06): semantic RED reproduced.**
With corrected evidence assertions, legacy CPU load/warmup and unknown-backend
rejection pass; three public-name checks fail at missing registration. Adopt
only the two registration blocks, rebuild the maintained Provider and repeat
the real executable checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-regression-r3).

**Spec181 backend probe R2 (2026-09-06): registration RED and assertion correction.**
Three checks reach missing public backend registration. The legacy backend
actually loads/warms the model, but its test misreads existing string-valued
evidence and nested device fields. Unknown-backend rejection passes. Correct
the schema assertion before the next focused run. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-probe-r2).

**Spec181 backend probe R1 (2026-09-06): BLOCK at test collection.**
An extra parenthesis in the new test prevents collection (0.36 s); no native
process ran. Correct the test syntax and retain R1 before a fresh R2 probe.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-probe-r1).

**Spec181 T005 formal R17 (2026-09-06): BLOCK at backend registration.**
All four Providers pass external assignment validation and enter their handler.
BackboneNeck then fails with no NativeModelRunner backend registered:
onnxruntime-cpu; dependent-role fetch deadlines follow. Preserve that first
boundary, repair actual backend registration and re-audit before retry. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#native-backend-boundary-r17).

**Spec181 digest native rebuild R1 (2026-09-06): PASS.**
Committed e6f44b65 rebuilds the Core library, native Provider and Python
extension; maintained build and independent verify both report native identity
OK. Source/byte checks and exact assignment rejection remain intact. Affected
convergence review PASS; proceed to a fresh formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-native-rebuild-r1).

**Spec181 digest repair R3 (2026-09-06): focused PASS; native rebuild pending.**
Four actual C++ helper checks pass (1.73 s) after the isolated Provider emits
canonical lowercase hex. Exact assignment size/digest checks remain intact.
Commit this unit, rebuild the native runtime and re-audit before a new matrix.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-repair-r3).

**Spec181 digest probe R2 (2026-09-06): RED reproduced.**
All four actual-helper comparisons fail only at Provider uppercase hex;
User matches independent SHA-256. Normalize Provider output to the existing
canonical lowercase contract, preserving exact byte/size checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-regression-r2).

**Spec181 digest probe R1 (2026-09-06): BLOCK at linker selection.**
The focused helper probe fails at compilation/linking (4 setup errors,
1.60 s), before digest comparison: system g++ selects Homebrew ld from PATH.
Use the system toolchain explicitly and retain the original log. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-probe-r1).

**Spec181 T005 diagnostic R16 (2026-09-06): BLOCK at external assignment validation.**
Core INFO logging shows each Provider receives and queues Selection, then
fails assignment preparation with external collaboration assignment size or
digest mismatch. The duplicate request log is not the controlling boundary.
Inspect assignment publication, fetch and validation on the same source.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#external-assignment-boundary-r16).

**Spec181 T005 formal R15 (2026-09-06): UNQUALIFIED after Selection commit.**
The sealed-plan repair reaches four-role Selection commit, then the User
receives REMOTE_RESPONSE_FAILED. Provider WARN logs contain duplicate request
rejections but no native execution failure reason; this does not yet identify
the controlling cause. Inspect the Core request/Selection boundary and obtain
bounded diagnostic logs before changing behavior. Source/input identities
stay unchanged and all NFDs exit. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-response-boundary-r15).

**Spec181 sealed-plan repair R3 (2026-09-06): focused PASS.**
All 22 sealing/candidate checks (0.97 s) and 36 existing plan integration
checks (0.77 s) pass in the isolated source. Fetch references remain separate
from canonical identity, immutable and digest-bound; malformed values reject
before commit, while legacy/local-preparation defaults remain compatible.
Affected A05 review PASS; resume a new formal matrix after committing. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-repair-r3).

**Spec181 sealed-plan validation R2 (2026-09-06): BLOCK at reference validation.**
The projected field resolves sealing; five normal/legacy/coverage checks pass.
Five malformed reference checks fail because non-string false values and
control characters are accepted. Require strings and reject control chars,
retaining the deliberate empty local-preparation reference. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-validation-r2).

**Spec181 sealed-plan regression R1 (2026-09-06): RED reproduced.**
Ten focused checks reproduce the missing field at the actual V3 sealing
expression (1.72 s). Project the existing shared contract and validate
transport forwarding, digest binding, legacy defaults and invalid references.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-regression-r1).

**Spec181 T005 formal R14 (2026-09-06): BLOCK at sealed-plan reference closure.**
The shared assembly repair permits request planning to reach plan sealing.
The committed SealedCollaborationPlan lacks artifact_fetch_data_names,
already consumed by placement. Review/adopt the existing field and digest
binding, verify plan production/consumption, then re-audit. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-boundary-r14).

**Spec181 canonical binding/assembly repair R6 (2026-09-06): focused PASS.**
The isolated Waf parity target builds successfully; all 40 canonical,
candidate and actual C++/Python assembly checks pass (12.91 s). Actual YOLO
two-candidate binding, recipe and publication-port checks pass as well.
Affected convergence review PASS. Commit only the tested shared CPU unit,
then resume a fresh formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-and-assembly-repair-r6).

**Spec181 canonical assembly R5 (2026-09-06): BLOCK at focused harness inputs.**
Thirty-two Python checks pass. Eight native checks lack the required parity
binary; the extended recipe probe incorrectly includes the non-ONNX Merge
role in its graph assertion. Correct those focused harness inputs before
further validation; no network attempt was made. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-assembly-boundary-r5).

**Spec181 canonical recipe R4 (2026-09-06): BLOCK after binding repair.**
R3 actual binding/publication and 24 focused checks pass. Extending the probe
through real role certification reveals that the committed recipe rejects
COMPONENT_SET zero intervals. Add the reviewed component/external-initializer
assembly changes to this source unit; retain unrelated CUDA provider selection
outside the unit. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-repair-r3-and-recipe-boundary-r4).

**Spec181 canonical-binding regression R2 (2026-09-06): RED reproduced.**
With the complete Python path and private offline NDN environment, actual
YOLO describe reproduces the missing canonical_graph_digest TypeError.
Proceed with the reviewed shared dependency unit and focused checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-regression-r2).

**Spec181 canonical-binding regression R1 (2026-09-06): BLOCK at probe import.**
The isolated probe lacks the Repo Python path and stops at SDK import before
the binding constructor. Complete the explicit Python/private NDN environment
for the next focused run. The source/reference contract,
shared deployment consumer and their tests form the reviewed dependency unit.
Validate that unit plus actual two-candidate publication before re-auditing.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-regression-r1).

**Spec181 T005 formal R13 (2026-09-06): BLOCK at canonical artifact binding closure.**
The epoch repair permits the User to request a V3 task. YOLO describe passes
canonical_graph_digest to the committed CanonicalArtifactBinding, whose
dataclass lacks that field. The working tree contains the shared canonical
reference extension, while the isolated commit omits it. Review and validate
the complete binding/publication dependency before another run; all NFDs
exited and source/input identities stayed unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-boundary-r13).

**Spec181 subcase-epoch repair R2 (2026-09-06): focused PASS.**
All 117 runner/matrix/grant seam checks pass (3.99 s). Child epoch now matches
the publication/Provider runtime inputs; the protected Y-B and all three
Y-N-E mutations retain their grant configuration, and the parent environment
is unchanged. Affected convergence review PASS; T005 needs a new formal run.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-repair-r2).

**Spec181 subcase-epoch regression R1 (2026-09-06): RED reproduced.**
Seven plaintext cases inherit the protected matrix epoch; four protected
profiles pass. The focused production runner capture stops before network
startup (7 failed / 4 passed / 88 deselected, 2.06 s). Bind child epoch to the
same runtime inputs used by publication and Provider process specifications.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-regression-r1).

**Spec181 T005 formal R12 (2026-09-06): BLOCK at subcase epoch environment.**
Committed fixture loading passes. Y-N-O User inherits the matrix's protected
epoch, although runtime publication and process specifications choose the
plaintext control epoch; its grant seam then raises SPEC181_REQUESTER_PRIVATE_KEY
KeyError. Scope the child epoch to the selected subcase and regression-test
both plaintext controls and protected Y-N-E before rerunning. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-boundary-r12).

**Spec181 fixed-input closure repair (2026-09-06): focused PASS.**
The existing PPM and provenance README are adopted without byte changes.
All 22 numerical regression checks pass (7.97 s); the actual canonical
package reference loader verifies the fixture digest and shapes. A05 is
re-audited PASS; verify the committed fixture in the isolated checkout
before resuming the formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#fixture-closure-repair).

**Spec181 T005 formal R11 (2026-09-06): BLOCK at fixed fixture source closure.**
The repaired lock permits Controller publication, Repo, and four native
Providers to become ready. User startup fails because the isolated commit
lacks tests/fixtures/spec180/yolo26n/fixed-fixture.ppm. The existing untracked
162-byte fixture matches the manifest digest. Reopen A05, adopt the fixture
and its provenance, validate the real reference loader, then re-audit before
the next matrix. Source/input identities stayed unchanged; NFDs exited. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#fixed-input-source-closure-r11).

**Spec181 T005 formal R10 (2026-09-06): BLOCK at Controller publication initialization.**
The explicit shell environment repaired process spawning. NFD readiness and
Controller startup pass, but the co-located publication ServiceUser constructor
throws `Failed to acquire file lock`. The maintained matrix exits 2 at Y-N-O;
The focused syscall probe finds EACCES before flock: the UID-0 lock path is
owned by UID 1000, with no kernel lock or fuser occupant. Preserve this stale
file in R10 before letting the runtime recreate it; no Core change is needed.
no protocol result is established. Source/input identities remain unchanged,
and all NFD processes exited. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#controller-publication-boundary-r10).

**Spec181 T005 formal R9 (2026-09-06): BLOCK at explicit shell environment.**
The new diagnostic identifies KeyError in Mininet node.py:419: shell=True reads
os.environ['SHELL'], absent from the launch environment. Preserve the exact
frame chain; set SHELL=/bin/bash explicitly for R10. No Controller was launched,
all NFDs exited, and source/input identities stayed unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-application-boundary-r9).

**Spec181 spawn diagnostic repair R3 (2026-09-06): focused PASS.**
All 93 runner/matrix checks pass. Partial-start cleanup and first-failure stop
remain intact; the new exclusive diagnostic records type and frame locations
without exception text or locals. The actual R8 spawn cause still requires a
new run. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-repair-r3).

**Spec181 spawn diagnostic R2 (2026-09-06): BLOCK at test import.**
Two-file checks produce 1 failed / 92 passed (1.55 s). The runtime writes its
new diagnostic; the new assertion lacks the json import. Preserve R2, add the
test import, and rerun. This is a test-fixture failure, not a runtime result.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-regression-r2).

**Spec181 spawn diagnostic regression R1 (2026-09-06): RED reproduced.**
The existing partial-start cleanup test now verifies a durable error boundary;
it fails because process-start-failure.json is absent (1 failed / 87 deselected,
0.90 s). Keep cleanup and failure verdicts intact; record only exception type
and frame locations, preserving first evidence without messages or locals.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-regression-r1).

**Spec181 T005 formal R8 (2026-09-06): BLOCK at application spawn diagnostics.**
NFD readiness/routing/keychains pass. Controller log creation is followed by an
immediate spawn failure; the matrix preserves only CONTROL_NOT_PROVEN and drops
the underlying traceback boundary. Preserve R8 and add exception type plus
file/function/line frames, without exception text or locals, before another
diagnostic run. NFD cleanup was verified. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-application-boundary-r8).

**Spec181 T005 formal R7 (2026-09-06): BLOCK at NFD readiness.**
All five NFD sockets exist, but node nfdc checks fail; no application child
started. The launcher inherited offline probe NDN_CLIENT_* overrides pointing
to unused.sock instead of node client.conf. Preserve startup diagnostics and
logs. R8 will isolate the parent via private HOME, remove the global overrides,
and retain explicit Python dependency paths. NFD/native Provider cleanup was
verified. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-network-boundary-r7).

**Spec181 T005 formal R6 (2026-09-06): BLOCK at Mininet executable readiness.**
The explicit PATH omitted sbin and Mininet could not find ifconfig (exit 1).
Preserve R6. Verify required network tools and append the system sbin paths for
R7 while preserving Python/native resolution order and recording the new launch
environment. This is startup readiness, not a protocol result. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r6).

**Spec181 T005 formal R5 (2026-09-06): BLOCK at launch environment parsing.**
The temporary parser rejected the registered SPEC180_CASE_OUTPUT_DIR in the
Y-N environment. No case/state or runner was created. Preserve R5; accept that
specific field and override it with R6's unique output. The five-role Y-N input
uses the same model; the protected epoch remains explicit for Y-N-E. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r5).

**Spec181 T005 formal R4 (2026-09-06): WAITING_EXTERNAL_INPUT at case roles.**
Envelope ownership now passes. The Y-B baseline configuration lacks Y-N's
required FullModel capability, so maintained validation exits 78 before network.
Preserve R4; inspect and use the existing Y-N-specific inputs for a new R5.
The registered role-set requirement remains unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r4).

**Spec181 T005 formal R3 (2026-09-06): WAITING_EXTERNAL_INPUT at key ownership.**
The repaired sudo source gate passes on 88e7a458. Maintained input validation
rejects the developer-owned envelope key for root execution (exit 78), before
network. Preserve R3 and provision the same bytes as a 0600 root-owned file in
R4's private state, retaining the original key untouched. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r3).

**Spec181 sudo source repair R2 (2026-09-06): PASS; formal R3 next.**
The gate preserves SUDO_UID only for root plus the actual selected checkout
owner. Real sudo positive/negative tests and existing local gate regressions
pass: 51 checks (8.68 s), with Git overrides still stripped. A05 is re-audited
PASS; T005 remains incomplete and will use a new run directory. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sudo-source-repair-r2).

**Spec181 sudo source regression R1 (2026-09-06): RED reproduced.**
Real sudo Git checks yield 1 failed / 1 passed: the legitimate owner is rejected,
while the wrong UID remains rejected. The test also supplies hostile GIT_DIR
and GIT_INDEX_FILE overrides. Preserve red.log and repair only the matching
sudo-owner identity in the sanitized environment. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sudo-source-regression-r1).

**Spec181 T005 formal R2 (2026-09-06): BLOCK at sanitized source Git.**
Outer Git now accepts the actual sudo user, but production _source_git drops
SUDO_UID again and fails before network. Reopen A05's sudo checkout boundary;
add a real sudo regression and retain the UID only when it matches the selected
checkout owner. All Git override variables remain stripped. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r2).

**Spec181 T005 formal R1 (2026-09-06): BLOCK at launcher Git ownership.**
The explicit environment dropped SUDO_UID; root Git rejects the user's checkout
before invoking the maintained runner. Preserve R1. Retain the actual sudo
caller UID in the explicit launch environment for R2; do not write global Git
exceptions or weaken the source gate. No network started. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r1).

**Spec181 T007 convergence (2026-09-06): PASS; T005 next.**
A05's source/configuration/input/build/application boundaries now map to their
focused regressions and final committed checks. A01–A12 are closed within their
documented scopes. The 12-principle audit permits same-source local validation;
it is not qualification or development-delivery PASS. Historical failures below
remain preserved. See [audit](../specs/181-ndnsf-di-protected-grant-qualification/audit.md#a05-closure-matrix).

**Spec181 committed source R13 (2026-09-06): R12 resolved.**
Repo build intermediates are preserved outside the checkout. The strict source
guard passes for 6b9bb51c, and the real application/native preflight plus four
application imports pass on that commit without network. Overall T007 audit
remains the next gate. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#committed-source-and-runtime-r13).

**Spec181 committed source R12 (2026-09-06): BLOCK at build intermediates.**
The isolated checkout matches 6b9bb51c with a clean tracked/index tree. The
actual source gate rejects untracked Repo setup build/src/ArtifactManifest.o.
Preserve the terminal result and move the complete intermediate build directory
to R12 before retry, retaining runtime extension and strict source checks. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#committed-source-reconciliation-r12).

**Spec181 source closure R11 (2026-09-06): focused failures resolved.**
The same isolated selected source passes 45 existing checks and 12 new candidate
binding checks. Actual application/native preflight and four application imports
pass without network. R1–R10 remain below as historical first-boundary evidence.
Final committed-source reconciliation and T007 audit remain open. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#focused-closure-r11).

**Spec181 focused source closure R10 (2026-09-06): BLOCK at request contract.**
Actual application/native preflight passes without network. Six-file checks
yield 9 failed / 36 passed (1.38 s): missing DIRequestEnvelopeV2 input transport
fields and InferenceApplication task arguments. Preserve R10 and close the two
request endpoints before retry. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-and-focused-r10).

**Spec181 application preflight R9 (2026-09-06): BLOCK at verifier implementation.**
Export alone is insufficient: ProviderOfferTrustVerifier itself is absent from
committed SDK provider.py. Preserve R9 and validate the implementation and its
existing signature/ACK tests as part of source closure. No network ran. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r9).

**Spec181 application preflight R8 (2026-09-06): BLOCK at public verifier export.**
Actual publication/process_specs/native guard pass. The post-guard import probe
then finds user.py requires ProviderOfferTrustVerifier missing from SDK exports.
Preserve R8 and add the existing verifier export; no network ran. Native guard
success alone does not prove application import closure. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r8).

**Spec181 application preflight R7 (2026-09-06): BLOCK at local launch helper.**
Runtime publication passes. Actual process_specs calls legacy python_cmd with
repo/py_dir, which the committed helper lacks. Close the matching local helper
parameterization, retaining default compatibility. Preserve R7; no network ran.
See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r7).

**Spec181 application preflight R6 (2026-09-06): BLOCK at catalogue conversion.**
SDK/Provider imports pass. Runtime publication requires the uncommitted
PreSplitCatalogSnapshot.from_mapping contract. Preserve R6 and close the
matching validation/serialization dependency before retry; no native guard or
network ran. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r6).

**Spec181 application preflight R5 (2026-09-06): BLOCK at Repo reference contract.**
The selected ApplicationInput contract requires LargeDataReference, absent from
committed repo_reference.py. Existing Provider/client/facades already consume
this shared publication/reference owner. Preserve R5 and close that dependency;
no network ran. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r5).

**Spec181 application preflight R4 (2026-09-06): BLOCK at shared input contract.**
Candidate construction now passes. SDK imports then fail because the committed
Provider requires MAX_INLINE_INPUT_BYTES absent from adapters.base; the
coordinator also requires InputTransportMode. Preserve R4 and close the shared
input contract/export dependency before retry. No native guard/network ran.
See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r4).

**Spec181 application preflight R3 (2026-09-06): BLOCK at shared candidate contract.**
Repo's same-source build and import pass. Production YOLO publication then
constructs SplitCandidate with selection_priority, absent from the committed
contract, and fails before native guard/network. Preserve R3; close the exact
shared contract dependency without mixing unrelated worktree changes. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r3).

**Spec181 application preflight R2 (2026-09-06): BLOCK at Repo Python extension.**
Adding four existing YOLO source files to the isolated checkout clears actual
catalogue verification. Policy generation then cannot import
py_repoclient._py_repoclient, which has not been built in that checkout.
No native guard/network started. Preserve R2; use Repo's maintained build
against the same source, not an unknown worktree binary. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r2).

**Spec181 application preflight R1 (2026-09-06): BLOCK at committed Python adapter import.**
The clean a51f87b3 native build passes and emits its new receipt. Actual runner
validate_inputs then fails to import build_yolo26n_adapter from adapters.yolo,
wrapped as CANONICAL_CATALOGUE_VERIFY_FAILED. No network or native guard was
started. Preserve R1 and repair the committed adapter package closure before
retry. See [local runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r1).

**Spec181 local input identity R3 (2026-09-06): BLOCK at new fixture import.**
After R2 66 PASS, new input tests yield 3 failed / 69 passed (7.68 s): three
tests reference json without importing it, before the identity owner runs.
Real-child drift/collection checks pass. Preserve R3, repair the fixture import,
then rerun. See [local input identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-input-identity-20260906.md#focused-r2-and-fixture-failure-r3).
R4 fixes the missing import: 72 PASS (8.40 s). Actual fixture children preserve
completed results and reject input drift before network cases or at final
aggregation. Both CLI discovery paths consume the explicit environment.
Configured input identity unit CLOSED; actual runtime audit remains T007 work.

**Spec181 local input identity R1 (2026-09-06): BLOCK at external input bytes.**
Model/map/referenced-key replacement and package additions reach the forbidden
child boundary after inventory creation (4 failed); launch configuration binds
path strings only. No qualification child ran. Preserve R1 and bind the actual
external inputs in the shared inventory/gate owner. See
[local input identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-input-identity-20260906.md).

**Spec181 explicit config root R1 (2026-09-06): BLOCK at protected launch selection.**
The runner overwrites explicit NDNSF_SPEC180_CONFIG_ROOT with the HOME default.
Absolute/relative overrides and missing-key rejection fail (3 failed / 1 passed,
0.81 s); fixture stops before native preflight/network. Preserve R1 and honor
the explicit root before child HOME changes. See
[explicit configuration root](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-explicit-config-root-20260906.md).
R2 honors the explicit root, resolves it before child HOME/cwd changes, and
rejects a missing explicit key even when the default key exists: 24 focused
checks PASS (0.85 s). Configuration selection unit CLOSED; T007 remains open.

**Spec181 Waf tool identity R3 (2026-09-06): BLOCK at CLI fixture PATH mismatch.**
1 failed / 79 passed (1.81 s): an old CLI linkage test builds with fixture PATH
then verifies with ambient PATH. The new Waf identity check correctly rejects
that mismatch first. Align the CLI fixture environment and retain its original
wrong-Core rejection assertion. R3 log and patch preserved in
[Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md#cli-fixture-boundary-r3).
R4 repairs the CLI fixture: 80 PASS (1.75 s). Actual Waf directory selection
matches the new owner in both current and isolated checkouts (80 source/resource
files each). Waf source/selection unit CLOSED; runtime/input closure remains
T007 work. Existing native receipts require a maintained rebuild for the new field.

**Spec181 Waf tool identity R2 (2026-09-06): BLOCK at fixture executable identity.**
57 failed / 18 passed (2.77 s): the existing fake interpreter lacks its
executable bit, so real PATH resolution rejects it before mocked build; one
environment assertion also predates child-only WAFDIR. Preserve R2 and repair
fixtures before retry, without relaxing production resolution.
See [Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md#fixture-boundary-r2).

**Spec181 Waf tool identity R1 (2026-09-06): BLOCK at generated build-tool identity.**
Four waflib content/location mutations escape native verification; interpreter
drift is rejected only after the native probe. Focused fixture checks: 5 failed,
70 deselected (0.36 s), no real build/network/qualification process.
Preserve R1, then bind actual selected Waf implementation in the maintained
native identity owner. See [Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md).

**Spec181 local configuration R1 (2026-09-06): BLOCK at launch identity.**
Six focused mutations of environment, reserved output variable, declared
digest and interpreter bytes reach the forbidden qualification-child boundary
(6 failed, 0.78 s). The runner already uses explicit environment; the missing
check binds its actual values to the declared digest. No qualification child
ran. Preserve R1 before repairing the shared launch-configuration owner.
See [local configuration identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-config-identity-20260906.md).
R2 binds actual launch inputs through one shared builder/gate owner (52 PASS).
R3 verifies real child environment consumption and post-execution identity
failure (61 PASS). R4 adds builder/gate CLI round-trip and rejects changed
configuration before output/children: 62 PASS (5.80 s). Launch configuration
unit CLOSED; T007 remains BLOCK at runtime/input-byte identity planes.

**Spec181 revision 7 (2026-09-06): BLOCK at local configuration identity.**
Committed-source validation is closed in 2628e3d2 (39 focused checks and
the configured checkout PASS). The remaining controlling work binds actual
local configuration, build/runtime dependencies and development delivery.
By owner decision, SIF/replay/Tiger move to the experiment machine and are
not local closure prerequisites. Git merge is deferred until development ends.
See [scope transfer](../specs/181-ndnsf-di-protected-grant-qualification/evidence/development-scope-transfer-20260906.md)
and [current tasks](../specs/181-ndnsf-di-protected-grant-qualification/tasks.md).

**Spec181 local gate identity R8 (2026-09-06): checkpoint hook rejection.**
The local commit hook rejects assistant-directory references in production
source validation. Remove those non-product exclusions and rerun the focused
checks; do not bypass the hook. No checkpoint was created by the failed commit.
R8 removes the exclusions: 39 focused checks PASS (4.46 s), and the actual
configured checkout passes SOURCE_CHECKOUT_OK. The hook remains enabled.
The retry checkpoint succeeds as 2628e3d2; this hook incident is closed.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md#checkpoint-gate-r8).

**Spec181 local gate identity R7 (2026-09-06): source unit CLOSED; configuration BLOCK.**
All 39 focused checks pass (3.44 s), and the configured 1ba99000 checkout
passes exact source validation. Bad preflight identity has no qualification
child/output side effects; mutation during fixture execution yields
UNQUALIFIED while preserving child/cleanup records. Effective configuration,
generated build-tool/runtime bytes and external import bindings remain open.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md#focused-green-r7).

**Spec181 local gate identity R6 (2026-09-06): BLOCK at Python compatibility.**
New symbolic-link checks use Path.is_relative_to, absent from the maintained
interpreter. Focused checks yield 23 failures / 16 passes; the subsequent
read-only checkout probe hits the same AttributeError before qualification.
Preserve R6; use relative_to with ValueError handling, then require focused
success before the next checkout probe.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).

**Spec181 local gate identity R4 (2026-09-06): BLOCK at Git LFS representation.**
The configured 1ba99000 checkout is rejected because a committed 134-byte LFS
pointer represents a materialized 240376592-byte release archive. Preserve
R4 before adding exact pointer size/SHA-256 verification; do not classify this
as source tampering or a protocol failure. No qualification child ran.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).
R5 closes exact LFS-byte verification and passes 33 focused checks (2.51 s).
The next configured-checkout rejection is generated Waf tool code; classify
only its exact generated layout under the separate build-tool identity plane,
whose byte binding remains an A05 obligation. Preserve both R5 logs.

**Spec181 local gate identity R1 (2026-09-06): BLOCK at source authority.**
The real local gate accepts a fixture root without a Git HEAD and an invented
40-character sourceRevision, then reports PASS for six fixture children.
All six exits/cleanup records are collected; no network qualification ran.
Validate actual checkout/source identity before any qualification child or
output directory is created, then retain focused regressions for rejection.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).
R2 adds real Git fixtures and nine identity mutations; all nine reach the
forbidden child boundary instead of being rejected (9 failed, 0.63 s).
No qualification child runs; preserve the RED log and source patch before
adding checkout, index, tracked-byte and untracked-code checks.
R3 passes all nine rejections and existing gate/inventory checks (23 PASS,
2.39 s). Actual configured-checkout and submodule boundaries remain under
focused review; configuration/candidate identity is not yet closed.

**Spec181 native plan closure R1 (2026-09-06): BLOCK at projection behavior.**
Twelve missing header lines close the maintained local native build, including
the extension import/identity check. Focused plan/merge tests then yield
27 PASS / 2 FAIL: COMPONENT_SET postprocessing is rejected, and two PIPELINE
tensors collide in runtime scope with mismatched producer/consumer names.
Preserve R1 before applying the exact parser/scope repair; no qualification
matrix or model run was started.
R2 closes the parser/scope unit: 29 cases / 133 assertions PASS, including
unchanged transport authorization groups. Refresh native identity from its
source checkpoint before advancing the remaining A05 candidate/config audit.
The 1ba99000 checkpoint subsequently passes the maintained native build,
including a fresh extension import and runtime identity receipt (R3).
See [native plan closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-native-plan-closure-20260906.md).

**Spec181 native source diagnostic R3 (2026-09-06): BLOCK at DI projection declaration.**
A checkout refusal left the first native retry on the prior HEAD plus an
explicit source patch; that run is invalid as clean-commit evidence. It
terminated with two compiler errors: NativeCanonicalOnnxAssembler reads
canonicalArtifactName absent from the committed NativeSelectionProjectionV3.
The nine tested files were then matched to 1df718c8 and checkout completed.
Inspect the declaration and assignment path before a fresh recorded retry.
See [framework source closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-framework-source-closure-20260906.md).

**Spec181 framework source closure R1 (2026-09-06): BLOCK at assignment metadata.**
The isolated six-file dependency closure builds the production framework
library. Its reused lifecycle/assignment checks yield 16 PASS / 1 FAIL:
ServiceProvider loses artifactDataName while projecting a structured
assignment set into CollaborationContext. Preserve the R1 source patch and
result, then close the exact Provider transfer before retrying.
R2 carries the root name through single/structured assignments and rejects
conflicting roots. The isolated target links and passes 26 cases / 204
assertions. This framework boundary is closed; full native closure remains.
See [framework source closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-framework-source-closure-20260906.md).

**Spec181 committed native build R4 (2026-09-05): BLOCK at C++ source closure.**
Clean 6c7a0b23 passes configuration and Waf graph creation, then ServiceUser.cpp
fails to compile: AckAuthenticationEvidence is missing, followed by missing
registration and publish-result declarations. The implementation depends on
uncommitted framework declarations/companions. Preserve R4 and close those
exact dependencies before the next clean build; no protocol result exists.
See [committed native closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-committed-native-build-20260905.md).

**Spec181 committed native build (2026-09-05): BLOCK at Waf graph creation.**
Detached d67de87a configures successfully, but tests/wscript references an
untracked native assembly integration source. find_node returns None and Waf
fails before C++ compilation. No protocol result exists; preserve the isolated
checkout and repair the committed source dependency before retrying.
One focused identity regression also fails: changing loaded tests/wscript
bytes with unchanged mtime does not invalidate the native receipt, because
that Waf control file is missing from source fingerprints.
R2 binds the missing control file (70 identity checks PASS) and builds the
integration target, but its seven named assembly cases yield 3 PASS / 4 FAIL.
All four fail at certified recipe_digest validation before ORT loading;
preserve R2 and compare fixture serialization with the production contract.
R3 corrects the old fixture's quoted integer dimensions, leaving production
digest validation intact: 7 cases / 160 assertions PASS. The missing test
source and identity repair are ready for a checkpoint; a fresh committed
checkout must still pass the maintained native build. T007 remains BLOCK.
See [committed native closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-committed-native-build-20260905.md).

**Spec181 A05 qualification inventory (2026-09-05): CLOSED within focused inventory repair.**
The inherited inventory requires Q-C/Q-W but discovers only Spec180 Python
tests, omitting Spec181 protected-grant regressions. Two tests using the real
inventory builder and pytest collection fail; no formal network run started.
R2 repairs those cases (15 PASS); an old count assertion is corrected, and R3
passes 16 checks. R4 then exposes a second boundary: two tests show rehashed
case-source substitution is accepted by the real builder and gate. These use
fixture children, not a network qualification. Preserve R4 before repair.
R5 rejects both substitutions (17 PASS); the old missing-oracle fixture also
changed its source path and is now correctly rejected earlier. R6 keeps the
registered path while withholding its oracle to preserve that check's scope.
R6 passes 18 checks. R7 then finds three unsafe entry IDs accepted by the
validator although the gate joins IDs into output directories; no escaped
write is attempted. Reject these IDs at the inventory boundary before R8.
R8 passes all 21 checks: active scope/collection, registered case source/args,
safe entry IDs, existing evidence/oracle controls and wrapper compatibility.
The remaining A05 native/candidate effective-configuration audit stays BLOCK.
See [qualification scope repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-qualification-scope-20260905.md).

**Spec181 T007 evidence inventory (2026-09-05): CLOSED within document inventory scope.**
The R003 record reports zero Spec181 evidence files, while the current tree
contains 37. Four active evidence records lack an explicit header layer, and
the audit body still describes already-closed T002/shared-runtime gaps. This
invalidates the old completeness claim, not the linked raw test results.
Four layer headers are repaired; the current inventory covers 145 entries,
including both audit roots and one explicit SELF row. Drift/structure checks
PASS, and all 105 Spec180 evidence-file hashes match the before-scan. Raw scan:
ignored workspace temporary directory `spec181-evidence-inventory-20260905-r1/`.
See [complete inventory](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-evidence-inventory-20260905.md).

**Spec181 shared generation worker (2026-09-05): CLOSED by focused repair.**
Two queued coordinator regressions entered the model after cancellation or
deadline. The existing pre-run cancellation check passed (1/3 cases PASS,
14/18 assertions PASS). Propagating the shared guard through the registered
runtime/worker and state staging repairs the boundary: rebuilt 48 cases /
366 assertions PASS. T007 remains BLOCK for its remaining audit obligations.
See [generation worker authority](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-generation-worker-20260905.md).

**Spec181 shared preparation control R1 (2026-09-05): CLOSED as startup error; R2 focused PASS.**
The isolated launcher omitted the empty output directory; `validate_inputs`
raised `OUTPUT_ROOT_MISSING` before MiniNDN startup. No protocol result exists.
The unified native build passed. R1 is preserved; fresh R2 passed the protected
P-256 control with four verified Providers and seven collected child exits.
See [shared preparation closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-shared-preparation-20260905.md).

**Spec181 T002 native handler observation (2026-09-05): CLOSED; early result INVALID.** A focused test
was started before the repair build completed and ran the previous binary.
R2/green.log is preserved. The original build completed (59.612s), then the rebuilt
binary passed 46 cases / 242 assertions in a new log. The early result is a
validation orchestration error, not a protocol result.
See [native handler closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-handler-closure-20260905.md).

**Spec181 T002 native prepared output binding (2026-09-05): CLOSED (focused repair).** The production
prepared-runner validator accepts a Merge output budget changed from the sealed
Selection's K=300 to K=1. R1 fails one of two assertions after a passing positive
control. Exact output shape/type binding now rejects the mutation; rebuilt focused
checks pass 46 cases / 242 assertions. Source closure remains pending; see
[native handler closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-handler-closure-20260905.md).

**Spec181 T002 native Merge contract (2026-09-05): CLOSED (focused repair).** Direct production-runner
checks pass the numerical controls but expose four missed rejections: unknown
postprocess identity, numeric suffix, trailing shape delimiter, and wrong output
dtype. R1 is preserved (3/6 cases, 22/26 assertions passed). Rebuilt repairs pass
6 cases / 26 assertions; related evidence/readiness checks total 10 cases / 69
assertions. Handler source closure remains open; see
[native Merge closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-merge-closure-20260905.md).

**Spec181 T002 native worker authority (2026-09-05): CLOSED (focused repair).** Four real-worker
regressions show that cancellation/expiry after preparation or during compute
still returns results and retains the registered plaintext lease. Grant
verification is valid initially; the missing boundary is worker consumption.
R1 is preserved. Request guards now fence preparation, compute, cached results,
events, publication, and return; rebuilt focused checks pass 51 cases / 280 assertions.
T002 source closure and unified production rebuild remain open; see [worker authority](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-worker-authority-20260905.md).

**Spec181 T001 request lifecycle (2026-09-05): CLOSED.** Twelve registered-handler
regressions show that cancellation or Selection deadline expiry before/during
grant fetch or after preparation still reaches model execution. Grant expiry
alone does not enforce request lifetime. Preserve the first red run before
repair. Four final regressions also expose a missing comparison between the
grant-reference and Selection policy snapshot. Both repairs pass 151 focused
regressions and six real Python process cases (24 exits collected). All red
runs are preserved. T001 acceptance is complete; T002/T007 remain open. See
[request lifecycle](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-request-lifecycle-20260905.md).

**Spec181 T001/T002 maintained process integration (2026-09-05): focused defects CLOSED.**
R1 native fixtures build, but NFD's own management FIB registration fails
because the test configuration omits management authorization. The requester
then receives connection refused; no grant verification occurred. Preserve
the r1 raw run. R2 fixes NFD but requester bootstrap requires a real Controller
for NAC public parameters. Separately, ten successful P-256 runtime calls leak
24560 bytes / 630 allocations under ASAN despite the Zeroized state. Both
boundaries are recorded before repair. R3 fixes the EC ownership leak: ten
P-256 calls pass ASAN. The real Controller becomes ready, but requester
publication readiness times out before any Provider result; preserve r3
before instrumenting that boundary. R4 NDN logs/stack locate the wait in
ServiceUser construction (NAC decryption key): the fixture needs its own
requester policy and certificate bootstrap. R5 passes five cases; Python's
wrong-recipient rejection is correct, but the test incorrectly requires a
Core wire prefix on an internal typed exception. Preserve r5 and correct
the scoped assertion without synthesizing a Core response. R6 passes all 11
checks (10 real-process cases plus ASAN), with all 40 child exits collected.
T001/T002 full acceptance and T007 remain open. See
[process integration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-t002-process-integration-20260905.md).

**Spec181 T002 P-256 production path (2026-09-05): focused defects CLOSED.**
R1 proves the runner overwrites an explicitly configured recipient-key map
with the Ed25519 offer-key map. Four other checks stop in fixture key
generation because this cryptography installation requires an explicit backend;
they do not prove a production refusal. R2 fixes the fixture: three actual
entry regressions fail (requester loader, Python Provider loader, map override),
and two wrong-curve rejection checks pass. R3 repairs those entries: 130 checks
pass, but the positive P-256 case reaches the production envelope creator and
fails because its EC key generation also omits the required backend argument.
All three raw results are retained. R4 repairs the production key generation;
134 focused checks pass, including bounded private-file rejection. The unified
native rebuild and a fresh four-recipient P-256 Y-B control pass: four native
grant verifications, terminal numerical match, seven collected child exits,
and empty staging. Full T001/T002 acceptance remains open.
See [P-256 production path](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-p256-production-20260905.md).

**Spec181 T002 recipient credentials (2026-09-05): focused defect CLOSED.**
The production factory accepts only Ed25519 private keys although T002 and
the native verifier also support EC P-256 envelopes. The rebuilt credential
regression runs eight cases; only P-256 loading fails before grant acquisition
with `provider recipient private key is not Ed25519`. The r1 build and RED
logs are retained. R2 loads validated P-256 PEM through the production loader;
all eight focused checks pass, including wrong-curve and permission rejection.
P-256 network acceptance and full T002 closure remain open. See
[recipient credentials](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-recipient-credentials-20260905.md).

**Spec181 T002 helper lifecycle (2026-09-05): focused lifecycle defects CLOSED.**
The native assembler waits synchronously for its helper, ignores request and
operation deadlines during assembly, observes grant cancellation/expiry only
after slow work, and can activate helper output beyond the role envelope.
Six real-process regression failures are retained in
`spec181-t002-helper-20260905-r1/red.log` in the ignored workspace temporary
directory. R2 builds but 14 native checks stop at the loader: the old installed
framework lacks `streamCancelled`. R3 binds the test executable and its
environment to the current build library: 26 focused checks pass. A new
source-fetch cancellation regression then proves that the parent recreates
the erased plaintext directory. R4 serializes protected staging writes
with runtime cleanup; 27 focused checks pass, including the new race. The
new RED log is retained in r3. The final unified native rebuild and a fresh
protected Y-B control pass: four actual grant verifications, three ORT CPU
roles plus native Merge, verified terminal output, and empty staging after
all seven child exits are collected. Full T002 acceptance remains in progress. See
[T002 helper lifecycle](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-helper-lifecycle-20260905.md).

**Spec181 T003 assembly parity (2026-09-05): focused build defects CLOSED.**
The fixed-vector Python lane passes 8 cases; the native lane fails 8 checks
because its required current-source test executable is not built yet. This
is a test-input boundary, not an assembly or protocol rejection. R2 compile
also fails before execution because the manual command omitted the installed
NAC-ABE package's `NAC_ABE_CMAKE_BUILD` definition. R3 also lacks the maintained
framework include path; r4 replaces the manual command with a focused Waf
target using the existing dependency configuration. R4 compiles but exposes
the framework's NDNSD link dependency; r5 adds that configured dependency.
R5 build passes; the final 19 parity checks pass, including real ORT CPU
execution and unchanged negative-cache state. Fixed-vector regeneration is
byte-identical. T003 is complete at its focused scope; T007 remains BLOCK. Preserve
`spec181-t003-assembly-20260905-r1` and build the production-entry fixture
before another attempt. See
[T003 assembly parity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t003-assembly-parity-20260905.md).

**Spec181 T005 evidence preservation (2026-09-05): focused defects CLOSED.**
Five focused checks expose the legacy driver's destructive attempt handling,
runtime-dependent entry, and continuation after matrix failures. No network
processes are started. The repair retires this unsafe automatic retry path
and makes the maintained matrix stop at its first failed subcase. Raw RED
output is retained under `spec181-t005-evidence-repair-20260905-r1` in the
ignored workspace temporary directory. R2 passes 118 focused checks after
retiring the legacy entry and stopping on the first matrix failure. This is
not formal matrix qualification; T005 remains NOT PROVEN. See
[T005 evidence repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-evidence-repair-20260905.md).

**Spec181 T006 positive control (2026-09-05): cold dependency timeout CLOSED.**
All four native grants verify, but r8 Merge's first tensor-manifest fetch
expires at the fixed 10 s no-progress bound while its producer finishes cold
model preparation. User then times out. The three actual grant negatives
pass. After binding the data wait to the configured request budget while
retaining the existing hard deadline and cancellation, r10 completes the
protected native control; a measured dependency wait is 13.97 s. The final
r11/r12/r13 negatives also pass with complete process collection. T006 is
complete at its focused scope; T007 remains BLOCK. See
[T006 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t006-production-repair-20260905.md).

**Spec181 T006 production rejection (2026-09-05): false-positive oracle CLOSED.**
Eight focused regressions fail: the configured requester seam publishes no
mutation, invalid mutation/epoch settings are admitted, and User-local
exceptions or markers can masquerade as Provider rejection. No selected
Provider network rejection is established by those old probes. See
[T006 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t006-production-repair-20260905.md).
After repair, 62 focused checks pass. The separate runner check retains one
obsolete expectation that a User-local Y-N-E probe should report PASS; that
test is corrected; the updated Python group has 150 PASS. A separate C++
harness link omitted the store source; the next command used a nonexistent
shortened filename. The verified source is NativeProtectedArtifactStore.cpp;
use a new directory for the corrected harness command. The unified native
build has independently completed successfully.
r4 C++ harness passes 22 cases. The r5 live entry stops before network
creation because Y-B inputs omit Y-N's FullModel role. Use the verified
five-role Y-N inputs with an explicit protected epoch for subsequent variants.

**Spec181 T004 lifecycle acceptance (2026-09-05): focused defects CLOSED.**
R1 failed before the waiter because the fixture had no running Controller
serving AA public parameters. R2 corrects that startup order and reaches the
real Provider waiter: an 80 ms wait returns false in about 6 us before run,
with SPEC181_PROVIDER_READINESS_PREMATURE_TERMINAL. The initial non-running
state was incorrectly terminal. R3 retains an OUTPUT_ROOT_MISSING preflight
failure. After the native fix/rebuild, r6 waits 83 ms and starts/stops the real
Provider; r4 reaches Controller readiness at 12.39 s; r5 cancels an active
Core probe in 2.3 ms without hot spinning. All three probes exit 0 after
process collection and network cleanup; six Core checks also pass. T004 is
complete at its focused acceptance scope; T007 remains BLOCK. See
[T004 lifecycle acceptance](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t004-lifecycle-acceptance-20260905.md).

**Spec181 T002/T004 native repair (2026-09-05): focused defects CLOSED; tasks OPEN.**
Retained failures identify group digest format, grant forwarding hint, model
basename, premature readiness, and debugger exit-code boundaries. Live-r7
uses ordinary process commands and the same rebuilt source: native protected
Y-B returns PASS/exit 0, with three ciphertext files and no ONNX plaintext or
staging remnants after cleanup. Production negatives, cancellation/resource
acceptance, source checkpoint closure and T007 remain open. See
[native launch repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-live-repair-20260905.md).

**Spec181 T002 production wiring (2026-09-05): build defect CLOSED; T002 OPEN.**
The first compile failed on the installed ndn-cxx forwarding-hint API. A
separate retained r2 build passes native/library/extension identity checks,
and 3 rebuilt-extension tests consume the 9 grant vectors. Real Provider
network and ORT lifecycle acceptance remains open. See
[T002 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-production-repair-20260905.md).

**Spec181 T002 native runtime repair (2026-09-05): focused defects CLOSED.**
18 native focused tests pass with real verification, managed content keys,
deadline/cancellation checks and retryable cleanup. Initial compile, linker
and consumption/deadline failures remain preserved. Factory, storage AEAD
and real network acceptance still keep T002 open.
See [T002 runtime repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-runtime-repair-20260905.md).

**Spec181 T001 registry repair (2026-09-05): focused defects CLOSED.**
100 focused tests plus 7 inherited grant tests pass for pinned registry
policy, private-key matching, distinct issuer/publication identities and
the final published-root allowlist. Initial RED and Python 3.8 compatibility
failures are retained. T001 network and lifecycle acceptance remains open.
See [T001 registry repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-registry-repair-20260905.md).

**Spec181 T001 Provider repair (2026-09-05): focused defects CLOSED.**
70 focused tests pass for authorization before preparation, in-memory keys,
on-disk AEAD loading, model/weights cleanup and registered-handler failures.
Registry-policy wiring and real network integration still keep T001 open.
See [T001 Provider repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-provider-repair-20260905.md).

**Spec181 T001 lifecycle repair (2026-09-05): focused defect CLOSED.** Five
RED failures are repaired; 19 focused tests pass for in-memory key leases,
duplicate protection, complete cleanup and private/symlink-safe files.
Provider integration remains open; this does not close T001.
See [T001 lifecycle repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-lifecycle-repair-20260905.md).

**Active Spec181 audit (2026-09-05): BLOCK.** Native protected runtime wiring,
production-path negative validation and assembly parity remain unproven;
the active Context Mode plan link has been repaired. Latest retained Spec181 Y-B log
reports `CASE_RUNTIME_PROCESS_START_FAILED:control`, not a protocol result.
See [Spec181 audit repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/audit-repair-20260905.md).
Focused repair checks are allowed; full qualification requires a fresh audit PASS.

**Current controlling repair (2026-09-05):** Y-N negative verdicts accepted
unrelated exceptions as PASS. The first focused regression reproduced 12
failures; the repaired User/runner/application/build-guard set passes 183
focused tests. The unified build also exposed a stale legacy RUNPATH in
`build-system-j2`, despite its name. See
[negative verdict and build repair](../specs/180-ack-driven-cross-model-qualification/evidence/t011-negative-verdict-repair-20260905.md).
FR-008 still lacks the production authenticated, recipient-encrypted grant
path and operator-authorized issuer configuration; see
[protected-grant gap](../specs/180-ack-driven-cross-model-qualification/evidence/t008-protected-grant-gap-20260905.md).
Older PASS labels below must not be reused as safety evidence. The r42 startup
observation remains useful but does not close this semantic verdict defect or
the missing FR-008 protected execution path.

| ID | Observed | Scope | First failing boundary | Disposition | Durable record | Raw run data |
| --- | --- | --- | --- | --- | --- | --- |
| `SPEC180-Y-N-R42-EXTENSION-CWD` | 2026-09-05 | Spec180 current-source extension rebuild | `pythonWrapper/setup.py` was invoked from the repository root, so its relative C++ source path could not be found | `CLOSED as command-invocation error`; compiler exited 1 before producing an artifact | [`t011-y-n-live-current-20260905-r42-extension-build-cwd.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-extension-build-cwd.md) | no raw run; command output is preserved in the task log |
| `SPEC180-Y-N-R41-I-EXTENSION` | 2026-09-05 | Spec180 focused Y-N-I current-source check | Loaded Python extension/framework artifact identity before interpreting the live protocol result | `CLOSED by r42`; rebuilt artifacts contain the current source and r42 logged the marker sequence | [`t011-y-n-live-current-20260905-r41-extension-check.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r41-extension-check.md), closure [`t011-y-n-live-current-20260905-r42-i-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-i-only.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r41-i-only/` |
| `SPEC180-Y-N-R40-PATH` | 2026-09-05 | Spec180 diagnostic I-only | Temporary run path before the maintained helper completed | `CLOSED as command-path error`; the command used a mistyped directory and was interrupted with exit 130 | [`t011-y-n-live-current-20260905-r40-path-preflight.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r40-path-preflight.md) | preserved partial data under ignored workspace temporary `spec180-diagnostic-path-error-r40/` |
| `SPEC180-Y-N-R39-I` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before Y-N-I reached provider execution | `CLOSED by r42 for the focused I-only boundary`; r42 reached readiness and provider execution with the rebuilt current artifacts; the full matrix remains open | [`t011-y-n-live-current-20260905-r39.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r39.md), closure [`t011-y-n-live-current-20260905-r42-i-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-i-only.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r39/` |
| `SPEC180-Y-N-R37-P-PREFLIGHT` | 2026-09-05 | Spec180 diagnostic P-only | MiniNDN root/user-namespace preflight before NFD creation | `CLOSED by r38`; diagnostic command omitted `unshare -Urnm` and exited 1 | [`t011-y-n-live-current-20260905-r37-p-only-preflight.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r37-p-only-preflight.md), closure [`t011-y-n-live-current-20260905-r38-p-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r38-p-only.md) | ignored workspace temporary P-only attempt `spec180-yolo-y-n-current-20260905-r37-p-only/` |
| `SPEC180-Y-N-R36-P` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before Y-N-P reached the ACK-closed boundary | `UNQUALIFIED`; Y-N-O/C/R/I/E/L passed, Y-N-P was not proven | [`t011-y-n-live-current-20260905-r36.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r36.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r36/` and console log with the same run-id |
| `SPEC180-Y-N-R35-P-E` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before either negative case reached the ACK disposition path | `UNQUALIFIED`; Y-N-O/C/R/I/L passed, Y-N-P/E were not proven | [`t011-y-n-live-current-20260905-r35.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r35.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r35/` and console log with the same run-id |

Historically, `r42` was recorded as closing its artifact-identity and focused
readiness occurrence. Its rebuilt framework library and Python extension were checked,
and the I log shows the Controller
issuing `registering prefix: /example/controller` and the six NDNSF filters,
followed by successful root and `PUBPARAMS` registration, the readiness probe,
and the expected Y-N-I execution markers. This is focused T011 evidence only;
the full Y-N matrix and T014 remain open. This does not qualify the subsequently
changed readiness implementation or negative oracle, nor prove the complete
dependency closure now checked by the unified build. The earlier r39 I
occurrence was closed only at that historical boundary; its bytes are preserved.

The earlier `r36` result likewise showed the Controller
issuing `registering prefix: /example/controller` and the six NDNSF filters,
but the first Face connection closes before the root registration completes;
the reconnect installs only the six NDNSF routes. There is no successful
Controller-prefix registration and no `PUBPARAMS` filter before the Python
readiness timeout. This is classified as a startup/transport boundary failure,
not as an ACK disposition failure. r35 remains relevant as the prior broader
P/E occurrence.

The ordinary `ConfigManager` message about a missing `/etc/ndn/ndnsf.conf` in
the child logs is ambient diagnostic noise for this run; it is not the
controlling failure because the successful subcases contain it as well.

## Historical pointers

These records remain useful when the current failure is related to their
boundary, but they do not advance the active gate by themselves:

- [`t013-controller-pubparams-readiness-current-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-controller-pubparams-readiness-current-20260904.md): why the real AA `PUBPARAMS` readiness barrier exists and why the old exact-SIF result was invalidated.
- [`audit-revision123-design-code-conformance-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/audit-revision123-design-code-conformance-20260904.md): revision-123 design/code findings and their evidence boundary.
- [`t014-tiger-path-audit-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t014-tiger-path-audit-20260904.md): Tiger-path audit block and the reasons implementation checks were not qualification evidence.
- [`t013-supervision-repair-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-supervision-repair-20260904.md): supervision and cleanup caveats retained after the repair.

## Failure-record contract

Every failed, blocked, or `UNQUALIFIED` command that can affect task order
must produce or update a durable record before the next retry. The record must
contain:

1. a unique run/failure ID and UTC/local date;
2. the exact source/candidate/config identity and command;
3. the first failing boundary, exact marker or error, and child exit status;
4. the raw-log path plus a compact, secret-free excerpt or structured summary;
5. the affected Spec task/gate and any candidate/evidence invalidation;
6. the next allowed action and the condition that closes the failure.

Keep large logs, private keys, credentials, and transient sockets out of Git.
Use a new ignored workspace temporary directory named with a unique run-id for
each attempt; never overwrite a prior run. The durable evidence record must be
sufficient to understand the failure when the transient directory is later
unavailable.

## Closing an entry

Change the disposition only after a fresh run reaches the same boundary and
produces the required closing marker. A focused unit test can close an
implementation defect, but it cannot close a live, SIF, or Tiger gate unless
the active Spec explicitly defines that test as the gate's evidence.
# NDNSF Failure Log

## 2026-09-06 — UAV API compatibility audit: static authorization expires without renewal

- **Symptom**: an application with unchanged static permissions can compile with the new APIs but lose authorization at the Controller status's 24-hour boundary. Re-fetching the status does not extend its fixed validity window. Source probes also reproduce seven old-call compilation failures: deleted public large-response helper, three positional aggregates, two Hybrid member pointers and one NAC ParamFetcher member pointer.
- **Cause**: Controller initialization/epoch advance sets `m_policyValidUntilMs`; ordinary `getPolicyStatus`/Interest handling does not renew it, while new runtime enforcement rejects expired status. Separately, the helper removal, inserted fields and overload additions break specific source forms. Asynchronous DKEY bootstrap and ACK payload redaction introduce further behavioral migration requirements.
- **Disposition**: audit only, production fixes remain open. Do not present prior short MiniNDN scenarios as static long-running compatibility acceptance or restore insecure fallback behavior. Prioritize safe immutable-status renewal, production readiness semantics, then source compatibility/migration.
- **Evidence**: `specs/179-request-scoped-confidentiality/evidence/uav-api-compatibility-audit-20260906.md`; `results/api-compatibility-audit-20260906/summary.json` has 40 compilations (17 historical and 6 official controls pass; current 10 pass/7 compatibility failures). Six unmodified historical Apps/examples compile. A separately linked production expiry-boundary probe reports lifetime86400000, allowed-before1, allowed-at-expiry0, `controller_status_expired`, expired-refresh-accepted0. Its exit0 means defect reproduced, not fixed; no actual 24-hour network soak was executed.
- **Lesson**: preserve separate source, ABI, protocol, startup and long-running behavior gates. Applications that do not call new authorization APIs still enter the new runtime defaults.

## 2026-09-06 — API audit probe setup produced false compatibility failures

- **Symptom**: initial typed helper probe failed on both revisions (`std::string` lacks `ParseFromArray/SerializeToString`); NAC probes failed with unknown ParamFetcher/PublicParams. Trying a different include order alone did not solve the NAC failure.
- **Cause**: the synthetic payload did not satisfy the pre-existing template contract. Probe compiler paths allowed NAC algo headers' bare `common.hpp` to resolve to NDNSF's same-named header. These were invalid audit controls, not new library defects.
- **Fix**: use a minimal protobuf-shaped compile-only payload; put the NAC source/package directory ahead of NDNSF paths and use NAC's CMake header mode. Preserve initial logs under `results/api-compatibility-audit-20260906/setup-attempts/`, then rerun the entire matrix with at most two compiler processes. Final historical controls all pass, and the seven current failures are attributable to actual API changes.
- **Lesson**: never call a compiler red an API regression until the identical source compiles on the declared baseline with correctly isolated headers.

## 2026-09-05 — T022 official-merge segmentation regression setup

- **Symptom**: first new Producer segmentation test build fails because the default-template factories are protected. Two guessed inspection paths did not exist; the actual campaign is `scripts/spec179_minindn_campaign.sh` and Segmenter belongs to installed ndn-cxx. Initial T022 placement also made the structure scanner warn about task order.
- **Fix**: use explicit Data templates in tests without changing production access; locate exact files and move T022 after T021. Preserve the failed build log under `results/spec179-official-merge-20260905/nac-red-build.log`; retry at unchanged-j2.
- **Lesson**: test public behavior through explicit inputs; do not expose production helpers for convenience. Use file discovery instead of guessing script paths. This compiler failure is a test setup defect, not the expected pre-merge behavioral red.
- **Behavioral red**: fresh85547eb build plus four new cases gives12/15 pass,3 failed cases/5 failed assertions, exit201. CP/KP ignore128-byte limits (1500-byte data, single CK segment); normalizeCkKey collapses distinct segment-named objects and the second warm-cache decrypt returns wrong plaintext. Existing object/exact-segment retrieval and invalidation/reentry tests pass. Official58f3948 merge is conflict-free; green results pending.
- **Merged green**: Experimentalc3aafa6 retains85547eb and official58f3948 ancestry. The same15 cases all pass, including both segment-limit tests and distinct-CK plaintext. Separate prefix installed with f5cb1ec8 library hash; full NAC/native/network qualification continues.
- **Dependency gates**: full46/46,4284 assertions; installed-prefix26/26,1082 assertions. An initial launcher unittest command treated tests/minindn as an importable package and failed2 imports; rerun with PYTHONPATH=tests/minindn and explicit module names. Preserve the import-error log separately from actual launcher results.
- **Launcher runner correction**: that import-path retry executes0 unittest cases, so it is rejected as vacuous. These are pytest function tests; inspect their declared runner and execute python3 -m pytest on both exact files. Neither the import failure nor the zero-test exit0 is a launcher green gate.
- **T022 closure**: merged15/15, full NAC46/46 (4284 assertions), installed26/26 (1082), clean native183 unit/74 integration (11998/1297), pytest28/28; full18 MiniNDN scenarios,188 assertions and both dedicated User grant gates at clean cdd8e55a.33 hashes match disk, driver/all CLIs exit0. Build28m9.720s at-j2; no runtime source changes beyond the two official NAC files. Preserve the compiler/import/zero-case and behavioral reds. T014 publication is deferred, not failed or satisfied by the local merge.

## 2026-09-05 — NAC upstream provenance inferred from a remote alias

- **Symptom**: the first T014 delivery update called suraviregmi/NAC-ABE the original project and inferred12 missing prerequisites from its stale master.
- **Cause**: verified remote SHAs but did not first verify GitHub fork parent/source metadata. The alias `suravi` did not establish official ownership.
- **Correction**: GitHub API confirms both remotes fork UCLA-IRL/NAC-ABE. Fetching official master58f3948 into an isolated clone shows fork master2 ahead/3 behind and Experimental6 ahead/3 behind. The missing changes are maxSegmentSize propagation and removal of segment stripping, plus their merge. Corrected all current delivery/plan/task/audit claims; the preceding entry is historical and its original-project12-commit statement is superseded here.
- **Evidence**: both isolated no-commit merge previews pass without conflicts, changing only cache-producer.cpp and consumer.cpp; previews aborted afterward. Candidate trees and exact source diffs are recorded in `specs/179-request-scoped-confidentiality/evidence/nac-abe-official-comparison-20260905.md`. No candidate runtime qualification, actual NAC branch update, push or PR occurred. User explicitly requests no PR.
- **Lesson**: establish official repository identity from parent/source metadata, then fetch live refs and compare both ancestry directions. A clean merge proves textual compatibility only. A multi-file documentation patch with one mismatched context was rejected atomically; split it into verified exact-context updates without altering runtime files.

## 2026-09-05 — T014 upstream package described an incomplete, ambiguous delivery

- **Symptom**: the earlier package omitted NAC T019/T020, called personal fork master upstream, mixed ParamFetcher API changes into a no-API-change PR and treated the tested OpenABE worker as independently optional.
- **Cause**: delivery prose was not refreshed after dependency compatibility repairs or verified against actual remote/base ancestry.
- **Fix**: retain the old draft as explicitly superseded; pin all four commits through85547eb, map every public surface, document ABI/lifetime limits, provide a concrete fork PR draft, and distinguish the12 prerequisite commits between original master and the tested base. A standalone bundle passes verification, fresh clone, exact head/tree comparison and fsck. T014 stays open for publication authorization, upstream acceptance and rebuilt gates.
- **Tool deviations**: a query guard rejected low-entropy `T014`; corrected to the exact feature basename before authoritative reuse. Broad CodeGraph exploration returned unrelated version symbols; after the required attempt, exact source-string verification established callers. apply_patch rejected delete/add operations on one path atomically; a separate current report and historical pointer avoid destructive replacement. No runtime files changed.
- **Lesson**: an upstream package must identify the destination, prerequisites and complete tested revision; a local bundle or fork push does not prove upstream acceptance. Use a high-entropy feature identifier and update each path once per patch.

## 2026-09-05 — Provider online authorization was absent from MiniNDN grant coverage

- **Area**: Spec179 T021, Controller service-offering permissions.
- **Finding**: existing network grants hardcode User/B and `/PERMISSION`; Provider component policy assignment and network revocation do not establish first-grant service execution. Controller example's grant timer cannot select `/SERVICE`, and Provider example lacks the User example's explicit post-startup permission renewal.
- **Repair**: explicit User/Provider grant-role option (default User), Provider App-owned renewal timer, separate Provider normal/late first-grant scenarios with targeted traffic and unaffected control. Pure evaluators retain target/control failures and reject early service, wrong role/provider, absent renewal and missing late timeout ordering.
- **Evidence**: new evaluator before implementation fails11/11 because no evaluator exists (`provider-grant-evaluator-red.log`); after implementation11/11 pass. Combined launcher gate initially25/26 passes; sole failure is the old exact User-only guard-error string. Parameterize the host-namespace guard test for both roles; final27/27 pass (`provider-grant-launcher-final.log`). Native rebuild and network results pending under `results/spec179-nac-compatibility-20260905/`.
- **Lesson**: Controller policy mutation, runtime permission installation, key readiness and actual service execution require distinct evidence on each role. A User grant cannot qualify Provider service-offering authorization.
- **First network red**: normal and late Provider runs both complete but exit4/gatefalse (`provider-campaign-first/`, driver exit1); User compatibility control passes. Benchmark paths ignore `--known-provider-ids`, so Provider/A serves requests intended for B, including pre-grant successes. User compares provider/service records when deciding DKEY refresh, so an added Provider route incorrectly refreshes an unchanged User service attribute. Normal control31/32 and target31/32 also retain a transition timeout. Provider status-advance permission revalidation legitimately precedes the manual timer in the normal case, contradicting the initial test assumption.
- **Follow-up repair**: benchmark calls use the existing explicit-provider overloads, preserving built-in/custom selection; User DKEY change detection compares service sets, with an added route-only Controller integration regression. Evaluate actual post-grant Provider permission-fetch events and the later idempotent App timer separately. Use the existing grant probe's250ms status cadence; retain all terminal failures and document that version changes can cancel in-flight work, rather than claiming uninterrupted traffic at arbitrary timing. Rebuild/native/network verification pending.
- **Repaired native checkpoint**: build exit0 in4m27.010s at-j2; six dependency closures pass; launcher28/28, unit182/182 (11971 assertions), integration72/72 (1281 assertions) pass. Final18-scenario network acceptance pending.
- **Second network red**: at925ec3a9 both Provider variants pass every check except the exact selected Provider:17/17 and22/22 post-renewal successes still mix Provider/A and B. The App now passes B correctly; `handleRequestAckByName` checks Controller permission but omits the request's explicit Provider set. Stop the campaign driver after retaining both failures (driver143); the in-flight User control also completes successfully. This partial cohort is not a final18-case gate.
- **ACK fix**: reject decoded ACKs outside a nonempty pending-call Provider set before status hints, ACK metrics or selection; keep empty-list discovery behavior. Complete any tracked decrypt accounting on rejection. A new native case verifies FirstResponding, RandomSelection and AllSelected refuse another Controller-authorized Provider and select the requested one. Rebuild and full final native/network gates pending.
- **ACK native checkpoint**: build exit0 in8m2.858s; six closures pass; unit183/183 (11998 assertions), integration72/72 (1281 assertions) pass. Final18-case network cohort still required.
- **T021 closure**: final `campaign-ack-final/` at clean994018ac passes18/18 scenarios,188/188 scenario assertions and both dedicated User grant gates. Driver and all CLI exits are0; all33 artifact hashes match disk. Provider target/control successes are17/17 +32/32 and22/22 +65/65; User grants10/10 +24/24 and21/21 +60/60. Planned restart/outage role exits-2 are explicitly covered; other role exits0. `final-network-verification.log` and `evidence/provider-online-grant-20260905.md` are authoritative. Earlier failures are not relabeled.

## 2026-09-05 — NAC-ABE compatibility review exposes dependency boundary defects

- **Area**: Spec179 T020, NAC-ABE Experimental compatibility and callback ownership.
- **Symptoms**: two new real CK fan-out tests abort with memory-access violations when success/error callbacks call `clearCache`; late parameter replies replace new state (two failed assertions); current Authority bytes are returned under an unavailable old version (two failed assertions); wrong/empty CP/KP keys decrypt after another key warmed the singleton cache (four failed assertions).
- **Root causes**: application callbacks invalidate the live waiter-map iterator; ParamFetcher fences neither fetch/validation nor retry generations; Authority reflects a requested exact name instead of its actual generation; the inherited crypto cache is indexed by ciphertext alone. The latter predates both reviewed commits but prior tests manually cleared it before negative-key checks.
- **Repair**: detach CK batches before callbacks and check generation between waiters; fence parameter delivery, validation and retries, and commit decoded/name/digest-checked candidates atomically; construct canonical Authority names; bind the crypto cache to a hashed length-delimited scheme/parameters/private-key/ciphertext tuple. Restore the original no-argument ParamFetcher entry and document class-layout rebuild requirements and silent cancellation semantics. Verification is in progress.
- **Evidence**: `results/spec179-nac-compatibility-20260905/red-{reentry-success,reentry-error,params-authority,cache}.log`; durable report `specs/179-request-scoped-confidentiality/evidence/nac-abe-compatibility-review-20260905.md`.
- **Test/tool findings**: first full NAC run passed41/42; the sole failure was an existing lifecycle probe requiring an unset role variable. Default it to the User role while retaining explicit role validation; CTest also needs the fixture directory and a supported report option. An initial class-layout probe omitted ndn-cxx at link time; relink with its pkg-config libraries. A provisional focused run started before final test linking and used the previous test executable, so it is not the final gate.
- **Inspection failure**: `objcopy --dump-section` without an explicit output object rewrote both input ELF files during an installed-prefix test. Stop that test (exit143, `nac-installed-interrupted.log`), relink from unchanged objects and reinstall to restore the original hashes, then repeat installed-prefix execution. Use read-only ELF readers or disposable input copies for future section comparisons; never inspect live libraries with an in-place tool.
- **Dependent build failure**: the mandatory layout rebuild hit GCC9 `internal compiler error: in ggc_set_mark, at ggc-page.c:1547` in system `basic_string.h`, while compiling `HybridMessageCrypto.cpp` for integration-tests (`ndnsf-build.log`, exit1). Retain completed objects and retry once at the same `-j2`; repeated compiler failure requires the already established Clang10/system-binutils fallback. This is not an executed runtime test failure.
- **ABI rebuild finding**: the retry linked successfully in6m35.140s, but object timestamps showed Controller flow and generic API tests still dated15:24–15:27, before the17:22 parameter-header layout change. A successful incremental link is insufficient: invalidate stale objects for the selected framework/tests/three Apps, retain the exact path list, and rebuild again at `-j2`. Unrelated build targets are excluded. No tests from the stale-object link count as acceptance.
- **Compiler fallback**: after invalidating90 selected stale objects, GCC again crashed in `basic_string.h` and produced a non-constant assembler `.size` expression (`ndnsf-build-fresh-objects.log`, exit1). Stop GCC retries. Configure a new `build-clang-spec179-nac-compat` with explicit Clang10, system binutils, the exact NAC prefix and `-j2`; no cached GCC objects enter that build.
- **Strict compiler finding**: Clang rejects the unused `this` capture in the User status-restore validation-error callback (`clang-build.log`). The mirrored Provider callback has the same unused capture. Remove only those captures; retain `-Werror` and the callback body. This is the only NDNSF runtime source change in the dependency review.
- **Lesson**: tested cancellation must include callback reentry and validator latency; cache tests must retain a warm cache for unauthorized callers. Source-compatible calls do not establish binary-layout compatibility.
- **Native checkpoint**: NAC42/42 full cases,20/20 installed-prefix cases; clean Clang NDNSF build exit0 in21m10.471s, unit182/182 (11971 assertions), integration72/72 (1278 assertions), all six target dependency closures pass. Complete16-scenario MiniNDN rerun pending.
- **T020 closure**: fresh16/16 MiniNDN runs completed with CLI exit0,161/161 scenario assertions and both dedicated User grant gates. All33 artifact hashes match disk and every manifest binds clean de1eb508. Planned Provider restart/Controller outage exits-2 are checked by their scenarios. See `campaign-verification.log` under the evidence root. Provider online first grant remains a separate T021 coverage gap.

## 2026-09-05 — NAC dependency test build retained a removed Boost prefix
- **Area**: T019 dependency regression build
- **Symptom**: after correcting a missing test error-header include and parenthesizing a Boost assertion message, test compilation succeeded but linking required absent `/usr/local/lib/libboost_unit_test_framework.so.1.82.0` (`gates/nac-revocation-red-build2.log`).
- **Root cause**: enabling tests reused stale Boost CMake cache entries in the existing exact-prefix build directory.
- **Fix**: unset only `Boost_*`/`boost_*` cache entries, configure `BOOST_ROOT=/usr` and `Boost_NO_BOOST_CMAKE=ON`; verify all resolved Boost libraries point to system1.71 before rebuilding with `-j2`. Expanded dependency tests subsequently passed14 cases/90 assertions.
- **Lesson**: enabling a previously disabled target can reveal stale optional dependency paths even while the shared-library target builds successfully.

## 2026-09-05 — late NAC content callback survives cache invalidation
- **Area**: Spec179 T019, local NAC-ABE Consumer and OpenABE error boundary
- **Symptom**: WAL campaign retry scenario passes all14 business checks, but Provider/A aborts(-6) with `Specified length is invalid` and uncaught `oabe::_OpenABE_ERROR` immediately after epoch3 installation. The process-exit gate correctly rejects it. Driver stopped(exit143); six completed probes retained, five passed. This is incomplete failed evidence.
- **Mechanism reproduced**: Consumer increments `m_cacheGeneration` only in `clearCache`; neither asynchronous content nor CK completion/error checked it. `nac-consumer-revocation-red2.log` fails5/23 assertions: late content reaches crypto with cleared DKEY and produces the same OpenABE error; late CK refills the cache. CP/KP enum conversion fails2/4 separately. GDB on the preserved old binary/library catches the enum in `constructKeyFromBytes -> parseKeyHeader -> importUserKey -> ABESupport::decrypt`, with the caller blocked in `Consumer::onCkeyData -> decryptContent -> SegmentFetcher` (`gates/nac-late-content-gdb-red.log`). This is the controlled reproduction's stack; the original network process has no stack dump.
- **Fix**: generation-fence both fetch stages' completion/error callbacks and normalize OpenABE enum errors into `NacAlgoError`. Unchanged Consumer tests pass23/23 and CP/KP error/recovery passes4/4; expanded dependency14 cases/90 assertions pass. Committed as NAC-ABE `8b462d0`, exact prefix installed and NDNSF resolution verified. Final NDNSF unit182/182, integration72/72 and full MiniNDN16/16 pass; the retry scenario retains14/14 business checks and all five processes exit0. No wire format, permission ownership, or revocation timing changes.
- **Closure evidence**: `specs/179-request-scoped-confidentiality/evidence/online-authorization-audit-20260905.md` and `results/spec179-online-auth-20260905/campaign-nac-final/`. This final cohort also closes the earlier pending online-grant, initial-DKEY admission, callback, namespace fault and PIB regression entries below; their intermediate failures remain historical evidence.
- **Lesson**: clearing maps and rejecting new requests does not cancel callbacks already admitted under old authority; every delayed stage must retain and verify its originating generation.
## 2026-09-05 — shared MiniNDN PIB reader races another role's startup write
- **Area**: Spec179 online-grant fixture, shared campaign PIB
- **Symptom**: the final-readiness late-grant probe had target21/21 but control0/0: User/A exited1 with `Signing certificate ... does not exist` before its first request. The gate correctly failed. Its certificate/key remained present in the retained PIB. Ordinary grant and eight other completed probes are retained; the driver was stopped (exit143) after this failure, allowing the active ninth probe to clean up normally. This is an incomplete failed campaign, not16-scenario acceptance.
- **Root cause evidence**: the fixture used DELETE journals; installed ndn-cxx PIB SELECT paths treat non-ROW, including BUSY, as absent and configure no busy timeout. The added concurrent-reader regression reproduces `database is locked` under the old setup (`gates/pib-concurrency-red.log`). Lock contention is the supported explanation for the transient App failure; that process did not log SQLite's nested return code.
- **Fix**: initialize the campaign-only PIB in WAL mode before any role starts, require the returned mode to be WAL, and record it in the manifest. Existing writer initialization serialization remains. No host PIB, native library, permission rule or deadline changes. Full harness and a fresh network campaign verify the repair.
- **Lesson**: isolated network namespaces do not isolate an intentionally shared SQLite key store; preserve concurrent signing reads as well as serializing initialization writers.

## 2026-09-05 — base RequestMessage overload bypasses shared admission helper
- **Area**: Spec179 initial-DKEY repair verification
- **Symptom**: first repaired build passed the unwrap callback checks but still failed the two publication checks (6/8, `gates/readiness-green.log`).
- **Root cause**: five convenience/Targeted callers used `prepareRequestControllerVersion`, but the base RequestMessage overload entered `startRequestServiceWithRequestId` directly, duplicating only status/version checks. Checking the helper's callers alone missed that separate entry.
- **Fix**: replace the duplicated base-entry checks with the same shared readiness/status helper. The unchanged regression now passes8/8 (`gates/readiness-base-green.log`); all five native targets rebuilt successfully with `-j2` in12m58.644s (`gates/build-base-admission-green.log`). Expanded unit182/182 and integration72/72 passed (`gates/unit-base-final.log`, `gates/integration-base-final.log`); MiniNDN remains pending.
- **Lesson**: trace from the public failing call to the publication boundary; a helper's caller list does not prove every entry uses it.

## 2026-09-05 — permission renewal admits requests before initial DKEY installs
- **Area**: Spec179 asynchronous User startup and hybrid decrypt callbacks
- **Symptom**: normal grant still failed with target11/13 despite status refresh. NAC Consumer debug shows permission renewal at12s coalescing behind the initial DKEY fetch; the stale result is discarded near15s before a replacement installs. ACKs for the first two requests arrive while the Consumer reports no private decryption key, with no application error callback.
- **Root cause**: removing constructor blocking exposed an implicit prerequisite: Request admission checks permission/status but not initial Consumer readiness. Both runtime hybrid decrypt functions move `onError` into the success closure before constructing the unwrap error callback, leaving the latter empty.
- **Fix**: gate real network Request admission on initial Consumer readiness while retaining LocalMock fixture semantics; copy error callbacks into the asynchronous success/unwrap branches and retain synchronous exception reporting. The real-constructor regression reproduced four failed assertions (publication1 instead of0, errors0 instead of1); `gates/readiness-red.log`, exit201. The pre-fix binary is retained. Fixed rebuild/regression is pending.
- **Evidence**: `grant-crypto-diagnostic/user-B.log`; earlier `campaign-grant-final` also retains one control timeout at the grant/status transition. Exact-version rejection is expected during distributed convergence; do not claim instantaneous default-policy convergence or hide that failed row.
- **Lesson**: asynchronous construction must replace former implicit prerequisites with explicit admission checks; moving a shared callback into one branch can silently disable another branch.
- **Probe setting**: normal grant now uses the existing 250ms status refresh knob (four opportunities per 1rps request interval); late grant retains1s. Every failed row remains counted. These measured settings do not establish zero interruption with default status-refresh timing.

## 2026-09-05 — grant control failures were not included in the gate
- **Area**: Spec179 grant-only control and status-convergence evidence
- **Symptom**: normal renewal probe failed with target12/13 and control12/24 successes. The collector exposed control successes only, so the twelve control failures were absent from gate conditions.
- **Root cause**: successful-only control projection lacked a companion failure count. Separately, this grant probe disabled scheduled status refresh: providers learned epoch2 from target traffic while User/A remained epoch1, and the first target request's ACK waited on Provider refresh. Exact-version rejection remains required; a short convergence probe cannot assume all peers instantly discover grant-only changes.
- **Fix**: record every control row and require zero control failures; the new regression fails before the change. Use the existing 1s status refresh knob for the normal grant probe, matching the corrected late-grant case and other bounded revocation tests. This is an explicit experiment/deployment setting, not a claim of instantaneous convergence under production defaults. Retain the 12/24 negative run; recheck earlier late-grant raw rows against the stronger control rule. Final normal grant rerun pending.
- **Ref**: `campaign-renewal/grant-only-advance`; `gates/harness-control-red.log`; T018.
- **Lesson**: key reuse and status-version convergence are separate conditions; count all control outcomes, not only successful ones.

## 2026-09-05 — asynchronous startup exposes three MiniNDN timing assumptions
- **Area**: Spec179 grant and offline-rejoin probes (T018)
- **Symptom**: rebuilt full campaign finished 13/16, exit 1. Normal grant had no renewal and zero granted requests; late grant had 21/21 successes but no exhausted permission retries; offline User installed epoch 2 before reaching epoch 3.
- **Root cause**: old constructor blocking implicitly deferred permission discovery until grant. Once constructors return, an empty permission response completes normally, so absence of a grant does not force transport retry exhaustion. Fixed SIGCONT timing relied on old startup skew and could precede the actual epoch-3 revoke. The late control also needed scheduled status renewal for its 60-second window.
- **Fix**: both grant probes use explicit App refetch; late probe drops outgoing UDP only inside the isolated user-b namespace until observed final permission timeout, then removes the exact rule in finally. A namespace guard refuses host execution. Keep control statuses renewed through existing knobs. Offline SIGCONT waits for the actual revoke marker. No failed rows or ordering checks are removed. Three focused MiniNDN reruns pending.
- **Ref**: `results/spec179-online-auth-20260905/campaign-final/`; `permission-startup-loss.json` in the corrected late run records both fault boundaries. Native binaries and libraries are unchanged for the rerun.
- **Lesson**: test prerequisites must be observed, not inferred from constructor latency, empty responses, or estimated mutation deadlines.

## 2026-09-05 — stream retry timing assertion fails alongside compilation
- **Area**: Spec179 final integration verification / shared-host load
- **Symptom**: `NormalStreamRetriesOneSuppressedEventFromProviderIms` completed successfully but recorded two retries instead of exactly one; the expanded gate returned 201 (70/71 cases, 1269/1270 assertions).
- **Root cause**: the test uses a 100ms Interest lifetime; concurrent `-j2` App compilation is a plausible scheduling cause, not yet established by a controlled load experiment. No source change to the stream implementation occurred in this repair.
- **Fix**: retained `gates/integration-final.log`, finished compilation and reran without compilation load. The isolated case passed immediately (12/12 assertions, exit 0); full isolated integration gate passed 71/71 cases and 1270/1270 assertions (`gates/integration-final-isolated.log`). No retry assertion or timeout was weakened.
- **Lesson**: independent binaries avoid link races but do not isolate timing-sensitive tests from shared CPU pressure. Run the final timing gate without compilation.

## 2026-09-05 — unprovisioned runtime cannot reach online permission renewal
- **Area**: Spec179 User/Provider construction and online grant (T018)
- **Symptom**: late-grant MiniNDN probe failed the App_User readiness deadline; no permission fetch or App refetch marker appeared, only repeated Waiting for decryption key lines. The original first-grant scenario began the App only after grant unlocked construction.
- **Root cause**: both real constructors synchronously pump their Face until Consumer has a DKEY. An identity with no grant cannot finish construction to call the App-owned permission API. This corrects the earlier startup-delay hypothesis: the relevant delay was the DKEY gate, not slow RSA initialization.
- **Fix**: begin the existing asynchronous Consumer fetch and return with an explicit bootstrap-pending marker. Keep all permission/status/key checks. Timed real User/Provider constructor coverage passes 8/8 assertions with zero unauthorized publication/execution; final late-grant network rerun remains pending.
- **Ref**: campaign/grant-after-permission-exhaustion; ServiceUser/ServiceProvider constructors; UnprovisionedRuntimesConstructAndRemainUnauthorized.
- **Lesson**: an application-owned recovery API is unusable if construction blocks waiting for the condition that API must recover.

## 2026-09-05 — one-second benchmark drain truncates delayed valid responses
- **Area**: Spec179 MiniNDN workload shutdown
- **Symptom**: corrected-duration inflight-revocation run reported two unsuccessful unaffected-user requests near workload end, despite ten successful post-revoke requests.
- **Root cause**: the launcher allowed only one second of drain for a three-second Provider delay and five-second request timeout. Open-loop finalization emitted incomplete rows before valid in-flight work could finish.
- **Fix**: drain six seconds (five-second request timeout plus margin), use at least 35-second role windows for the standard scenarios, retain all failures and rerun. No success filter is added.
- **Ref**: campaign/inflight-revocation; App_User drainDeadline; T018.
- **Lesson**: measured-window completion and process survival must include the entire request drain budget.

## 2026-09-05 — failed withdrawal bypassed by grant or same-target retry
- **Area**: Spec179 Controller pending ABE rotation
- **Symptom**: the new real Controller regression produced 10 failed assertions: same-target retry left the old ABE pair; grant removed the revocation during injected rotation failure; direct recovery produced an equal-version conflicting status rejected by RevocationState.
- **Root cause**: duplicate-target return preceded reconciliation; grant never checked pending rotation; reconciliation reused a ControllerVersion whose old parameter identity could already be published.
- **Fix**: reconcile before duplicate handling and grant mutation, persist a newer epoch before recovery crypto work, return successful completion for the pending same-target retry, and retain ordinary completed-duplicate no-op behavior. One-shot injection and a repeated App revoke support the matching MiniNDN scenario.
- **Ref**: T017; PendingRotationFencesGrantAndPreservesImmutableStatus; `results/spec179-online-auth-20260905/gates/controller-red.log` (exit 201, 10 failed assertions) and `controller-green.log` (exit 0, 36/36 assertions, including old/replacement DKEY decryption). MiniNDN `campaign/revocation-rotation-failure-retry`: 14/14 checks pass, epoch 2 -> 3, affected denial throughout, 16/16 unaffected post-recovery calls succeed.
- **Lesson**: failure recovery is an authorization mutation too; test every public entry and preserve already published immutable status identities.

## 2026-09-05 — late-grant probe initially missed its timing contract
- **Area**: Spec179 MiniNDN T018 probe
- **Symptom**: first late-grant run completed but gate failed (exit 4): user/B started after the Controller grant, no startup timeout exhaustion occurred, and the result exporter omitted the new scenario's grant evidence.
- **Root cause**: startup outlasted the 12-second grant offset; the later probe identified the constructor DKEY wait (see the entry above), superseding the initial RSA-delay hypothesis. One scenario-name equality remained in the evidence return despite sharing the grant collector.
- **Fix**: grant evidence now follows the grantOnlyAdvance configuration for both scenarios; added an exporter regression. Increased grant/renewal/workload windows and require measured exhaustion < grant < refetch <= first invocation plus post-refetch unaffected successes. First run retained as a failed timing probe.
- **Ref**: results/spec179-online-auth-20260905/late-grant-first; 12 launcher tests pass. Corrected network rerun pending.
- **Lesson**: launch offsets are assumptions; acceptance must verify event ordering from observed timestamps.

## 2026-09-05 — MiniNDN open-loop milliseconds interpreted as seconds
- **Area**: Spec179 launcher workload lifetime
- **Symptom**: a run configured for 16 seconds kept enqueueing until the process lifetime killed it; late-grant-first user/A enqueued for about 51 seconds despite the nominal workload/count.
- **Root cause**: requestDurationMs was passed directly to App_User --duration, which uses std::chrono::seconds; --count applies to closed-loop mode and does not cap this open-loop workload. Teardown could therefore truncate in-flight requests, previously hidden by the grant collector.
- **Fix**: round milliseconds up to integer seconds at the App_User command boundary. The final late-grant probe uses 60-second workloads for both users, spanning explicit renewal at 40 seconds after App startup; the fault/retry scenario explicitly spans both mutation events and drains before process shutdown.
- **Ref**: examples/App_User.cpp openLoopDurationSeconds and measurementStopAt; run_request_scoped_confidentiality.py user_command; T018.
- **Lesson**: verify units at the actual CLI consumer and distinguish open-loop duration from closed-loop count.

## 2026-09-05 — online authorization audit detects censored MiniNDN failures
- **Area**: Spec179 MiniNDN evidence and exit status
- **Symptom**: a success plus a failed request while providers were alive was counted as one successful row; a completed run with gatePassed=false returned exit code 0.
- **Root cause**: the grant collector used the earliest provider log timestamp as a termination cutoff and dropped failure rows; main checked process completion alone.
- **Fix**: retain all terminal rows, allow bootstrap/workload/drain in the grant scenario lifetime, and require gatePassed=true for exit 0. Two regression cases reproduced both defects before the fix; the 11-case launcher suite passed afterward.
- **Ref**: tests/minindn/test_request_scoped_confidentiality.py; /tmp/spec179-online-auth-harness-red.log and harness-green.log; T018.
- **Lesson**: process completion is not a security gate; never infer teardown from a first log or silently discard negative evidence.
- **Real-run confirmation**: reprocessing `results/spec179-online-auth-20260905/baseline-grant` retained 16 requests with 14 successes and 2 timeouts. The old collector had reported 14/14. The campaign driver now propagates failed gates, uses fresh output directories, and refuses to overwrite retained scenarios; each new manifest records revision, working diff and executable/library hashes.

## 2026-09-05 — online authorization audit preflight and test authoring corrections
- **Area**: Context Mode and Controller regression fixture
- **Symptom**: active authority hashes were stale; project query guard rejected a low-entropy identifier and then an identifier absent from its query; a new C++ regression did not compile.
- **Root cause**: prior Spec edits were not indexed; malformed guard arguments; makeServiceRevocation takes const char* rather than std::string.
- **Fix**: reindexed canonical authority documents, verified active health, corrected and reran the guarded project query; passed the temporary URI through c_str for the immediate copying helper call.
- **Lesson**: use file-backed checkpoints after retrieval failures and verify fixture signatures before writing a regression. An initially rejected query is not accepted authority.

Append-only engineering failure record. Rule (AGENTS.md): every failure that
costs non-trivial debugging MUST be appended here **in the same checkpoint
commit that fixes or records it**. New tasks MUST read the recent entries as
part of task context. Format per entry:

```text
## <date> — <one-line symptom>
- **Area**: <spec or module>
- **Symptom**: <what was observed>
- **Root cause**: <why>
- **Fix**: <what changed / workaround>
- **Ref**: <commit, evidence file, or script>
- **Lesson**: <one line to carry forward>
```

## 2026-09-05 — git index duplicate entries wrote a corrupted tree
- **Area**: tooling/git
- **Symptom**: `git add -A` with a pathspec containing a comma staged
  duplicate index entries; the resulting commit tree had `duplicateEntries`
  + `treeNotSorted` (`git fsck` errors), and a rename-detection warning
  "duplicate destination".
- **Root cause**: comma pathspec left the index with unordered/duplicate
  stage entries.
- **Fix**: `rm .git/index && git reset --mixed <last-good>` rebuilt the
  index; re-staged with explicit paths; `git prune --expire=now` dropped
  the bad commit.
- **Ref**: NDNSF commits `32b1fc23` (re-created) replacing the bad
  `8fc879ce`.
- **Lesson**: never use `git add -A` with comma/odd pathspecs; verify
  `git ls-files | sort | uniq -d` is empty before committing.

## 2026-09-05 — stale campaign-summary.tsv contradicted final MiniNDN results
- **Area**: Spec179 evidence
- **Symptom**: `results/spec179-minindn/campaign-summary.tsv` showed many
  scenarios `gatePassed=False` while per-scenario `result.json` said true.
- **Root cause**: the TSV predated the final campaign runs (13:08 vs runs
  15:47–17:18) and was never regenerated.
- **Fix**: regenerated from the final `result.json` files (14/14
  `gatePassed=True`).
- **Ref**: Spec179 `evidence/post-implementation-audit.md` R179-A4.
- **Lesson**: derived summary artifacts must carry a timestamp and be
  regenerated, or deleted, after the runs they summarize.

## 2026-09-04 — MiniNDN campaign caught two admission defects
- **Area**: Spec179 runtime
- **Symptom**: S9 — `RequestServiceTargeted` issued versionless requests;
  S10 — `requestServiceStreamingBytes` discarded the admission result so a
  denied stream start logged STARTED.
- **Root cause**: missing version binding on the Targeted request path;
  ignored revocation admission result on the stream start path.
- **Fix**: fixed in `ServiceUser.cpp`; rebuilt; genuine campaign rerun
  green.
- **Ref**: `evidence/minindn-campaign-20260904.md`, NDNSF commit `e7ea0a74`.
- **Lesson**: the cross-process campaign is the admission-boundary oracle;
  component tests did not catch either defect.

## 2026-09-03 — OpenABE mixed-generation decrypt returns garbage
- **Area**: NAC-ABE/OpenABE crypto
- **Symptom**: decrypting new-generation ciphertext with a retained old
  DKEY could return garbage plaintext instead of throwing.
- **Root cause**: OpenABE generation mismatch does not always fail loudly.
- **Fix**: Spec179 test assertions use `decryptFailsClosed` (throw **or**
  recovery failure both accepted); RV-U20 mixed-generation matrix with
  fresh ciphertext per case (the ABESupport singleton CK cache would
  otherwise mask the mismatch).
- **Ref**: Spec179 `evidence/runtime-revocation-lifecycle-20260904.md`.
- **Lesson**: crypto-negative assertions must accept "recovered garbage"
  as failure; never reuse a successfully-decrypted ciphertext in a
  generation-mismatch case.

## 2026-09-03 — NAC-ABE stale DKEY after grant-only policy replacement
- **Area**: NAC-ABE dependency
- **Symptom**: target refresh could receive the previous complete DKEY.
- **Root cause**: DKEY segments published with `FreshnessPeriod=4s`; the
  unversioned `MustBeFresh` discovery Interest then hit a still-fresh
  Content Store copy of the old policy.
- **Fix**: DKEY segments now publish with `FreshnessPeriod=0`; exact
  versioned segment names remain retrievable.
- **Ref**: NAC-ABE `Experimental` branch commit `b1c9c4f` (not pushed).
- **Lesson**: any in-place policy replacement needs freshness discipline
  on unversioned discovery names.

## 2026-09-03 — versioned exact public-params Interest could never match
- **Area**: NAC-ABE dependency
- **Symptom**: after status installation,
  `refreshPublicParameters` with the exact
  `/PUBLIC-PARAMS/<ABE-TYPE>/v=<version>` name (CanBePrefix=false) timed
  out repeatedly.
- **Root cause**: `AttributeAuthority::onPublicParamsRequest`
  unconditionally appended `<ABE-TYPE>` + version to the Interest name,
  producing a Data name that can never satisfy the exact request.
- **Fix**: detect an already-versioned name and do not append again;
  ParamFetcher binds expected name/digest.
- **Ref**: NAC-ABE `Experimental` commit `b1c9c4f`.
- **Lesson**: producer-side name derivation must mirror every Interest
  shape the consumer may legally send.

## 2026-09-02/04 — build and test-environment traps (Spec179 baseline)
- **Area**: build/tests
- **Symptom** (three independent traps):
  1. GCC 9 ICE on `data-enc-dec.cpp` — NAC-ABE must be built with
     `clang++-10`.
  2. Two concurrent waf builds in different out dirs conflict on the
     shared lock and one is killed silently.
  3. After a full-suite SIGSEGV, Boost.Test keeps running and the
     residual process disturbs later timing runs — kill residuals before
     re-running.
- **Fix**: documented build recipe (clang++-10, single build at a time,
     kill-then-retest).
- **Ref**: Spec179 `evidence/restore-fixes-20260904.md`,
     `evidence/regression-red-green-20260904.md`.
- **Lesson**: environment traps must be recorded next to the build
  recipe, not rediscovered per session.

## 2026-09-02 — DummyClientFace hangs and LocalMock DKEY reattach
- **Area**: tests
- **Symptom**: `processEvents` blocked forever on a fully idle face;
  pump-driven LocalMock members could not verify DKEY segments.
- **Root cause**: deferred DKEY reattach had no bound when the face went
  idle.
- **Fix**: bounded retry (250 ms × 20) for deferred DKEY reattach;
  request-pump fixture extended to pump the AA face (Spec179 remounts).
- **Ref**: Spec179 baseline fixes in NDNSF commit `e7ea0a74`.
- **Lesson**: every deferred async retry needs a bounded schedule or an
  idle-face test can deadlock the whole suite.

## 2026-09-02 — SegmentFetcher infinite fetch on discovery Data
- **Area**: Core `ServiceProvider::replyFromIMS`
- **Symptom**: SegmentFetcher kept requesting segments until timeout.
- **Root cause**: discovery Data served from IMS lacked `FinalBlockId`.
- **Fix**: forward to the last contiguous IMS segment and set
  `FinalBlockId`.
- **Ref**: Spec179 baseline fixes.
- **Lesson**: any segmented Data served to a SegmentFetcher must carry a
  terminal marker or the fetch is unbounded.

## 2026-09-07 — Spec182 T006-D: worker child catch mislabels every chain rejection as DI_NATIVE_ONNX_WORKER_INTERNAL
- **Area**: spec182 T006-C/T006-D native worker wire protocol (frozen in
  T006-C); child side of `runNativeOnnxAssemblyWorkerMain`.
- **Symptom**: focused `Spec182OnnxActivation` case
  `ActivationRejectsCertifiedGraphPoisonThroughWorker` failed while T006-D
  verified the real worker binary: the frozen `reject-identity-digest`
  vector (poisoned `recipe.graphDigest`) surfaced from the parent transport
  as `DI_NATIVE_ONNX_WORKER_INTERNAL` instead of the required chain
  rejection code `DI_NATIVE_ONNX_RECIPE`.
- **Root cause**: off-by-one in the child catch of
  `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.cpp`
  (shipped in the T006-C commit). The guard was
  `what.compare(0, 14, "DI_NATIVE_ONNX_") == 0`, but the family literal is
  15 bytes; the three-argument `compare()` treats the whole literal as the
  right side, so it compared a 14-byte prefix of `what` against the whole
  15-byte literal — never equal. Every chain rejection (`fail(code)` →
  `"DI_NATIVE_ONNX_" + code`, S1-S7 of the certified chain) was therefore
  reclassified as `DI_NATIVE_ONNX_WORKER_INTERNAL`, and the parent
  transport (which propagates the worker error code verbatim when it
  carries the `DI_NATIVE_ONNX_` family prefix) relayed the wrong reason.
  The frozen suites had never pushed a reject row through the real worker
  child, so the defect only surfaced during T006-D activation coverage.
- **Fix**: bound 14 → 15 (`what.compare(0, 15, "DI_NATIVE_ONNX_") == 0`),
  restoring verbatim family-code propagation. A new frozen-lock case
  `Spec182OnnxWorkerProtocol/SubprocessChainRejectionPropagatesItsOwnCode`
  spawns the real worker with the frozen reject row and asserts the parent
  sees exactly `DI_NATIVE_ONNX_RECIPE`; registered in
  `tests/fixtures/spec182/case-manifest.json`.
- **Ref**: run dirs retained under `.codex-tmp/` (`t006d-*` logs);
  manifests `tests/fixtures/spec182/case-manifest.json`.
- **Lesson**: a protocol invariant ("reason-family code must arrive
  verbatim at the caller") needs at least one end-to-end lock that drives
  a real subprocess with a frozen reject vector; unit mocks of the child
  catch could not expose the string-compare bug.

## 2026-09-07 — Spec182 T007-A: wscript helper insertion broke _pin_compiler_toolchain; boost 1.71 cannot print std::vector
- **Area**: spec182 T007-A static Rust tokenizer link; root `wscript` and
  `tests/unit-tests/di-native-tokenizer.t.cpp`.
- **Symptom**: (1) configure failed rc=2 with `NameError: name 'tools' is
  not defined` at wscript ~line 152 — the `_ensure_tokenizer_bridge` helper
  had been inserted in the middle of `_pin_compiler_toolchain`, and that
  function's tail statements (`conf.env.NDNSF_LINKER`, the 'Closed C++
  toolchain' msg) dangled at 4-space indent, becoming the helper's last
  statements. (2) The compile fix then revealed a duplicate
  `_pin_compiler_toolchain` def (original tailless copy plus a reconstructed
  complete copy) — Python shadowing made it work but left ~25 lines of dead
  code. (3) `unit-tests` compile failed on `BOOST_REQUIRE_EQUAL(encode(), ids)`:
  boost 1.71's `print_helper` has no `operator<<` for `std::vector<long>`.
- **Root cause**: (1) `Edit` with an old_string ending mid-function appended
  the new helper inside the old function body; indentation kept the tail
  inside the helper. (3) boost 1.71 test-tools cannot stream a vector; the
  assertion is fine at runtime but does not compile.
- **Fix**: (1) re-emitted the helper as a complete module-level function and
  restored `_pin_compiler_toolchain` with its own tail; (2) deleted the dead
  original copy, keeping one documented def; configure rc=0 with 'Pinned
  Rust tokenizer staticlib' resolved. (3) switched vector equality to
  elementwise `BOOST_REQUIRE_EQUAL_COLLECTIONS` (three sites:
  `compareVectorCase`, owner-reuse roundtrip, concurrency baseline check).
- **Ref**: run dirs retained under `.codex-tmp/` (`t007-configure-r2.log`,
  `t007-build-r3.log`, `t007-full-regression.log`); cargo PATH lesson: the
  pinned rustc must be on PATH (`rust-prefix/bin`) or `cargo build` dies
  with "could not execute process `rustc -vV`".
- **Addendum（同卡）**: an unfiltered full run (`./build-nac182/unit-tests`,
  no exclusion) segfaulted rc=139 inside the known environment-dependent
  stream-facade family (`PredictiveProviderExactWireValidationAndAtomicFlush`,
  stream-facade.t.cpp:270 last checkpoint); negative preserved, same family
  every spec182 card excludes since T006-B/C/D. The card regression gate ran
  with `--run_test='!StreamFacade'` → 859 cases, No errors detected.
- **Lesson**: insert a new top-level `def` only with an old_string that ends
  at a module-level boundary (blank-line pair); after any structural waf
  edit, re-run configure before building. Assert `std::vector` equality in
  boost 1.71 with `EQUAL_COLLECTIONS`, never `REQUIRE_EQUAL`.

## 2026-09-07 — Spec182 T007-B: stale Rust staticlib silently kept old ABI; python codec vs Rust-std surrogate prefix divergence
- **Area**: spec182 T007-B stable-prefix decode; `wscript`
  `_ensure_tokenizer_bridge`, `tests/unit-tests/distributed-inference-tokenizer.t.cpp`.
- **Symptom**: after rewriting `tokenizer-bridge/src/lib.rs`, the focused
  `Spec182TokenizerStable/*` run still failed with the *old* behavior
  (ByteLevel prefix `[a != a�]`, reject-Fuse stable calls not throwing).
  A reconfigure rebuilt the archive (`13:03:24`) but `./waf build` finished
  in 17 s having relinked nothing: both `libndnsf-distributed-inference.so`
  (12:59) and `unit-tests` (13:01) predated the new archive, and a second
  failure signature then appeared (surrogate cut `[�� != ]`).
- **Root cause**: (1) the archive lives *outside* the build dir
  (`.codex-tmp/spec182-t001-dependencies/tokenizer-bridge-target/...`);
  waf links it by path and does not signature-track external STLIB files, so
  a changed archive alone never dirties the link task — and waf is
  content-hash based, so `touch`ing a source does not help either.
  (2) Authoring-proxy divergence: the frozen ByteLevel row expectations came
  from python's incremental UTF-8 codec (`errors=replace`), which *defers*
  a 3-byte-lead decision until its third byte; Rust std rejects `ED A0`
  eagerly (second byte must be 80..9F), so the surrogate row's mid-prefix
  cuts diverged (`""` vs `"��"`).
- **Fix**: (1) delete `build-nac182/libndnsf-distributed-inference.so` and
  `build-nac182/unit-tests`, then rebuild — outputs missing forces the link
  task to rerun against the new archive (verify with
  `ls --time-style` after every T007-class lib.rs change). (2) rewrote
  `author-stable-vectors.py`'s per-cut model as an explicit mirror of
  `std::str::from_utf8` error attribution (tight E0/ED/F0/F4 second-byte
  ranges, continuation consumption, trailing-incomplete `None`); HF decode
  stays the independent cross-check at full length for every row.
- **Ref**: ABI probes retained at `/tmp/t007b-abi-probe/` (probe.cpp,
  probe2.cpp) and `/tmp/t007b-surrogate-check/` (std-semantics micro
  checks); frozen vectors regenerated, whole-file sha256
  `a80597b3c96833a61a4dd22606b014715f62e2111e0bd7b3325cc97665bc6254`;
  fixture tokenizer shas unchanged.
- **Lesson**: after any `tokenizer-bridge/src/*.rs` change, delete the
  `build-nac182` `.so`/`unit-tests` link products (or run a second
  configure + build and verify mtimes) before trusting a test run; frozen
  per-prefix expectations for ByteLevel must be authored with Rust-std
  utf-8 semantics, not python codec semantics.

## 2026-09-08 — Spec182 T016 preflight: MiniNDN node context unavailable

- **Area**: T016-A local qualification preflight;
  `Experiments/NDNSF_DI_NativeClosure_Minindn.py` campaign owner.
- **Command**: `python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py
  --manifest .codex-tmp/spec182-t016-r2/manifest.json
  --output .codex-tmp/spec182-t016-r2/result` with `campaignCase=I01`.
- **Observed boundary**: exit `2`, result status `UNQUALIFIED`, reason
  `MININDN_NODE_CONTEXT_NOT_PROVIDED`; `/usr/bin/bwrap`, `strace` and `nsenter`
  are installed, but `/run/nfd/nfd.sock` is absent and no real MiniNDN
  `node/netns` metadata was supplied.
- **Interpretation**: this is a harness/environment preflight boundary, not an
  I01 protocol result and not a native no-Python PASS/FAIL. No business process,
  namespace or network request was started.
- **Raw evidence**: `.codex-tmp/spec182-t016-r2/` (manifest, stdout/stderr and
  persisted result.json). Keep this run immutable; a later retry must use a new
  run directory after node/NFD context is available.
- **Next step**: provide the externally owned MiniNDN node/netns/socket context,
  then rerun the complete T016 matrix with a fresh raw run directory.

## 2026-09-08 — Spec182 R6-B7: current binary drops one legacy D2b Selection before provider1 callback

- **Area**: legacy Spec170 `DATA_V1` compatibility path exercised by the
  current Spec182-linked `integration-tests` binary;
  `Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2bSelectionIntoSvsDataV1`.
- **Symptom**: current `build-nac182/integration-tests` returns rc=201. ACK
  collection and collaboration plan commit pass, and provider0 receives,
  decrypts, queues, and completes its Selection. User trace records both
  provider1 and provider0 Selection publications, but provider1 records no
  Selection callback; `provider0Published`, `provider1HandlerCalled`, and the
  bounded DATA_V1 fetch assertion fail. The older `build-system-j2` binary
  passes the isolated selector.
- **First boundary**: after User `PublishServiceSelectionMessageV2` publication
  (both `SVS_PUBLISH_DONE` entries are present) and before provider1's
  `ServiceProvider::handleServiceSelectionMessage` callback. ACK, plan, and
  provider0 execution are not the failing boundary.
- **Attempts**: isolated run with User/Provider TRACE; repeat with
  `NDNSF_HANDLER_THREADS=0`. The handler-thread override did not change the
  failure. An exploratory full `-j4` rebuild was interrupted after `vmstat 1`
  showed sustained swap-in/out; no build result from that attempt is used as
  validation. Raw immutable logs are under
  `.codex-tmp/spec182-r6-b7-legacy-d2b-20260908/`.
- **Interpretation**: preserve this as a current-source compatibility/runtime
  boundary. Do not change `ServiceProvider::isFresh` or claim a Spec182
  qualification result until a focused probe identifies the provider1
  receipt/newness ordering and a named regression selector is added.
- **Next step**: inspect the exact SVS delivery/newness interleaving, then either
  make a bounded compatibility repair with an independent selector or retain
  the failure as a T013-B migration prerequisite. See
  `specs/182-native-di-python-bindings/evidence/r6-b7-legacy-d2b-regression-20260908.md`.

## Proposal authorization PPTX build-path preflight — 2026-09-08

生成器拒绝末级名称不以 `ndnsf-` 开头的临时目录，未覆盖交付物。
首个失败边界为目录安全检查，不是 slides 内容错误。
原始日志：`.codex-tmp/proposal-authorization-20260908-docs/logs/pptx-build.log`。
采用新的合规目录后生成通过，1944/1944 文本 spans 分配通过；详见
`docs/PAPER/proposal-defense/authorization-revision.md`。前述正文引用失败也已通过隔离重建解决。

## 2026-09-09 — Spec182 R7-B2 alternate replacement boundaries

R7-B2 首次批次构建在 `tests/integration-tests/ndnsf-di-core-flow.t.cpp:952` 触发
helper brace/syntax cascade，首个边界是 C++ 编译器解析失败；原始输出保留在
`.codex-tmp/spec182-r7-b2-replacement-map-20260909/build.log`，修复后同一 source
closure 构建通过。

随后双 Provider alternate selector 首次运行到 recovery Selection，但旧的 V2 parser
把 `/NDNSF/DI/REQUEST/1/recovery/2` 截短为 `/NDNSF/DI/REQUEST/1/recovery`，在
provider1 callback 前以 `SELECTION_NO_PENDING` 暴露。首个运行边界不是 Provider 执行或
checkpoint 提交；完整 trace 保留在
`.codex-tmp/spec182-r7-b2-replacement-map-20260909-r4/integration-alternate-trace2.log`。
parser 修复后增加结构化 recovery、结构化 decision 和 legacy `request-1/3` 回归，全部
通过。最终批次结果与未观测的跨进程/T016边界见
[R7-B2 evidence](../specs/182-native-di-python-bindings/evidence/r7-b2-alternate-provider-replacement-20260909.md)。

## 2026-09-09 — Spec182 R6-B9: bounded repair passes D2b but broad suite exposes later D2h crash

- **Area**: current-source legacy Spec170 D2b Selection freshness and `DATA_V1` ingress;
  `ServiceProvider::isFresh`.
- **First boundary and repair**: R6-B7's trace showed provider0 sequence 4 arriving before
  provider1 sequence 3. The old single producer/session frontier discarded the unseen provider1
  publication before its callback. The bounded repair keeps old-session rejection and same-name
  duplicate fencing, while storing sequence frontiers per publication name under `svs_mutex`.
- **Focused result**: current `integration-tests` was rebuilt with system-first `-j4` (118/118,
  exit 0, Waf 1m37.712s). The five D2b selectors and the named
  `ProductionNativeHandlersRunD2h212ToCompleteOracleResponse` selector each exited 0 in isolated
  runs; raw logs are under `.codex-tmp/spec182-r6-b9/`.
- **Remaining failure boundary**: the first unfiltered `Spec170NdnsfDiCoreFlow/*` attempt reached
  the D2h production case with a missing response/role and `double free or corruption`; the same
  boundary reproduced in `2/20` isolated D2h212 repeats. Two subsequent fresh unfiltered runs and
  all `50/50` D2b repeats exited 0, so this remains an intermittent independent runtime/test
  observation rather than an attributed D2b regression.
- **Interpretation**: R6-B9 is `CLOSED_FOR_VALIDATION` only for the local D2b freshness behavior;
  T013-B/T013-C, cross-process compatibility and T016 qualification remain `PARTIAL`/open.
  See [R6-B9 evidence](../specs/182-native-di-python-bindings/evidence/r6-b9-legacy-d2b-freshness-20260909.md).

### 2026-09-09 — R6-B9 follow-up stability observation

The historical D2h212 boundary remains preserved in the R6-B9 run directory (`2/20` failures
with callback/role loss and `double free or corruption`). A later fresh-process rerun of
`ProductionNativeHandlersRunD2h212ToCompleteOracleResponse`, with dependency and NDNSF tracing
enabled, passed `20/20` without timeout; logs are under
`.codex-tmp/spec182-r6-b9/d2h212-dep-repeat20/`. This narrows current reproducibility but does
not explain the historical interleaving, prove cross-process behavior, or close T016.

### 2026-09-09 — R9-B1 D2h212 selection-status UAF

The longer D2h212 sample reproduced `SIGABRT`/`double free or corruption`. An ASAN allocator
preload identified the first invalid access as a heap-use-after-free in
`ServiceProvider::reportSelectionOperationStatus`: concurrent Provider workers appended to the
same `memberStatuses` vector while another worker wrote an element from storage invalidated by
reallocation. The status map was also read by the Face query path without a snapshot lock. The
repair added `m_selectionExecutionStatusMutex` around report/update/get, without changing the
status wire or state-transition contract.

After the repair, the concurrent unit selector passed, D2h212 passed `50/50` fresh processes,
and an ASAN-preload follow-up passed `20/20` with allocator type-size mismatch diagnostics
disabled (the uninstrumented SVSPubSub dependency otherwise reports a non-product size warning).
The original R6-B9 logs remain preserved; cross-process status publication and T016 qualification
are still unobserved.

## 2026-09-09 — Spec182 R10-B5 Boost.Test filter setup miss

- **Area**: R10-B5 Provider REPO_REF execution selector; regression command boundary.
- **First boundary**: the command combining three `--run_test` values with commas returned
  Boost.Test setup code `200` (`no test cases matching filter or all test cases were disabled`)
  before entering any test case. No Provider, network, or native execution started.
- **Interpretation**: this is a test-command syntax/harness miss, not a product failure. The
  corrected suite selector `Spec170NativePostSelection` entered all five cases and passed,
  including the new REPO_REF case.
- **Raw evidence**: `.codex-tmp/spec182-r10-b5/invalid-filter.log` and
  `.codex-tmp/spec182-r10-b5/regression-suite.log`; keep both immutable with the build and
  `vmstat` logs in the same run directory.
- **Next step**: use suite selectors or verified Boost.Test filter syntax for this target; retain
  the failed command in the batch retrospective as a `runtime/test` setup miss.

## 2026-09-09 — Spec182 R10-B6 missing REPO_REF negative exceeded local pump boundary

- **Area**: R10-B6 Provider REPO_REF fail-closed negative; missing encrypted input object.
- **First boundary**: the production handler entered and did not enter the runner, but the
  missing-object fetch did not reach `CollaborationContext::fail` before the fixture's fixed
  200-round (about 3 s) pump ended. The assertions for `statusFailed` and the failure reason
  therefore failed; no successful response was observed. The size-mismatch and malformed-envelope
  cases in the same suite passed.
- **Interpretation**: this is an observed fetch-timeout/test-harness boundary, not a Provider
  success. `ServiceProvider::fetchAndDecryptLargeData` uses the shared
  `NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS` budget (default 30 s) and may try the legacy fallback,
  while the helper pump stops earlier. The missing-object result remains unqualified until the
  test uses a deterministic parser-level missing-reference case or an explicitly bounded fetch
  budget.
- **Raw evidence**: `.codex-tmp/spec182-r10-b6/suite-missing-failure.log`, `build.log`, and
  `vmstat.log`.
- **Next step**: repair the negative fixture so its missing-reference boundary is deterministic,
  then rerun the complete `Spec170NativePostSelection` suite before closing R10-B6.

## 2026-09-09 — Spec182 R10-B38 T016 runtime context recheck remains unqualified

- **Area**: T016-A MiniNDN qualification owner preflight after R10-B37.
- **First boundaries**: a fresh default run in `.codex-tmp/spec182-t016-r6/` returned exit `2`,
  `UNQUALIFIED/MININDN_NODE_CONTEXT_NOT_PROVIDED` before business startup. A separate fresh
  `--execute-owner` run in `.codex-tmp/spec182-t016-r7/` returned exit `2`,
  `UNQUALIFIED/MININDN_REQUIRES_ROOT` because the current process is UID 1000.
- **Observed host state**: `/run/nfd/nfd.sock` exists and a system `nfd` process is running, but
  no MiniNDN requester/provider node PID/starttime, network namespace, per-node NFD socket or peer
  metadata is available. The socket alone does not satisfy the owner contract.
- **Interpretation**: these are campaign preflight/environment boundaries, not protocol PASS/FAIL
  and not no-Python evidence. No requester/provider business process, namespace, network request or
  product build was started.
- **Raw evidence**: `.codex-tmp/spec182-t016-r6/` and `.codex-tmp/spec182-t016-r7/`; keep both
  immutable and do not overwrite the earlier R6-B4 preflight.
- **Next step**: provide a root MiniNDN owner context with identity-bound namespaces, PID starttimes,
  independent NFD sockets and peer metadata, then run the complete I01–I08/PO matrix in a new run
  directory.

## 2026-09-09 — Spec182 R10-B40 PO-001 raw manifest preflight retries

- **Area**: T016-A PO-001 owner/runner execution after root owner access was available.
- **First boundaries**: fresh `.codex-tmp/spec182-t016-r10/` reached the canonical runner but stopped
  at `PreflightError: process role is invalid`; fresh `.codex-tmp/spec182-t016-r11/` then stopped at
  `PreflightError: artifact digest mismatch` for the existing `integration-tests` executable.
- **Interpretation**: both failures were runner-manifest identity/configuration boundaries, not native
  protocol results. The old manifest omitted the required `process.role` and carried a stale binary
  digest; neither attempt is qualification evidence.
- **Repair and result**: `.codex-tmp/spec182-t016-r12/` used a fresh manifest with the requester role
  and current executable digest. PO-001 then completed with runner evaluation `PASS`, rc `0`, the
  `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` marker, and complete namespace/process/trace/cleanup evidence.
- **Raw evidence**: preserve `r10`, `r11`, and `r12` under `.codex-tmp/spec182-t016-*`; the durable
  bounded result is [R10-B40 evidence](../specs/182-native-di-python-bindings/evidence/r10-b40-t016-po001-native-owner-pass-20260909.md).
- **Scope correction**: r12 proves an isolated native process inside an owner-created namespace;
  the PO-001 Provider callback is still supplied by the in-process integration fixture. It does not
  prove independent requester/Provider process transport.
- **Next step**: generate each remaining I01–I08/PO-002–PO-014 manifest from the current build
  identity before execution; do not reuse the stale raw manifest.

## 2026-09-09 — Spec182 R10-B41 stream business-oracle and artifact identity retry

- **Area**: T016 PO-001 stream-only owner/runner after the native result-marker repair.
- **First boundaries**: r13/r14 preserved the earlier stale-selector and missing-marker observations.
  After adding the marker to the unary and stream-only result branches, r15 still returned
  `UNQUALIFIED/MISSING_EVIDENCE:business-oracle` because its fresh runner staged the older
  `build-nac182/integration-tests` artifact. Structural observation was complete and the native
  process returned `0`; the failure was an artifact-source identity boundary, not a protocol
  failure.
- **Changed gate and result**: r16 bound the runner manifest to the actual fresh linked output
  `.codex-tmp/spec182-r4-b2/build/integration-tests` and recomputed its digest. PO-001 then
  returned `PASS` with `SPEC182_NATIVE_DI_REQUEST_RESULT_OK`, complete namespace/identity/
  process-tree/endpoint/trace/cleanup evidence, and owner exit `0`.
- **Interpretation**: close only the stream/unary business-oracle and one isolated native-process
  acceptance boundary. The Provider callback remains in the in-process fixture; independent
  requester/Provider transport, I01–I08, PO-002–PO-014, maintained caller/no-Python and full
  T016 qualification remain open.
- **Durable evidence**: [R10-B41 evidence](../specs/182-native-di-python-bindings/evidence/r10-b41-stream-business-oracle-20260909.md).
- **Raw runs**: `.codex-tmp/spec182-r10-b41/`, `.codex-tmp/spec182-t016-r13/`,
  `.codex-tmp/spec182-t016-r14/`, `.codex-tmp/spec182-t016-r15/`, and
  `.codex-tmp/spec182-t016-r16/`.

## 2026-09-09 — Spec182 R10-B50 Provider executable link closure

- **Area**: P2 preparation for an independent native Provider executable.
- **First boundary**: `./waf build --targets=di-native-provider -j2` compiled all 72 tasks but
  failed at the final link with unresolved conversation journal/wire, selection JSON, request
  envelope, and placement symbols. No Provider process or protocol request started.
- **Interpretation**: this is an examples `source` registration/link-closure miss. It is not a
  protocol result, MiniNDN result, or qualification failure. The missing translation units were
  identified by symbol-definition lookup and added to `di_native_session_sources`.
- **Raw evidence**: `.codex-tmp/spec182-r10-b50-provider-build/` plus the durable
  [R10-B50 evidence](../specs/182-native-di-python-bindings/evidence/r10-b50-provider-link-closure-20260909.md).
- **Next step**: rerun the same system-first `-j2` target build, then record a fresh source/link
  identity and run only a bounded Provider executable smoke before attempting cross-process T016.

- **Follow-up boundary**: the first repair removed the initial six unresolved symbol groups, but
  the next `-j2` link still stopped on `NativeSignedGrantRequest::sign`,
  `NativeArtifactGrantIssuer::issue`, and `planNativeRequest`. These map to
  `NativeArtifactPolicyAuthority.cpp` and `NativeRequestPlanner.cpp`; preserve this second link
  boundary in the same R10-B50 evidence before retrying.

- **Follow-up boundary 2**: after adding those two translation units, the next link stopped on
  `NativeOfferAdmission::verify`, `NativeCanonicalPreparationCatalog::bindStateContracts`,
  `NativePlanProjectionBuilder::build`, `NativeGroupKeyAdmission`, and
  `NativeGroupProjectionBuilder::build`. The definitions are in five additional native source
  files recorded in the R10-B50 Changed gate; no Provider process started.

## 2026-09-09 — Spec182 R10-B53 plan/ONNX smoke runtime loader boundary

- **Area**: local `di-native-plan-onnx-smoke` executable after its source/link build.
- **First boundary**: the target compiled and linked 86/86 tasks, but process startup failed with
  `symbol lookup error: undefined symbol: ServiceUser::publishSignedAppData`, exit `127`, before
  plan parsing or model execution. `ldd` resolved `libndn-service-framework.so.0.1.0` from
  `/usr/local/lib`; that library does not export the required symbol. No protocol or model result
  was observed.
- **Interpretation**: this is a runtime shared-library identity/RUNPATH mismatch between the
  Spec182 build tree and `/usr/local`, not an ONNX or Provider behavior failure. The raw command,
  loader output and build log are retained under `.codex-tmp/spec182-r10-b53-plan-onnx-smoke-20260909/`.
- **Changed gate before retry**: verify the candidate build-tree `libndn-service-framework.so`
  exports the symbol and run the same binary with an explicit candidate `LD_LIBRARY_PATH`; compare
  `ldd` paths and symbol lookup before classifying any smoke result. Do not overwrite the first
  `rc=127` log or treat a corrected loader path as a protocol qualification.

- **Follow-up boundary**: with the candidate library explicitly loaded, the process passed dynamic
  symbol resolution but failed during ONNX model load because manifest artifact paths are relative
  (`artifacts/qwen-native-tracer-backbone.onnx`) and the retry was launched from the repository
  root. Exit `2`; no role executed. The output is retained as `smoke-candidate-lib.log` in the same
  raw run directory. The changed gate is to launch from the bundle root and recheck `ldd` selects
  the candidate framework library before interpreting model/session behavior.

- **RPATH retry command boundary**: after adding the target `$ORIGIN/..` RUNPATH, the relink and
  `ldd` check succeeded, but the first no-`LD_LIBRARY_PATH` retry expanded `$PWD` after changing
  into the bundle directory and therefore pointed at a nonexistent nested binary path (`rc=127`).
  This is a harness command construction error; its raw output is in
  `.codex-tmp/spec182-r10-b53-rpath-retry-20260909/`. The corrected retry fixes the repository
  root path before `cd` and keeps the bundle cwd only for artifact resolution.

## 2026-09-09 — Spec182 R10-B54 plan/manifest smoke source-closure boundary

- **Area**: local `di-native-plan-manifest-smoke` executable covering native plan/manifest
  parsing, role registration and dependency publication.
- **First boundary**: the initial 55/55-task `-j2` link stopped with unresolved native ONNX
  planning/recipe helpers and `ServiceUser` publication/collaboration methods. No smoke process
  or protocol request started; the raw linker output is retained in
  `.codex-tmp/spec182-r10-b54-plan-manifest-smoke-20260909/build.log`.
- **Interpretation**: this was a target source/link closure miss. The target's source list omitted
  `di_native_onnx_assembly_sources`, and its link closure omitted the candidate
  `ndn-service-framework`/ONNX/Protobuf dependencies. It is not a plan, manifest or protocol
  behavior result.
- **Changed gate before retry**: map every unresolved project symbol to its defining translation
  unit, add that source set and the shared framework/dependency closure to `examples/wscript`,
  add the target RUNPATH, then run a fresh target link and default-loader `ldd` check before
  interpreting smoke output. This source-definition map and target registration check is now a
  required feedback item for the shared Spec Kit build/source-closure gate.

## 2026-09-09 — Spec182 R10-B55 Provider serve preflight argument boundary

- **Area**: bounded startup probe for the repaired standalone Provider `--serve` path.
- **First boundary**: the first probe supplied plan/manifest and serving options but omitted the
  required `--serve` mode flag. `parseArgs` rejected the invocation with
  `exactly one of --check-only or --serve is required` (exit `2`), before Face creation or any
  Provider registration. Raw output is retained in
  `.codex-tmp/spec182-r10-b55-provider-serve-preflight-20260909/serve.log`.
- **Interpretation**: this is a harness command-construction/CLI boundary, not a serving,
  permission, protocol or qualification result.
- **Changed gate before retry**: assert the explicit `--serve` mode in the command and retain the
  metadata-only manifest (no preassembled artifact paths); classify only startup markers and the
  first NFD/certificate/permission/readiness boundary from the corrected bounded probe.

- **Follow-up collection boundary**: the Controller-assisted retry itself reached
  `NDNSF_DI_NATIVE_PROVIDER_READY` and the Controller exited cleanly, but the command's post-run
  `rg` marker extraction returned `command not found` because the runtime-only PATH intentionally
  omitted the developer `rg` location. Provider/Controller logs and exit codes were preserved;
  marker extraction was repeated with system `grep`/direct file reads. This is a harness collection
  issue, not a Provider or permission result.

## 2026-09-09 — Spec182 R10-B75 Provider failure-exit repair build boundary

- **Area**: Provider failure-observability repair after the R10-B74 static audit.
- **First boundary**: the first rebuild after adding the scheduler-stop API failed during
  `ServiceProvider.cpp` compilation because `ndn::scheduler::ScopedEventId` has no `reset()`
  member. No executable was linked from that attempt. The raw command and compiler boundary are
  retained under `.codex-tmp/spec182-r10-b75-provider-failure-exit-20260909-r0-build-j4-fail/`.
- **Changed gate before retry**: check the exact ndn-cxx handle API, replace `reset()` with move
  assignment to an empty scoped event, and reduce the next build to `-j2` after `vmstat` showed
  sustained swap-in/out on this host.
- **Follow-up behavior boundary**: after the compile repair, the first standalone Provider
  failure probe still timed out because the Face event loop was restarted by the NDNSF heartbeat
  and permission retry schedulers. The raw timeout runs are retained under
  `.codex-tmp/spec182-r10-b75-provider-failure-exit-20260909-r1` through `r10`.
- **Final result**: stopping the Face `io_context` from the failure task, then cancelling the
  Provider heartbeat scheduler on the main thread, produced rc=2 in 1.08 seconds after
  `NDNSF_DI_NATIVE_PROVIDER_PROVISION_FAILED`. This closes failure observability for this bounded
  CLI boundary; it does not establish requester/Provider transport or qualification.

## 2026-09-10 — Spec182 R10-B80 finite Provider probe startup boundary

- **Area**: finite standalone Provider serve probe after the R10-B78 run-limit cancellation repair.
- **First boundary**: the first probe used the fresh Provider binary without a running local
  Controller. Face and serving markers were emitted, then the event loop raised
  `Failed to fetch public parameters after multiple attempts.` and the process returned rc=2
  before the run-limit marker. Raw output is retained in
  `.codex-tmp/spec182-r10-b80-provider-run-limit.log` with its rc record.
- **Interpretation**: this is a local Controller/public-parameter startup boundary, not evidence
  against the run-limit repair or a protocol result.
- **Changed gate before retry**: start the real local `App_ServiceController` with the recorded
  temporary policy, keep the four-role plan and metadata-only manifest unchanged, and classify
  only the Provider `SERVE_READY`, run-limit, permission-wait cancellation, and process exit
  markers.
- **Follow-up result**: the Controller-assisted probe reached `SERVE_READY`, emitted
  `RUN_LIMIT_REACHED` and `PERMISSION_WAIT_CANCELLED`, and Provider/Controller both exited
  rc=0. Raw output is retained under
  `.codex-tmp/spec182-r10-b80-provider-run-limit-controller/`; this remains a bounded lifetime
  result and not requester/Provider transport qualification.

## 2026-09-10 — Spec182 R11-B8-G6 native process fixture socket-path boundary

- **Area**: fresh C++ requester/Provider conversation process validation.
- **First boundary**: NFD exited before creating its Unix socket with
  `File name too long`; the retained run root made the generated socket path
  exceed the Unix-domain pathname limit. The driver was stopped while waiting
  for the socket (exit 130).
- **Interpretation**: fixture startup failure only. No Controller, Authority,
  grant, selection, stream, or conversation result is counted.
- **Corrective gate**: rerun unchanged binaries under a short `/tmp` run root,
  preserve the raw run and link this evidence before evaluating protocol
  behavior.
- **Evidence**:
  [`r11-b8-g6-native-process-fixture-startup-20260910.md`](../specs/182-native-di-python-bindings/evidence/r11-b8-g6-native-process-fixture-startup-20260910.md)

- **Corrective result**: the shared process fixture now selects a short
  per-process `/tmp` socket pathname whenever the retained run root is too
  deep. The corrected conversation/recovery and unary process runs reached
  their declared C++ protocol boundaries; see G7/G8 evidence.

## 2026-09-10 — Spec182 R11-B1 independent authority process fixture boundaries

- **Area**: C++ requester to independent `DI_NativeArtifactAuthority` grant process over a
  private NFD and Controller.
- **First boundaries**: the initial fixture shared one writable PIB/TPM among Controller,
  Authority and requester. Authority startup raced ndn-cxx identity/default-certificate state;
  Controller then reported a missing signing certificate and Authority could not fetch PUBPARAMS.
  After switching the requester namespace to read-only, its HOME was also read-only and the C++
  probe stopped at `Failed to acquire file lock`. A copied identity store then retained the
  bootstrap `tpm-file:` locator, so Authority reached permission fetch but could not decrypt the
  Controller response. Raw runs are retained under `/tmp/spec182-r11-b1-process-rpziurt1`,
  `/tmp/spec182-r11-b1-process-wqjaczju` and `/tmp/spec182-r11-b1-process-1c19cxaj`.
- **Interpretation**: these were process-fixture identity-storage and namespace boundaries;
  none was a grant protocol result. The Controller-only run passed before Authority was added.
- **Changed gate before retry**: generate NDN identities once, copy the full PIB/TPM per native
  role, rewrite each copied PIB's TPM locator, retain only that role's private NDN key files,
  keep requester HOME on tmpfs, and leave Authority grant private material outside requester
  mounts. Only the corrected fixture's C++ positive/negative markers count as R11-B1 evidence.
- **Final result**: the corrected independent process fixture passed one positive grant, five
  Authority-handler rejection cases (bad signature, wrong epoch, unknown recipient, malformed
  wire, expired grant) and one Authority-unreachable transport timeout. R11-B2 onward and T016
  remain open.

- **Related batch-end check**: one concurrent `Spec182*` unit-suite run returned rc=201 with a
  memory access violation in the pre-existing `Spec182V3Placement/PublicClientConversationCommitsSeededReceiptAndCheckpoint`
  fixture at `tests/unit-tests/di-native-v3-placement.t.cpp:1006`. The selector passed ten isolated
  reruns, and a fresh complete `Spec182*` rerun passed 256/256 cases and 7077/7077 assertions;
  raw output is retained at `.codex-tmp/spec182-r11-b1-spec182-unit-rerun.log`. This is retained as
  an intermittent full-suite fixture boundary, not counted as a process protocol failure or as a
  reason to alter the R11-B1 production path.

## 2026-09-10 — Spec182 R11-B8-G11 reservation-overflow guard compile boundary

- **Area**: native Provider co-location capacity accounting.
- **First boundary**: the first rebuild after adding checked cumulative reservation arithmetic
  failed in `NativeV3Placement.cpp`; a multi-statement block was introduced under an existing
  single-line `if` without braces, producing a misleading-indentation warning and a map key type
  compile error. No binary from this attempt was used for behavior claims.
- **Interpretation**: implementation compile failure only; no request, Provider, NDN, or
  qualification result was produced.
- **Correction before retry**: add explicit braces around the guarded reservation block, retain
  the failed build directory, then rebuild the same Waf target with the system-first `-j2` command.
- **Evidence**:
  [`r11-b8-g11-provider-colocation-20260910.md`](../specs/182-native-di-python-bindings/evidence/r11-b8-g11-provider-colocation-20260910.md)

## 2026-09-10 — Spec182 R11-B9-G3 independent process dependency boundary

- **Area**: fresh C++ requester/Provider cross-process conversation validation.
- **First boundary**: the Python driver failed while importing the current `_ndnsf`
  extension because the environment selected `/usr/local/lib/libnac-abe.so`, which lacks
  `ndn::nacabe::Consumer::clearCache(...)` required by the build. The run returned `rc=1`
  before Controller, Authority, Provider, requester, or NFD protocol startup.
- **Interpretation**: dynamic-loader dependency mismatch only; no protocol result is counted.
  Raw command, output, return code, and `ldd` capture are retained under
  `.codex-tmp/spec182-r11-b9-cross-process-current-20260910-r0/`.
- **Changed gate before retry**: put the matching
  `/home/tianxing/NDN/nac-abe-integration-182/install/lib` and
  `/home/tianxing/NDN/ndn-svs/build` prefixes first in `LD_LIBRARY_PATH`; retain
  `/usr/local/lib` only after them for remaining ndn-cxx/ndnsd dependencies.
- **Evidence**:
[`r11-b9-g3-cross-process-dependency-boundary-20260910.md`](../specs/182-native-di-python-bindings/evidence/r11-b9-g3-cross-process-dependency-boundary-20260910.md)

## 2026-09-12 — Spec186 baseline model inventory search timeout

- **Area**: Spec186 Qwen3-0.6B and external model inventory.
- **First boundary**: an initial broad `find` over `/home/tianxing`, `/mnt`,
  `/data` and `/project` exceeded the command timeout before producing a
  complete result. No experiment, model qualification or source mutation was
  inferred from that command.
- **Root cause**: unbounded traversal of large historical checkouts and result
  trees on a memory-constrained host.
- **Correction**: repeat only against targeted NDNSF roots with a bounded
  depth; record explicit hashes for the two YOLO ONNX inputs and the unrelated
  Qwen2.5 GGUF, then mark Qwen3-0.6B as `WAITING_EXTERNAL_INPUT`.
- **Lesson**: model discovery must be path-scoped and digest-bound before a
  candidate is prepared; an available Qwen artifact with a different family,
  size or backend cannot substitute for Qwen3-0.6B.

## 2026-09-12 — Spec186 native build dependency and optional-fixture boundaries

- **Area**: T006 native runtime build on the exact Spec186 baseline.
- **First boundaries**: the first `./waf build -j2` stopped during Waf graph
  construction because an optional Spec182 worker fixture was absent and
  `find_node()` returned `None`. After guarding that optional target, the build
  reached `ServiceUser.cpp` and stopped because the selected NAC-ABE headers
  lacked `getPublicParamsDataName`, `getPublicParamsDigest`, `clearCache`,
  `refreshPublicParameters` and `refreshDecryptionKey`.
- **Interpretation**: no binary from this attempt is a Spec186 runtime
  candidate. The second failure is a dependency/header ABI mismatch, not a
  YOLO or Qwen protocol result.
- **Correction**: keep the optional fixture guard, reconfigure against the
  matching `/home/tianxing/NDN/NAC-ABE` source/build prefix, and rebuild with
  at most `-j2`. Capture the complete dependency and loader closure before
  using any resulting binary.
- **Lesson**: a successful Waf configure or pre-existing build output cannot
  establish candidate readiness; all consumers must compile against the same
  NAC-ABE API and packaged runtime identity.

## 2026-09-12 — Spec186 ONNX toolchain and SIF hash boundaries

- **Area**: T006 clean dependency and runtime closure.
- **First boundaries**: installing the matching NAC-ABE build into
  `/tmp/spec186-nacabe-install` copied headers and the library but could not
  update the old root-owned `build/install_manifest.txt` (permission denied).
  Reconfiguration then stopped because no ONNX 1.17 full-protobuf prefix with
  `checker.h`, `libonnx.a` and `libonnx_proto.a` exists on this host. Hashing a
  3.9-GB cached SIF also exceeded the first 30-second command window; the
  bounded retry completed with SHA-256
  `2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`.
- **Interpretation**: temporary install metadata and hash timing are tooling
  boundaries; missing ONNX inputs and the cached SIF's unproven source seal
  are candidate blockers. No artifact is promoted.
- **Correction**: retain the copied temporary prefix, request/provide a real
  ONNX 1.17 full-protobuf prefix, and reseal any SIF against the exact Spec186
  source and dependency tuple before rebuilding.
- **Lesson**: dependency headers, libraries, RPATH and SIF bytes must be
  verified together; an old lock digest or a partial install cannot close ABI.

## 2026-09-12 — Spec186 ONNX source-build and pinned Rust boundary

- **Area**: T006 clean Waf dependency closure.
- **First boundaries**: two `pip download --no-binary=:all: onnx==1.17.0`
  attempts entered PEP 517 build-dependency resolution and were terminated
  after consuming the bounded local build window. A direct `protoc 3.6.1`
  generation initially used both `onnx.proto` and `onnx-ml.proto`, producing
  duplicate definitions. The corrected temporary probe generated all three
  protobuf translation units and built `libonnx_proto.a`/`libonnx.a` under
  `/tmp`, using compatibility helpers for the host's older protoc API.
- **Next boundary**: Waf configuration with those temporary ONNX archives and
  the matching NAC-ABE prefix reached the pinned tokenizer step, then stopped
  because `.codex-tmp/spec182-t001-dependencies/rust-prefix/bin/cargo` and its
  offline cargo home are absent.
- **Interpretation**: the temporary ONNX probe is not a packaged dependency
  seal; no product binary or runtime candidate is promoted. The remaining
  blocker is the missing locked Rust toolchain, followed by a clean Waf build
  and loader closure.
- **Lesson**: do not replace a locked compiler/toolchain input with an ad-hoc
  stub or host package; preserve the first missing dependency and resume from
  the same source/dependency tuple when it is supplied.

## 2026-09-12 — Spec186 NAC-ABE header/library ABI mismatch

- **Area**: T006 native loader closure after dependency discovery.
- **First boundary**: forcing the temporary `/tmp/spec186-nacabe-install`
  library ahead of `/usr/local/lib` still failed `_ndnsf.so` import at
  `ndn::nacabe::Consumer::clearCache(...)`; `ldd -r` also listed the related
  refresh/public-parameter and policy-rotation symbols as unresolved.
- **Evidence**: `nm -D --defined-only` on the temporary library found no
  definitions for the methods declared by its installed headers. Waf's
  existence-only NAC-ABE check therefore cannot establish ABI compatibility.
- **Interpretation**: the temporary prefix copied a library built from an older
  NAC-ABE revision. No binary, import or runtime result from this probe is a
  candidate. The dependency library must be rebuilt from the same source/API
  revision as the headers, then the full Waf and loader gates rerun.
- **Lesson**: verify exported symbols and runtime resolution, not only header
  presence, library path or successful configure output.

## 2026-09-12 — Spec186 pinned Cargo cache boundary

- **Area**: T006 clean Waf dependency closure after restoring the locked Rust
  toolchain.
- **First boundary**: `./waf configure --out=build-spec186
  --nac-abe-prefix=.deps/nac-abe-spec179-official
  --onnx-prefix=/tmp/spec186-onnx-prefix2 --disable-local-dependency-prefix`
  reached the required tokenizer bridge and failed in Cargo offline mode:
  `no matching package named tokenizers found`.
- **Interpretation**: Rust 1.90.0 itself is restored and verified, but the
  isolated Cargo home has no registry/cache for the locked `Cargo.lock`; no
  tokenizer library or product binary exists from this attempt.
- **Correction**: populate the isolated Cargo cache using the repository lock
  and then rerun the exact `--locked --offline` build. Do not substitute a
  host toolchain, unpinned crate, or stub implementation.
- **Lesson**: a verified compiler is only one part of a reproducible Rust
  input; the lockfile, crate cache and offline resolution must also be sealed.

## 2026-09-12 — Spec186 NDN-SVS installed-version mismatch

- **Area**: T006 native source compilation after the Rust and ONNX gates.
- **First boundary**: the initial targeted build stopped at
  `ServiceProvider.cpp:6811` because the `/usr/local` NDN-SVS headers exposed
  no `SVSPubSub::subscribeToProducerWithCatchUp` member.
- **Evidence**: the source/build pair at `/home/tianxing/NDN/ndn-svs` declares
  and exports that method, while `/usr/local/lib/libndn-svs.so` exports only
  the older `subscribeToProducer` API. Waf's explicit source/build closure
  check passes when that pair is selected.
- **Correction**: reconfigure with
  `--ndn-svs-source-tree=/home/tianxing/NDN/ndn-svs` and
  `--ndn-svs-build-tree=/home/tianxing/NDN/ndn-svs/build`; the subsequent
  targeted build reached 24/97 without that error.
- **Lesson**: pkg-config success and SONAME equality do not prove NDN-SVS API
  compatibility; source headers, link input and runtime path must be fixed as
  one tuple.

## 2026-09-12 — Spec186 GCC assembler crash during ONNX adapter compile

- **Area**: T006 targeted native build after matching NDN-SVS selection.
- **First boundary**: `./waf build --out=build-spec186
  --targets=ndn-service-framework,ndnsf-distributed-inference -j4` reached
  38/97, then GCC 9's assembler reported
  `/tmp/ccSZgVDV.s: Internal error (Segmentation fault)` while compiling
  `NativeOnnxRecipeAssembler.cpp`.
- **Interpretation**: this is a compiler/assembler or resource boundary, not
  a source diagnostic or runtime qualification result. The build produced no
  complete candidate library.
- **Correction**: retain all successful objects and rerun the same target set
  at `-j2` to test whether the crash is concurrency-sensitive; if it repeats,
  isolate the source with a single-job compile and record the compiler input.
- **Lesson**: a partial object graph and successful dependency checks do not
  establish a native candidate; toolchain crashes require an independent
  reproducible compile before any ABI evidence is accepted.

## 2026-09-12 — Spec186 GCC 9 internal compiler error in provider target

- **Area**: T006 provider executable compilation with the matched dependency
  tuple.
- **First boundary**: after the `-j4` assembler crash was avoided, the
  provider target retried at `-j2` and then `CXXFLAGS='-O0 -g0'`; both reached
  `NativeCanonicalArtifactPublisher.cpp` and GCC 9 terminated with
  `internal compiler error: in ggc_set_mark, at ggc-page.c:1547`.
- **Interpretation**: the source has no ordinary diagnostic, but the locked
  host GCC cannot produce this provider object reliably. No provider binary
  or runtime candidate is promoted.
- **Correction**: retain the successful framework/DI libraries and test the
  system `/usr/bin/clang++` 10 compiler under an explicit `/usr` toolchain
  root. This is a build-toolchain experiment; the resulting compiler identity
  must be recorded in the candidate seal.
- **Lesson**: lowering optimization and parallelism do not guarantee a
  compiler workaround; keep the failing compiler input and verify any
  alternate toolchain with the same ABI/RPATH gates.

## 2026-09-12 — Spec186 Tiger compute Apptainer subcommand hang

- **Area**: T009.a compute-node capability preflight.
- **First boundary**: a bounded `srun` allocation on `itiger05` returned the
  GPU identity and `apptainer --version` (`1.5.3-1.el9`), but
  `/usr/bin/apptainer version` did not return within five seconds. The login
  node reports `1.3.4-1.el9`, so the two nodes cannot be treated as one runtime.
- **Correction**: use the compute-node `--version` probe in the next job and
  keep the `version` hang in the receipt; do not enter an unbounded readiness
  loop or validate a SIF only on the login node.
- **Lesson**: every container capability used for qualification must be
  probed on the allocated compute node with a bounded command; login-node
  version output is not execution evidence.

## 2026-09-12 — Spec186 local/compute Apptainer release mismatch

- **Area**: T006/T009 release compatibility gate.
- **First boundary**: all sampled `bigTiger` GPU nodes (`itiger01`, `itiger02`
  and `itiger07`) report Apptainer `1.5.3-1.el9`, while the experiment host
  provides Apptainer `1.3.4`. The release workflow requires the local builder
  and allocated compute runtime to match semantically before a SIF is
  promoted.
- **Correction**: record the mismatch as a hard pre-dispatch boundary and
  repair the compute probe to use bounded `apptainer --version`; do not build
  or execute a Spec186 SIF with the cached 1.3.4/1.5.3 pair.
- **Lesson**: a successful GPU allocation does not make an image portable;
  Apptainer version parity is a prerequisite for exact SIF composition.

## 2026-09-12 — Spec186 standalone reference receipt lost by cleanenv

- **Area**: standalone YOLO GPU substrate reference.
- **First boundary**: the first `itiger02` ORT CUDA run completed model
  execution, then the in-container receipt writer raised `KeyError: 'SIF'`
  because `apptainer exec --cleanenv` removed the host environment variables
  used only for metadata.
- **Correction**: preserve the failed run identity and rerun in a new
  `spec186-standalone-yolov8n-r2` directory using fixed container paths and
  explicit ORT thread limits. The new receipt records 350 CUDA profile events,
  a 4.262384 ms measured request and the raw `[1,84,8400]` output hash.
- **Lesson**: metadata needed after a clean-container boundary must be passed
  as sealed inputs or container paths; a successful kernel execution is not a
  durable experiment receipt until the writer completes.

## 2026-09-12 — Spec186 local Apptainer 1.5.3 full-target timeout

- **Area**: local runtime release switch before T009/T010.
- **First boundary**: after configuring Apptainer v1.5.3 for `/usr/local`, the
  bounded `make -C builddir -j4` generated the main binary and then exceeded
  its 300-second bound in the bash-completion Go generator. A direct list of
  CNI output paths also failed because the Makefile exposes the aggregate
  `cniplugins` target rather than individual build rules.
- **Correction**: run the support targets separately (`cniplugins`, starter
  and offsetpreload), generate the prefix-specific config, and install the
  complete non-SUID runtime files explicitly. Version/help probes then passed
  with `/usr/local/bin/apptainer` 1.5.3.
- **Lesson**: a successful main binary does not imply a complete Apptainer
  installation; install and probe starter, CNI, config and runtime paths
  separately, with bounded generators and no version fallback.

## 2026-09-12 — Spec186 provider help repair cannot reuse stale build tree

- **Area**: T005/T006 native provider entrypoint closure.
- **First boundary**: after adding the required zero-status `--help` parser
  branch, a direct clang translation-unit compile passed, but relinking from
  the retained `build/` objects failed. The objects mix an older Core/DI ABI:
  the provider expects newer `NativeInferenceProvider`, tokenizer-decoder and
  admission symbols, while the retained framework and DI objects expose older
  names; their NDN-SVS/NAC-ABE closure is also incomplete.
- **Correction**: reject the stale tree as a candidate and keep the source
  repair explicit. Recreate the locked Rust tokenizer bridge, ONNX prefix and
  same-revision Core/DI/NDN-SVS/NAC-ABE build before producing a new provider
  digest. No ad-hoc relink or unresolved-symbol binary is promoted.
- **Lesson**: a small CLI fix still changes the candidate source identity;
  compile success or an old executable cannot replace a clean full-closure
  rebuild and runtime loader probe.

## 2026-09-13 — Spec186 closed-toolchain resolver selected Linuxbrew `ld`

- **Area**: T006 clean Waf configuration.
- **First boundary**: the first reconfiguration saw Linuxbrew's `ld` through
  the ambient `PATH`; the closed-toolchain resolver rejected it before creating
  a usable build graph.
- **Correction**: pin the build environment to the selected `/usr` toolchain
  (`PATH=$RUST_PREFIX/bin:/usr/bin:/bin`, `--toolchain-root=/usr`) and record
  the compiler/linker identity in the native build manifest. No source or
  dependency change was made.
- **Lesson**: a clean build tree is insufficient when the ambient linker can
  change; candidate closure must pin the complete compiler and linker path.

## 2026-09-13 — Spec186 native probe selected an ambient incompatible NAC-ABE

- **Area**: T006 Python extension/runtime identity probe.
- **First boundary**: the official native-build script was run without the
  exact dependency `LD_LIBRARY_PATH`; the loader selected `/usr/local/lib` and
  failed on `ndn::nacabe::Consumer::clearCache(...)` even though the matching
  `.deps/nac-abe-spec179-official/lib/libnac-abe.so` exports it.
- **Correction**: rerun the canonical import, runtime probe and identity
  verifier with the matching NAC-ABE, NDN-SVS, ONNX Runtime, ONNX and system
  directories in a fixed order. `SPEC180_NATIVE_IDENTITY_OK` and canonical
  import then passed.
- **Lesson**: `LD_LIBRARY_PATH` is part of the native candidate identity;
  success under an ambient loader path is not portable runtime evidence.

## 2026-09-13 — Spec186 staged app bundle rejected by old runtime SIF

- **Area**: T006.c/T009 exact base-plus-app composition boundary.
- **First boundary**: the content-addressed Spec186 app bundle staged to
  `/project/tma1/ndnsf-di/apps/spec186/spec186-app-bundle-r4` recomputed to the
  expected digest, but probing it with the existing
  `spec180-runtime-b6710fd6/spec180-runtime.sif` on `itiger04` failed before
  entrypoint startup: `libboost_system.so.1.71.0` was not found and the job
  exited `127`.
- **Correction**: retain the staging receipt and reject the old SIF as a
  Spec186 candidate. Build or provide an exact source-sealed Apptainer 1.5.3
  base SIF, then repeat the composition and loader checks inside that image.
- **Lesson**: matching an app directory digest and a container runtime version
  does not establish ABI compatibility; the base SIF identity and loader
  closure must be verified together before any Tiger submission.

The compute-node `apptainer inspect --json` independently labels that SIF as
`spec174-local-candidate-r24-b0bbca30` with source seal
`sha256:9766e37fcedd176a4316e795142db3106287b85cb0102f367b267d136f4d0127`,
which differs from the Spec186 handoff seal
`sha256:597c44a97b34655dfb67b9fc3bff3693b844f5cc1f10624870554bdee8e658e2`.

## 2026-09-13 — Spec186 source handoff required clean dependency worktrees

- **Area**: T006.c source-sealed 1.5.3 build inputs.
- **First boundary**: preparing a handoff from the current experiment checkout
  first failed because the requested provider revision was written with an
  incorrect full hash; the corrected detached worktree then exposed an
  ignored generated NAC-ABE example certificate that was not tracked by its
  pinned commit.
- **Correction**: use the exact commit
  `4751148375dad9149c7c185c9381d5734c733e13` and clean detached dependency
  worktrees. The source sealer and relocatable handoff now verify with seal
  `sha256:597c44a97b34655dfb67b9fc3bff3693b844f5cc1f10624870554bdee8e658e2`.
- **Lesson**: ignored generated files can change a source archive even when
  `git status` looks clean; source sealing must run from clean commit-pinned
  worktrees and record the exact source identity.

## 2026-09-13 — Spec186 profiles carried a stale collector identity

- **Area**: T005/T006 pre-dispatch candidate identity.
- **First boundary**: all eight profiles referenced collector digest
  `a7cf3577e0ac7256e3554c6facca63b740db9cba935cd9f2ef74c49a020fdf7b`, while
  the directory-aware bundle and pre-dispatch implementation had digest
  `b9bb5f886fc2ee39ab50f7d85814974c595d866ef1962164968165dad9697291`.
  Pre-dispatch therefore rejected the profiles before reaching their missing
  SIF/model checks.
- **Correction**: update both collector fields in every Spec186 profile and
  rerun JSON validation plus the full 74-test TigerCluster suite.
- **Lesson**: a collector change invalidates every candidate profile; update
  all producer/consumer identity fields in one checkpoint before diagnosing
  downstream runtime blockers.

## 2026-09-13 — Spec186 profiles carried stale source and app paths

- **Area**: T005/T006/T007/T009 candidate tuple binding.
- **First boundary**: after the collector repair, profiles still named the old
  build-tree provider path, old application digest and the ancestor-only source
  identity. Local app entrypoint checks therefore failed even though the
  content-addressed bundle had been built and staged to Tiger storage.
- **Correction**: bind all eight profiles to source commit
  `4751148375dad9149c7c185c9381d5734c733e13`, source seal
  `597c44a97b34655dfb67b9fc3bff3693b844f5cc1f10624870554bdee8e658e2`, bundle
  digest `badf6a0afb36e43d02f7103cba36383bf8f0336e2310a0fd546c33223734734d`,
  and the correct `bin/di-native-provider` entrypoint. Regenerated receipts
  now reach only the expected base/model/visibility boundaries with zero side
  effects.
- **Lesson**: source, bundle directory, entrypoint and digest form one
  candidate plane; updating one field without the others creates a false
  runtime blocker.

## 2026-09-13 — Spec186 profile entrypoint lost its binary suffix

- **Area**: T005/T006 native entrypoint pre-dispatch.
- **First boundary**: the first source/app path replacement changed each
  profile's `entrypoint` to the bundle directory itself, so native checks
  reported `NATIVE_ENTRYPOINT_MISSING` despite the staged
  `bin/di-native-provider` file being present.
- **Correction**: restore the explicit `bin/di-native-provider` suffix for all
  local and Tiger profiles and regenerate candidate digests. Local YOLO
  pre-dispatch now reaches only the missing base SIF and native-library
  boundary; Tiger paths remain intentionally invisible from this host.
- **Lesson**: directory-aware bundles still require an explicit executable
  entrypoint; a valid directory digest cannot stand in for executable identity.

## 2026-09-13 — Spec186 host M01 could not use the staged application extension

- **Area**: T006.b/T006.c current-source host/CPU qualification prerequisite.
- **First boundary**: a real M01 launch with the staged Python/native bundle
  failed before MiniNDN startup because the host loader selected the older
  `/usr/local/lib/libndn-service-framework.so.0.1.0`; the staged
  `libndnsf-distributed-inference.so` requires newer framework symbols such as
  `ServiceRegistration::closed`.
- **Correction**: keep the M01 output as a failed attempt, reject it as a
  host-gate result, and retain the successful in-container provider `--help`
  and `ldd -r` evidence from the temporary Apptainer 1.5.3 ORT-1.26 base.
  Rebuilding a current host/CPU gate requires a matching framework/dependency
  prefix and the missing ONNX full-protobuf development prefix; neither may be
  fabricated from the old host install.
- **Lesson**: an application bundle that loads inside a container is not a
  host qualification artifact; the host M01 manifest must be produced by a
  real run with an ABI-matched framework and dependency closure.

## 2026-09-13 — Spec186 host framework rebuild stopped at missing ONNX headers

- **Area**: T006.c exact source-sealed SIF build prerequisite.
- **First boundary**: configuring a fresh current-source host build with the
  pinned NAC-ABE and NDN-SVS prefixes stopped because the previously used
  `/tmp/spec186-onnx-prefix2` no longer exists and `onnx/checker.h` is absent
  from the available ORT-1.26 SDK.
- **Correction**: do not substitute the runtime-only ORT SDK or the old
  installed framework. Record the missing full-protobuf ONNX prefix as an
  external build input and leave the canonical SIF build gate closed until it
  is supplied and the current host M01 is rerun.
- **Lesson**: source sealing fixes repository identity, but it cannot replace
  a missing development dependency required to compile the ABI consumers.

## 2026-09-13 — Spec186 Python binding mixed NAC-ABE ABIs

- **Area**: T006.b native extension/runtime closure and host M01 startup.
- **First boundary**: the Waf Core/DI build selected the explicit
  `.deps/nac-abe-spec179-official` NAC-ABE prefix, but the separate setuptools
  binding process did not receive that prefix and compiled against the older
  `/usr/local/include` headers. AddressSanitizer then reported a
  `ServiceUser` allocation of 12864 bytes followed by deletion using the
  13400-byte type, producing `new-delete-type-mismatch`/`double free` during
  `NativeServiceUser` teardown.
- **Correction**: make `spec180_native_build.py` read and validate
  `NDNSF_NAC_ABE_PREFIX` from Waf's `c4che/_cache.py`, pass it to setup.py, and
  force-rebuild the binding. The resulting compile command uses the explicit
  NAC-ABE include directory; the native identity verify and
  `NativeServiceUser` construct/stop/delete regression both return zero.
- **Lesson**: a clean `ldd` closure cannot detect C++ class-layout drift across
  a Waf/setuptools boundary; every explicit dependency prefix must be forwarded
  to each compiler child and the lifecycle must be exercised after rebuilding.

## 2026-09-13 — Spec186 host M01 remained below the repository route gate

- **Area**: T007 MiniNDN host-gate retry after the NAC-ABE ABI repair.
- **First boundary**: the first retry without the root owner context stopped at
  MiniNet's `*** Mininet must run as root` check. A second retry with the
  passwordless root owner completed topology/NFD/controller startup, but the
  repository publisher's three bounded `/STATUS` probes all timed out and the
  run stopped at `REPO_SERVICE_ROUTE_NOT_READY` before the application request.
- **Correction**: preserve both run directories (`r10` and `r12`) as failed
  attempts; do not promote them to MiniNDN or YOLO qualification. Keep the
  corrected native extension and r5 bundle as the next candidate, and rerun
  only after the repository route/readiness barrier is repaired or its intended
  host prerequisite is supplied.
- **Lesson**: a valid native import and a successful NFD/controller start do
  not prove the repository service route; the first failed readiness probe is
  the controlling boundary for this host campaign.

## 2026-09-13 — Spec186 r5 staging hit the Tiger project quota

- **Area**: T006.c application-layer staging.
- **First boundary**: streaming the unstripped r5 application bundle stopped
  with `tar: .../di-native-provider: Cannot write: Disk quota exceeded` after
  approximately 100 MiB had arrived at the new remote directory.
- **Correction**: remove only the incomplete newly created r5 directory,
  rebuild the delivery copy by stripping ELF debug sections while preserving
  dynamic symbols, recompute the bundle manifest, and restage the 69,116,035
  byte read-only bundle. Local and remote tree digests now both equal
  `687610de859155449c51ec2ba4bb7b57c77614cbf0a53f106bb65152f8c07129`.
- **Lesson**: application delivery must account for project quota; stripping
  non-runtime debug sections is safe only when the post-strip import/help and
  loader closure are rerun and the new content digest is bound everywhere.

## 2026-09-13 — Spec186 profile runtime and r5 identity drift

- **Area**: T003/T006 candidate closure after the Apptainer policy change.
- **First boundary**: the profiles named the r5 directory but still carried its
  earlier `c4503887...` tree digest; after adding the explicit runtime pin, the
  collector source hash was also stale. These mismatches would reject a fresh
  candidate before any remote action and could hide which Apptainer binary was
  selected.
- **Correction**: bind the measured r5 digest
  `687610de859155449c51ec2ba4bb7b57c77614cbf0a53f106bb65152f8c07129` and the
  current collector digest in all eight profiles. Add explicit Apptainer path
  and version fields, use the declared path in the launcher, verify local
  1.5.3 with bounded `--version`, and reject non-1.5.3 compute preflight.
- **Lesson**: a runtime policy must be part of candidate identity; version
  prose and PATH lookup are insufficient for reproducible SIF execution.

## 2026-09-13 — Spec186 r39 M01 request did not reach a YOLO provider

- **Area**: T007 MiniNDN host-gate SVS delivery diagnosis.
- **First boundary**: with `NDNSF_SVS_MAX_PIGGYDATA_BYTES=4096`, the User
  published the 2,585-byte tiny-ONNX request and recorded `SVS_PUBLISH_DONE`,
  but all four Providers returned zero ACKs. Route snapshots and the small
  repository path were available; no Provider entered the YOLO request
  callback.
- **Correction**: add an opt-in, bounded `NDNSF_SVS_DIAGNOSTIC` trace at the
  Provider SVS boundary. It records missing sync ranges and distinguishes a
  delivered publication rejected by freshness from a publication that never
  reached `OnRequest`; the default protocol behavior and log volume remain
  unchanged.
- **Lesson**: increasing the piggy-data limit and observing a User publish
  event do not prove Provider delivery. The request publication/fetch path and
  the Provider freshness decision need independent evidence before changing
  the SIF or model candidate.

## 2026-09-13 — Spec186 r40 repository route probe failed on segmented signature validation

- **Area**: T007 host-gate diagnostic rerun.
- **First boundary**: the corrected diagnostic launch reached topology,
  Controller, and repository startup, but all three bounded `/STATUS` probes
  failed while resolving the repository's large-response reference with
  `Segment validation failed: Signature verification failed`; the harness
  stopped at `REPO_SERVICE_ROUTE_NOT_READY` before the YOLO request.
- **Correction**: preserve r40 as a failed evidence directory and keep the
  new SVS diagnostic disabled by default. Do not promote the route probe or
  infer a Tiger/SIF version fault from this run; repair the segmented response
  certificate/validator path and rerun the repository barrier first.
- **Lesson**: a route Interest can reach the Provider and still fail before
  the application gate. The terminal response reference must pass independent
  segment validation before repository readiness or YOLO qualification can be
  claimed.

## 2026-09-13 — Spec186 r41–r44 SVS delivery had no current Provider evidence

- **Area**: T007 MiniNDN host-gate delivery diagnosis.
- **First boundary**: repeated bounded host runs showed the User's SVS
  publication and route snapshots, but no current Provider request or native
  publication/hash evidence. The runs therefore could not distinguish an NFD
  route loss from an SVS freshness or repository fetch rejection.
- **Correction**: preserve each immutable run directory and add the opt-in
  `NDNSF_SVS_DIAGNOSTIC` boundary trace; do not increase SIF/model scope or
  promote User-side `SVS_PUBLISH_DONE` to protocol delivery.
- **Lesson**: a publication event on the producer is not evidence that a
  Provider accepted the publication. Both sides of the SVS boundary need
  timestamped evidence.

## 2026-09-13 — Spec186 r45–r47 host retries exposed transient barriers

- **Area**: T007 MiniNDN host-gate retries.
- **First boundary**: r45 stopped at a transient repository-route probe; r46
  reached the Provider after route recovery and exposed an asynchronous SVS
  unregister race; r47 stopped during bounded repository prefetch before the
  application request.
- **Correction**: retain all three run directories; repair the SVS lifecycle
  only after the r46 trace, and treat r45/r47 as startup/transient boundaries
  rather than model or SIF verdicts.
- **Lesson**: retries must preserve the first boundary and separate startup
  transients from deterministic application failures.

## 2026-09-13 — Spec186 r46 SVS re-registration removed the replacement group route

- **Area**: T005/T007 Provider SVS lifecycle.
- **First boundary**: after permission-triggered reinitialization, the old
  `RegisteredPrefixHandle` unregister command completed asynchronously after
  the replacement `/group` registration, deleting the new route and leaving
  Providers without current request delivery.
- **Correction**: clear the old registrations, wait the bounded
  `NDNSF_SVS_REINIT_UNREGISTER_SETTLE_MS` interval, then construct and register
  the replacement SVS publisher. The default settle is 100 ms and r53 used
  150 ms.
- **Lesson**: destroying an NFD registration handle is asynchronous; a
  replacement registration needs an explicit event-loop settle boundary.

## 2026-09-13 — Spec186 r48 compact streamed Selection omitted Provider grants

- **Area**: T005 streamed collaboration selection.
- **First boundary**: r48 reached all four Provider ACK callbacks and Selection,
  but every request-scoped Provider rejected with
  `request-scoped stream grant rejected`. The compact multi-Provider Selection
  had one grant field and could not represent a distinct wrapped event key per
  Provider.
- **Correction**: preserve r48 and change streamed multi-Provider Selection to
  publish one Provider-specific Selection per selected ACK, so each grant is
  wrapped for its actual Provider key offer.
- **Lesson**: a compact wire form is invalid when a security field is
  recipient-specific; selection fan-out must preserve recipient binding.

## 2026-09-13 — Spec186 r49 non-terminal collaboration roles still required grants

- **Area**: T005 streamed collaboration Provider execution.
- **First boundary**: after the per-Provider Selection split, r49 still failed
  because the request-scoped path initialized a stream publisher for every
  collaboration role. Only the terminal response owner receives the event-key
  grant; non-terminal roles have authenticated assignment payloads instead.
- **Correction**: preserve r49 and make publisher initialization conditional on
  a grant. Permit a missing grant only when the service is registered as a
  collaboration service and the assignment payload is nonempty; ordinary
  streamed requests remain fail-closed.
- **Lesson**: collaboration dependency stages and terminal stream ownership
  are different authorization roles and must not share one unconditional
  publisher precondition.

## 2026-09-13 — Spec186 r50 repository prefetch timed out before application start

- **Area**: T007 MiniNDN host-gate retry.
- **First boundary**: r50 exceeded the bounded 120-second repository fetch
  deadline during stage-3 startup and produced no application request logs.
- **Correction**: retain r50 as a startup failure; rerun with a fresh run ID and
  the already bounded retry/backoff settings. Do not attribute this boundary to
  the collaboration grant repair.
- **Lesson**: a pre-application repository timeout cannot validate or falsify
  the application protocol path.

## 2026-09-13 — Spec186 r51 confirmed terminal-only grant construction

- **Area**: T005 streamed collaboration diagnosis.
- **First boundary**: diagnostic r51 showed event-key grants were built only for
  Provider/3, while Provider/0–2 had authenticated requests but no grant; the
  unconditional request-scoped publisher initialization then rejected those
  non-terminal roles.
- **Correction**: use r51 as the controlling diagnosis, apply the registered
  collaboration-assignment exception, and verify the repair with a fresh r53
  run rather than rewriting the failed receipt.
- **Lesson**: log grant construction and grant acceptance separately; terminal
  ownership must be observable at both producer and consumer boundaries.

## 2026-09-13 — Spec186 r6 application staging exceeded the project quota

- **Area**: T006.c current-source application bundle delivery.
- **First boundary**: the first rsync of the new r6 bundle stopped with
  `Disk quota exceeded` while writing `libndnsf-distributed-inference.so`;
  only a partial remote directory was present and its manifest was unusable.
- **Correction**: remove the newly created partial r6 directory, remove the
  invalidated remote r5 staging after retaining the complete local r5 receipt,
  then stage the current r6 bundle. Local and remote tree recomputation now
  both equal `04c2dd64b4f070cbd909a87f75a0372a0e3d4dae45c7e712641369cf76531d73`.
- **Lesson**: source changes invalidate the old staged application; quota
  cleanup must happen before immutable replacement staging, never by
  overwriting a bundle whose digest is still referenced by a receipt.

## 2026-09-13 — Spec186 r6 profile source identity was malformed

- **Area**: T006.c/T009.a profile refresh and TigerCluster regression tests.
- **Symptom**: all eight refreshed profiles carried a 39-character
  `candidate.sourceCommit`, so strict loading stopped at
  `INVALID_COMMIT:candidate.sourceCommit`; eight tests failed before checking
  their intended Apptainer or scheduler behavior.
- **Root cause**: the current commit string was copied without its final `d`
  while updating the r6 source/app identity.
- **Correction**: replace the profile and evidence references with the exact
  40-character commit `6d143d3f0f7a7c627af2c1ef6810d79c0738b52d`, then rerun
  strict profile, JSON and Spec186 checks.
- **Lesson**: validate hash length and resolve it with `git rev-parse` before
  binding a source identity into candidate profiles or receipts.

## 2026-09-13 — Spec186 r55 first G3 invocation omitted campaign identity

- **Area**: T006.c host-gate evidence generation.
- **First boundary**: the first current-source M01 run completed the real
  four-Provider tiny-ONNX path, but its `spec175-case-result.json` carried an
  empty `campaignId` because the bounded invocation did not pass
  `--campaign-id`; the result could not be accepted by the canonical G3
  manifest generator.
- **Correction**: preserve r54 as a diagnostic receipt and rerun once with
  the explicit campaign `spec175-M01-1750001`. r55 then generated and passed
  the canonical host manifest and validator.
- **Lesson**: a successful application trace is not a reproducible campaign
  receipt until seed, campaign, source seal, topology and result identity are
  all present.

## 2026-09-13 — Spec186 r6 application does not compose with the historical base SIF

- **Area**: T006.c exact base-plus-application composition.
- **First boundary**: mounting the current r6 application bundle into the
  cached Apptainer 1.5.3 base SIF failed `ldd -r`: the historical base lacks
  the required `ServiceRegistration`/`ServiceUser` symbols and exports an
  older ONNX Runtime ABI, so `libndnsf-distributed-inference.so` requires
  `VERS_1.26.0` and `OrtGetApiBase` that the cached runtime does not provide.
  The provider `--help` therefore exited nonzero before application startup.
- **Correction**: reject the cached SIF as a Spec186 candidate; keep r55 as
  host-only evidence and do not claim a layered runtime. Build or obtain a
  source-sealed 1.5.3 base carrying the matching Core/ONNX ABI, then rerun the
  full loader matrix and exact composition receipt with the r6 read-only app.
- **Lesson**: Apptainer version alignment alone does not establish runtime
  compatibility; exact library SONAME, symbol and ONNX Runtime version
  closure must be checked inside the selected SIF before Tiger submission.

## 2026-09-13 — Spec186 complete-builder dependency and base-image boundaries

- **Area**: T006.b/T006.c local source-sealed SIF construction.
- **First boundaries**: the first complete-builder attempt stopped because
  Ubuntu focal has no `python3.10-dev` package in the configured repositories;
  the next attempt stopped because the historical ORT SDK exposed only
  `libonnxruntime.pc`, then NDNSF stopped on the missing official ONNX
  1.17 full-protobuf prefix and missing pinned Rust 1.90/Cargo input. After
  those inputs were made explicit, `OnnxRuntimeModelRunner.cpp` exposed that
  the ORT pc metadata pointed to `/usr/local` while the SDK headers and
  library were under `/opt/onnxruntime`. Two parallel GCC 9.4 builds then
  independently hit internal compiler errors in `ExecutionLease.cpp` and
  `PolicyStatus.cpp`.
- **Correction**: use the bundled Python 3.10 headers, require ONNX and Rust
  inputs inside the base SIF, normalize ORT metadata to `/opt/onnxruntime`,
  and serialize the largest NDNSF Waf target (`-j1`) while keeping smaller
  dependency builds at `-j2`.
- **Base-image finding**: the cached base SIF itself contained unreadable
  gzip blocks. A readable rootfs was re-packed and verified end-to-end;
  zstd re-packing exposed a corrupt 510 MiB optional Triton `libtriton.so`,
  so the provisional YOLO/ORT-only base excludes that component and cannot
  yet be called a complete Qwen runtime.
- **Lesson**: a successful `apptainer exec /bin/true`, pkg-config probe, or
  component build does not prove the full input closure. Every candidate must
  bind source seal, dependency revisions, compiler/Rust/ONNX/ORT identities,
  immutable base digest, and a complete SIF extraction plus loader matrix.

## 2026-09-13 — Spec186 effective launcher rendered an unusable process boundary

- **Area**: T004/T005 `Experiments/TigerCluster/jobs/spec184/` static wiring.
- **Symptom**: `render_effective` wrapped the host MiniNDN runner in
  `apptainer exec --cleanenv --containall`, passed a host Python and host
  launcher path that do not exist inside the image, omitted model/input/
  identity binds from argv, and sent the YOLO runner an unsupported
  `--run-id/--candidate-digest` pair. `local_run` also discarded the rendered
  environment, while Qwen used the weight path where its runner requires the
  stage manifest.
- **Root cause**: the generic lifecycle renderer treated host orchestration and
  in-image execution as one command shape and never made the effective env a
  child-process input. The profiles also identify YOLOv8n while the maintained
  Spec180 runner requires a YOLO26n canonical package plus eleven undeclared
  environment inputs.
- **Correction**: render MiniNDN as a host command with the exact-SIF command
  provider variables; map split YOLO profiles to Y-B/Y-N; use Qwen stage
  manifests; propagate a closed environment through local and Slurm execution;
  and reject Tiger rendering before scheduler mutation until a dedicated
  workload document, canonical inputs, and per-role argument files are
  declared. This avoids importing unavailable MiniNDN from inside the SIF or
  calling the existing Spec180 supervisor with the incompatible Spec186
  candidate schema. Pre-dispatch reports explicit harness-family/environment
  diagnostics until the missing canonical inputs are declared.
- **Lesson**: a static argv string is not an executable contract. Validate the
  interpreter namespace, CLI case, bind application, environment propagation,
  and profile-to-runner model contract before allowing scheduler mutation.

## 2026-09-13 — Spec186 terminal collector accepted marker-only PASS

- **Area**: T004/T005 `runtime/spec186_candidate.py` terminal collection and
  `jobs/spec184/run.sbatch` scheduler boundary.
- **Symptom**: a receipt containing only `candidateDigest`, `status=PASS`,
  `exitCode=0` and `cleanup.reaped=true` was promoted to `PASS`; malformed
  scheduler argv produced a Python traceback, and an existing run root could
  be reused. The latter two behaviors also allowed unstable or stale run
  boundaries.
- **Root cause**: terminal validation checked identity and process cleanup but
  did not require protocol completion, numerical oracle, observed role/backend,
  or process-exit evidence. The shell adapter parsed argv after creating the
  run directory and used `exist_ok=True` for evidence/state paths.
- **Correction**: require candidate-bound `runId`, protocol, numerical, role,
  process and cleanup evidence; validate scheduler config/argv/environment
  before creating an owned run root; reject root reuse and return stable error
  codes; create state/security/home directories for both local and scheduler
  paths; and sanitize local child environments against host ABI/Python path
  leakage.
- **Lesson**: a terminal marker and a reaped process are necessary but not
  sufficient for qualification. Collector and scheduler boundaries must reject
  incomplete evidence and stale run roots before any execution side effect.

## 2026-09-13 — Spec186 profile fields could drift from executable contracts

- **Area**: T003/T004/T005 profile validation and local/Tiger dispatch.
- **Symptom**: role GPU assignments were accepted with the wrong backend or
  service name, duplicate NFD endpoint hosts were accepted, Tiger profiles
  could carry `REPLACE_ME` allocation/GPU values into a rendered candidate,
  and the Qwen profile declared GGUF/llama.cpp while its maintained entrypoint
  only accepts ONNX/onnxruntime inputs. A caller could also pass an arbitrary
  executable to `local_run` and receive a qualification `PASS` on exit 0.
- **Root cause**: validation covered field presence and top-level types but did
  not bind role semantics to topology/model contracts, distinguish reusable
  templates from scheduler-ready values, or distinguish diagnostic command
  overrides from the declared launcher.
- **Correction**: bind role service/backend/GPU policy and Qwen stage
  dependencies, reject duplicate endpoint hosts, report allocation/GPU
  placeholders before dispatch, detect Qwen harness format/input drift, add
  Slurm memory/GPU resource flags, reserve run roots with mode 0700, and mark
  command overrides `UNQUALIFIED` even when they exit successfully.
- **Lesson**: a schema that parses is not an executable contract. Every value
  that affects placement, model format, identity or qualification status must
  be consumed or rejected before process or scheduler mutation.

## 2026-09-13 — Spec186 scheduler child escaped its run-owned working directory

- **Area**: T004/T005 `Experiments/TigerCluster/jobs/spec184/run.sbatch`.
- **Symptom**: the scheduler payload created `runRoot/home`, but a child using a
  relative evidence path wrote into the submission working directory; `HOME`
  also pointed at a fixed `/tmp/spec186-home` shared by concurrent jobs.
- **Root cause**: the payload sanitized the child environment without changing
  directory after run-root creation and used a global temporary home path.
- **Correction**: change directory into the reserved run root before `execvpe`
  and set `HOME` to that run's private `home/` child; add a regression that
  checks both the working directory and home value.
- **Lesson**: run-root ownership must cover cwd and HOME as well as directory
  creation, otherwise relative evidence and NDN identity state can cross run
  boundaries even when the process is reaped.

## 2026-09-13 — Spec186 nested manifest mutation exposed a validator name error

- **Area**: T005 `runtime/spec186_candidate.py` candidate manifest shape gate.
- **Symptom**: the first focused run after adding strict nested manifest-field
  checks failed nine cases with `NameError: _ASSET_MANIFEST_KEYS` instead of
  returning deterministic rejection receipts.
- **Root cause**: the new validator referred to a non-existent constant; the
  profile asset schema is named `_ASSET_KEYS`.
- **Correction**: use the existing asset-key set, retain the nested unknown and
  missing-field checks, and add a mutation regression that preserves the
  candidate digest while adding an unexpected nested field.
- **Lesson**: new fail-closed branches need an immediate focused run before
  updating candidate seals or evidence counts; an exception at a rejection
  boundary is itself an executable-contract defect.

## 2026-09-13 — Spec186 manifest asset schema omitted its label field

- **Area**: T005 `runtime/spec186_candidate.py` nested candidate manifest gate.
- **Symptom**: after fixing the validator name, the new mutation regression
  still did not report the injected asset field because manifest assets carry
  the generated `label` alongside `path` and `sha256`.
- **Root cause**: the first repair reused the profile asset-key set, which is
  intentionally smaller than the candidate-manifest asset schema.
- **Correction**: add the explicit three-field manifest asset schema and retain
  the nested unknown/missing checks; refresh profile collector seals after the
  source change.
- **Lesson**: profile and manifest representations are separate contracts;
  validation must name each wire shape instead of assuming their key sets are
  interchangeable.

## 2026-09-13 — Spec186 scheduler environment could drift from run identity

- **Area**: T004/T005 `jobs/spec184/run.sbatch` environment boundary.
- **Symptom**: a directly invoked scheduler payload could supply a valid-looking
  `SPEC186_CANDIDATE_DIGEST` or `SPEC186_RUN_ID` different from the effective
  config, allowing a child process to report an identity unrelated to the
  scheduler record.
- **Root cause**: the allow-list checked names, types and NUL bytes but did not
  bind reserved identity variables to the validated config object.
- **Correction**: reject mismatched reserved identity variables before run-root
  creation and add a no-side-effect regression.
- **Lesson**: environment allow-lists need value-level binding for provenance
  fields; a permitted key is not trustworthy merely because its syntax is safe.

## 2026-09-13 — Spec186 YOLO case bundle omitted the catalogue authority key

- **Area**: T006/T007 direct MiniNDN package validation for Y-A/Y-B/Y-N.
- **Symptom**: the first real Y-B `validate_inputs` attempt stopped before
  topology startup with `CANONICAL_CATALOGUE_VERIFY_FAILED`; the canonical
  registry referenced `contracts/catalogue-authority.pub`, which was absent
  from the staged case bundle.
- **Root cause**: the case-bundle staging step copied the signed package,
  registry, trust maps and identity maps but treated the package directory as
  self-contained even though catalogue signature verification resolves the
  authority key relative to the case bundle.
- **Correction**: stage the public catalogue authority key at the declared
  `contracts/catalogue-authority.pub` path for every Y-A/Y-B/Y-N bundle,
  recompute each immutable bundle digest, and update the profile input seals.
- **Lesson**: a signed model package is not a runnable experiment input until
  every verifier-relative trust artifact named by its registry is present in
  the same sealed bundle.

## 2026-09-13 — Spec186 portable private-key maps were rejected at runtime

- **Area**: T006/T007 case-bundle validation and Provider process construction.
- **Symptom**: after the catalogue key was staged, the portable Y-B bundle
  still could not be used by the production runner because its private-key
  map deliberately contained `keys/<role>.pem` entries while both the
  preflight and Provider command builder required absolute paths.
- **Root cause**: portability was implemented for public maps but the private
  map checks retained an older host-only absolute-path requirement.
- **Correction**: resolve relative private-key entries against the map file's
  parent directory in both validation and process construction; retain digest
  binding and readability checks after resolution.
- **Lesson**: all path-bearing case-bundle fields need one consistent
  relative-to-bundle policy across preflight and child-process boundaries.

## 2026-09-14 — Spec186 profile accepted an unrelated source commit

- **Area**: T003/T005 candidate identity closure.
- **Symptom**: the profile validator checked only that `sourceCommit` was a
  40-hex value, so a syntactically valid commit unrelated to the requested
  `575b43c` baseline could reach candidate-manifest construction.
- **Root cause**: the baseline requirement existed in the Spec text and
  evidence but was not enforced at the executable profile boundary.
- **Correction**: require `sourceCommit` to be the baseline or a verifiable
  descendant using `git merge-base --is-ancestor`; add a mutation regression
  and clarify the baseline-versus-repaired-candidate rule in spec, plan and
  quickstart.
- **Lesson**: source provenance is a validation predicate, not documentation;
  commit syntax alone cannot establish candidate lineage.

## 2026-09-14 — Spec186 lineage gate skipped subtree callers

- **Area**: T003/T005 source-lineage regression for the TigerCluster profiles.
- **Symptom**: the new unrelated-commit regression did not raise because the
  test passed `Experiments/TigerCluster` as `repo_root`, and the validator
  treated that subtree as a git-less replay archive.
- **Root cause**: the lineage helper checked only `<repo_root>/.git` instead
  of resolving the enclosing repository root.
- **Correction**: walk `repo_root` and its parents for `.git`; keep the
  git-less Tiger replay behavior that consumes the submit-side sealed identity.
- **Lesson**: repository-root discovery must be independent of the profile's
  directory so a static gate cannot be bypassed by a valid subtree path.

## 2026-09-14 — Spec186 MiniNDN child PATH omitted system network tools

- **Area**: T004/T007 local and SIF-wrapped MiniNDN launch environment.
- **Symptom**: the launcher environment exposed `/usr/local/bin:/usr/bin:/bin`
  only, so a real MiniNDN start could fail before NFD creation when a host's
  `ifconfig` or related network utility resolved from `/usr/sbin` or `/sbin`.
- **Root cause**: the earlier scheduler PATH hardening was not propagated to
  the local child environment, the SIF Apptainer `--env` vector, or the
  scheduler allow-listed PATH.
- **Correction**: add `/usr/sbin:/sbin` consistently at all three process
  boundaries and retain the bounded child cleanup behavior.
- **Lesson**: runtime environment closure includes administrative system paths;
  matching application libraries alone does not make MiniNDN start portable.

## 2026-09-14 — Spec186 r7 SIF final Python packaging failed on base image data

- **Area**: T006 exact base-plus-application SIF build, Python extension stage.
- **Symptom**: the 1h07m C++/NDNSF build completed through `[213/213]`, then
  both the application packaging step and its image `%post` stopped during
  `pip install` with `FileNotFoundError` for
  `setuptools/_vendor/jaraco/text/Lorem ipsum.txt`.
- **Root cause**: the pinned base SIF's setuptools installation omitted a
  package-data file that setuptools imports while generating project metadata.
- **Correction**: restore the empty package-data file in the tracked runtime
  definition before pip builds the pybind extensions; the r7 SIF and record
  remain rejected and are not reused.
- **Lesson**: a successful native compile is not a complete SIF gate; run the
  final Python packaging/import checks inside the exact base image and retain
  the first packaging failure.

## 2026-09-14 — Spec186 r8 SIF omitted the installable DI shared library

- **Area**: T006 exact base-plus-application SIF build, native Python binding closure.
- **Symptom**: the 50m32s native build reached `[213/213]`, then `pythonWrapper`
  metadata generation rejected `/opt/ndnsf-di/current/lib` because it did not
  contain `libndnsf-distributed-inference`.
- **Root cause**: the runtime definition built the DI sources into the native
  provider executable but did not request or stage the installable
  `ndnsf-distributed-inference` shared-library target required by
  `pythonWrapper/setup.py`.
- **Correction**: add the DI shared library and pkg-config target to the locked
  Waf target set, stage/copy it into both builder and final runtime prefixes,
  include it in the native artifact mapping and final hash manifest, then retry
  from the same source-sealed base.
- **Lesson**: compiling a consumer with duplicated implementation objects does
  not satisfy the shared ABI boundary used by the Python extension; every
  declared link boundary must be built, staged, and verified as an artifact.

## 2026-09-14 — Spec186 r9 source archive omitted the DI pkg-config template

- **Area**: T006 exact base-plus-application SIF source closure.
- **Symptom**: the source-sealed r9 build passed dependency compilation, then Waf failed at the explicit target list with `could not find 'NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in'`.
- **Root cause**: `prepare-local-sif-source.py` selected the DI C++ and Python trees but omitted the tracked `ndnsf-distributed-inference.pc.in` input, so the sealed workspace archive could not materialize the requested pkg-config target.
- **Correction**: include the tracked pkg-config template in the source selector and regenerate the source-sealed handoff before the next SIF attempt.
- **Lesson**: every explicit Waf target must have its template input in the immutable source archive; local checkout presence does not prove sealed-build presence.

## 2026-09-14 — Spec186 r11 native closure audit did not resolve a system DSO symlink

- **Area**: T006 exact base-plus-application SIF final native closure.
- **Symptom**: after 284/284 native targets and both Python extension wheels succeeded, the builder audit stopped at `SYSTEM_LIBRARY_PACKAGE_UNKNOWN:/lib/x86_64-linux-gnu/libpthread.so.0`.
- **Root cause**: the audit queried `dpkg-query` only with the `ldd` symlink path; Ubuntu's package database records the resolved `libpthread-2.31.so` target under `libc6`.
- **Correction**: resolve each system DSO path before package ownership lookup while retaining the original and `/usr` alternate candidates.
- **Lesson**: dependency closure checks must account for loader-facing symlinks before declaring a system package unowned.

## 2026-09-14 — Spec186 r12 parallel NDNSF build triggered GCC 9 ICE

- **Area**: T006 exact base-plus-application SIF native build.
- **Symptom**: with the repository ceiling `-j4`, Waf failed while compiling `NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.cpp` with GCC 9 `internal compiler error: Segmentation fault`.
- **Root cause**: this host's GCC 9/Apptainer combination is unstable when large NDNSF translation units are compiled concurrently; the failure is a compiler resource/concurrency issue, not a source diagnostic.
- **Correction**: restore the serialized `-j1` largest-target build documented for this host; dependency stages remain bounded at `-j2`.
- **Lesson**: the TigerCluster `-j4` value is a hard upper bound, while this host's reproducible SIF recipe must use the lower stable parallelism selected by its compiler behavior.

## 2026-09-14 — Spec186 r13 native closure audit hit a stripped dpkg updates directory

- **Area**: T006 exact base-plus-application SIF final native closure.
- **Symptom**: after 284/284 native targets, both Python wheels, and DI
  staging succeeded, the builder audit stopped at
  `SYSTEM_LIBRARY_PACKAGE_UNKNOWN:/lib/x86_64-linux-gnu/libcrypto.so.1.1`.
- **Root cause**: the minimal base SIF retained dpkg status and file lists but
  omitted `/var/lib/dpkg/updates`; `dpkg-query -S` exits before scanning any
  path when that transient directory is absent, so the audit reported a false
  unowned system library.
- **Correction**: create the empty transient updates directory immediately
  before the audit's package-ownership loop, while keeping symlink resolution
  and `/lib`/`/usr/lib` alternate candidates.
- **Lesson**: package-closure audits must validate the package-manager state
  needed by their query tool; a minimal image can make a valid installed DSO
  appear unowned without any ABI defect.

## 2026-09-14 — Spec186 r14 native closure audit missed a retained Boost file list

- **Area**: T006 exact base-plus-application SIF final native closure.
- **Symptom**: after the complete native build and both wheels, the audit still
  stopped at `SYSTEM_LIBRARY_PACKAGE_UNKNOWN:/lib/x86_64-linux-gnu/libboost_filesystem.so.1.71.0`.
- **Root cause**: this reduced base image retained the Boost package's
  `/var/lib/dpkg/info/*.list` record but its `dpkg-query -S` path index did not
  consistently resolve the loader-facing `/lib` spelling.
- **Correction**: retain strict path and symlink checks, then fall back to an
  exact match in dpkg's package file lists before declaring a DSO unowned.
- **Lesson**: closure evidence must tolerate reduced package indexes without
  accepting a basename-only or guessed package mapping.

## 2026-09-14 — Spec186 r16 builder import exposed incomplete NumPy wheel data

- **Area**: T006 exact base-plus-application SIF Python import gate.
- **Symptom**: the complete 284/284 native build, both wheels, DI staging, and
  builder native closure audit succeeded, then the in-image import check failed
  with NumPy's misleading source-tree error; the underlying missing dependency
  was `libopenblas64_p-r0-0cf96a72.3.23.dev.so`.
- **Root cause**: the repaired base SIF contained NumPy 1.26.4 and its
  `_multiarray_umath` extension but an empty `numpy.libs` directory, so its
  manylinux private OpenBLAS/Fortran DSOs were absent.
- **Correction**: stage the exact NumPy 1.26.4 `numpy.libs` payload (OpenBLAS,
  libgfortran, and libquadmath) from the sealed handoff before builder and
  final import checks.
- **Lesson**: Python package presence and extension `ldd` closure are separate;
  wheel-private RPATH assets must be included in the immutable SIF composition.

## 2026-09-14 — Spec186 r17-r18 py-repoclient wheel retries failed before closure

- **Area**: T006 exact base-plus-application SIF Python extension packaging.
- **Symptom**: both retries completed the 284/284 native build and ndnsf wheel,
  then `py-repoclient` wheel construction exited nonzero before the builder
  native closure/import checks.  The Apptainer stream truncated the inner pip
  diagnostic, so no source-level error was observed.
- **Root cause**: unresolved packaging-stage failure; it did not reproduce in
  the earlier r13-r16 runs with the same source and toolchain, and no evidence
  ties it to the NumPy payload change.
- **Correction**: preserve both failed candidates as rejected, keep the exact
  source-sealed recipe unchanged, and run a bounded identical retry before
  changing packaging code.
- **Lesson**: a wheel-stage red must retain the candidate boundary and must not
  be converted into a code fix without the inner compiler diagnostic.

## 2026-09-14 — Spec186 r19 restored NumPy DSOs beside the wrong package tree

- **Area**: T006 exact base-plus-application SIF Python import gate.
- **Symptom**: r19 completed 284/284 native targets and both Python wheels,
  then the final import still failed with
  `libopenblas64_p-r0-0cf96a72.3.23.dev.so: cannot open shared object file`.
- **Root cause**: the template extracted the exact NumPy wheel-private DSOs
  into `/opt/ndnsf-stage/python/numpy.libs`, but NumPy 1.26.4 is supplied by
  the base SIF venv and its RPATH resolves from `/opt/venv/.../numpy/core` to
  `/opt/venv/.../site-packages/numpy.libs`.
- **Correction**: extract the same hash-locked DSO set beside the base venv's
  NumPy package, synchronize the portable handoff template, and retry once.
- **Lesson**: a wheel payload must be placed relative to the package that
  actually owns the importing extension; staging an identical directory in a
  different `sys.path` root does not satisfy an extension RPATH.

## 2026-09-14 — Spec186 r20 final SIF import lost the repaired NumPy DSOs

- **Area**: T006 exact base-plus-application SIF final image closure.
- **Symptom**: r20 completed all 284 native targets, both Python extension
  wheels, builder import and native closure checks, but the final `%post`
  import failed with `libopenblas64_p-r0-0cf96a72.3.23.dev.so` missing.
- **Root cause**: the r19 repair restored NumPy's private wheel payload only in
  the builder stage. The final stage starts from the base SIF, copies the
  staged application packages, and ran its import check before restoring the
  base venv's `numpy.libs` directory.
- **Correction**: restore the same hash-locked three-file NumPy payload in the
  final `%post` before `ldconfig` and the import gate; strengthen the cheap
  preflight to require both builder and final RPATH restoration sites.
- **Lesson**: a multi-stage SIF definition needs a closure assertion for every
  stage that imports inherited Python extensions; a passing builder probe does
  not prove the final image contains its wheel-private DSOs.

## 2026-09-14 — Spec186 r21 handoff selector saw an ignored NAC-ABE certificate

- **Area**: T006 source-sealed handoff preparation.
- **Symptom**: the handoff preparer stopped before writing a bundle with
  `HANDOFF_SOURCE_UNTRACKED:examples/example-trust-anchor.cert` even though
  the pinned NAC-ABE checkout was clean.
- **Root cause**: NAC-ABE's ignored `*.cert` example artifact existed in the
  selected `examples` tree. The selector walked filesystem files, while the
  handoff contract requires every archived source path to be Git-tracked.
- **Correction**: filter both workspace and dependency selectors against
  `git ls-files` before archive creation; the ignored certificate is excluded
  without changing the pinned checkout.
- **Lesson**: a clean Git status does not mean a recursive source walk contains
  only source-controlled files; source sealing must enforce tracked membership
  at selection time.

## 2026-09-14 — Spec186 r22 handoff omitted the wheel required by its own template

- **Area**: T006 source-sealed handoff and SIF input reproducibility.
- **Symptom**: the regenerated r22 handoff reported `SOURCE_READY` but its
  `wheels/` directory had no NumPy wheel; the strengthened preflight then
  stopped with `SPEC186_PREFLIGHT_NUMPY_WHEEL_COUNT`.
- **Root cause**: the handoff lock and `REQUIRED_WHEELS` set predated the
  NumPy RPATH repair, while the definition already required the exact
  `numpy-1.26.4` wheel for both builder and final stages.
- **Correction**: make the hash-locked NumPy wheel a first-class handoff
  dependency in the canonical lock and preparer, using the existing verified
  wheel digest.
- **Lesson**: every file consumed by a rendered definition must be declared by
  the source lock; a successful handoff manifest is insufficient if its input
  set is smaller than the recipe's runtime contract.

## 2026-09-14 — Spec186 r23 tracked filter dropped a symlinked workload

- **Area**: T006 source-sealed handoff preparation.
- **Symptom**: after filtering ignored files, r23 failed the SIF preflight at
  `SPEC175_WORKLOAD_NOT_SEALED:Experiments/TigerCluster/jobs/spec175/workload.json`.
- **Root cause**: the maintained `packaging/ndnsf-di-container/jobs` path is a
  compatibility symlink to `Experiments/TigerCluster/jobs`. The new tracked
  filter checked the symlink spelling against Git's canonical path list and
  discarded the workload that the build script passes to the preflight.
- **Correction**: resolve each selected path before tracked-membership and
  exclusion checks, then archive its canonical repository-relative spelling.
- **Lesson**: compatibility symlinks must be normalized before source-seal
  membership checks; otherwise a safety filter can remove a required runtime
  input while reporting a clean checkout.

## 2026-09-14 — Spec186 r24 preflight referenced base SIF before initialization

- **Area**: T006 local SIF build preflight.
- **Symptom**: the r24 build exited before the cheap preflight with
  `build-local-sif.sh: line 279: base_sif: unbound variable`.
- **Root cause**: the new preflight hook was inserted before the existing
  bootstrap parser initialized `base_sif`; `set -u` made this ordering defect
  deterministic whenever the caller did not export an unrelated shell
  variable.
- **Correction**: initialize the base-SIF identity variables alongside the
  other build inputs before constructing the preflight arguments, and add a
  static ordering assertion.
- **Lesson**: a fail-closed preflight must itself be executable under the
  builder's strict shell mode; validate hook dependencies before invoking it.
## 2026-09-14 — Spec186 r25 serialized GCC provider ICE

- **Area**: T006 exact base-plus-application SIF native build.
- **Symptom**: the corrected r25 recipe passed its preflight and compiled 120
  native targets, then GCC 9 terminated while compiling
  `NativeAuthenticatedGrantClient.cpp` with `internal compiler error: in
  ggc_set_mark, at ggc-page.c:1547`. Apptainer stopped the builder at
  `LOCAL_SIF_BUILD_START`; no SIF or build record was promoted.
- **Root cause**: the host GCC 9 compiler remains unstable for a large NDNSF
  translation unit even with the largest target serialized at `-j1`.
- **Correction**: retain this first failure and add the already qualified
  Clang 10 fallback as an explicit, container-installed compiler input. A
  fallback build must start from a clean NDNSF build tree and record the
  selected compiler identity in the candidate manifest.
- **Lesson**: a lower Waf job count is only a concurrency bound, not a proof
  that GCC can compile this source. Compiler fallback is part of the sealed
  build recipe and must be validated before SIF promotion.
## 2026-09-14 — Spec186 r26 base image missing dpkg updates directory

- **Area**: T006 exact SIF builder dependency installation.
- **Symptom**: r26 passed the rendered-definition preflight, but the builder
  stopped before compiling source when `apt-get install clang-10` invoked
  `dpkg` and reported `cannot scan updates directory
  '/var/lib/dpkg/updates/': No such file or directory`.
- **Root cause**: the repaired local bootstrap SIF omits the normal dpkg state
  directories; the added explicit Clang fallback therefore reached a missing
  package-manager directory.
- **Correction**: create `/var/lib/dpkg/updates` (and the optional apt
  preferences directory) before the first package install, then retry with
  the same sealed source and dependency inputs.
- **Lesson**: adding a compiler fallback also requires checking the package
  manager state of the bootstrap image before consuming a full native build.
## 2026-09-14 — Spec186 r27 build stopped to restore successful-template route

- **Area**: T006 exact SIF build operator boundary.
- **Symptom**: r27 passed preflight and reached the native Waf build, but the
  user stopped it at 10/284 to prevent another long retry while the workflow
  was being changed to reuse the historical successful Tiger GPU template.
  No r27 SIF or build record was promoted.
- **Root cause**: the current repair loop was still rebuilding from a newly
  evolved definition instead of first comparing against the retained
  Spec183 GPU-success tuple and making only active-Spec changes.
- **Correction**: retain the partial run as non-qualification evidence, add a
  mandatory successful-candidate template to `itiger-ndnsf-ops`, and keep a
  repository copy under `Experiments/TigerCluster/docs/` for every future
  Spec186/TigerCluster preparation.
- **Lesson**: prior successful SIF/Tiger receipts are reusable design inputs;
  consult them before starting an expensive build, while still revalidating
  every changed source, ABI, SIF, app, model, and node plane.
## 2026-09-14 — Spec186 r28 stale bootstrap SIF digest

- **Area**: T006 successful-template source handoff render.
- **Symptom**: the first r28 render rejected the bootstrap input with
  `HANDOFF_BASE_DIGEST`; the temporary lock retained an older SHA for the
  local repaired base SIF.
- **Root cause**: the historical base file was reused without recomputing its
  bytes after the prior interrupted build series. The file is 3,088,814,080
  bytes with SHA-256
  `4182109c0e3d26cf9c52928a571abf2bb0349a3f28682147fb2d80e0fc3da38b`.
- **Correction**: update the new candidate-only lock with that exact digest,
  regenerate the handoff and definition, and rerun the preflight before any
  build.
- **Lesson**: reuse means revalidate exact bytes, not copy a historical
  digest; every SIF transfer or file replacement must refresh the candidate
  identity.
## 2026-09-14 — Spec186 r28c Clang toolchain-root omission

- **Area**: T006 native Waf configure using the historical successful recipe.
- **Symptom**: after dependency stages completed, native configure rejected
  `/usr/lib/llvm-10/bin/clang` as outside the required `/usr/bin` toolchain
  root and stopped before compiling source.
- **Root cause**: the template copied the Clang10/O0 compiler and flags but
  omitted the historical explicit `--toolchain-root=/usr` configure option.
- **Correction**: add that option to the canonical native configure command,
  regenerate the sealed definition, and rerun preflight before building.
- **Lesson**: a successful compiler name/flag match is insufficient; the Waf
  toolchain-root boundary is part of the reusable build recipe.
## 2026-09-14 — Spec186 r30 bootstrap SIF corrupted CUDA block

- **Area**: T006 exact source-sealed SIF final image assembly.
- **Symptom**: r30 compiled all 284 native targets, built both Python wheels,
  passed native import/help/`ldd` checks, then Apptainer failed while
  re-extracting the bootstrap image: `zstd uncompress failed with error code
  20` for `usr/local/cuda-12.4/.../libnppist.so.12.2.5.30`.
- **Root cause**: the reused local bootstrap SIF has a damaged compressed
  CUDA block. Its SIF table is readable, but full rootfs extraction is not;
  the image cannot be used as a base even though earlier filtered stages did
  not touch the damaged file.
- **Correction**: retain r30 as a failed immutable build attempt and replace
  the bootstrap input with the verified Spec183 successful-template v23 base
  SIF before retrying. Recompute its SHA and rerun the cheap preflight first.
- **Lesson**: SIF metadata and partial extraction are insufficient; validate
  complete bootstrap decompression before spending another native build.
## 2026-09-14 — Spec186 r31 handoff saw post-r30 documentation commit

- **Area**: T006 source handoff regeneration.
- **Symptom**: the first r31 handoff rejected the checkout because its lock
  still named the r30 source revision `18e05f09`, while the working tree now
  includes the committed runtime-path documentation/profile correction
  `32e25cdd`.
- **Root cause**: handoff revision validation correctly requires the lock to
  match the exact current checkout, even when the selected SIF source files
  are unchanged.
- **Correction**: update the candidate-only r31 lock to the current checkout,
  regenerate the handoff and definition, and keep the source seal as the
  authority for the selected build inputs.
- **Lesson**: every retry must rebind the lock to the actual checkout before
  rendering; do not force a stale revision through the handoff validator.
## 2026-09-14 — Spec186 r31 local unsquashfs was older than the template image

- **Area**: T006 bootstrap SIF portability.
- **Symptom**: the remote v23 SIF passes `apptainer exec` and remote
  `unsquashfs` 4.4-git.1 extraction, but local Apptainer 1.5.3 with Ubuntu's
  `unsquashfs` 4.4 (2019) fails zstd decompression on different files such as
  `usr/lib/gcc/x86_64-linux-gnu/9/lto1`.
- **Root cause**: the local squashfs-tools build is older than the toolchain
  used on Tiger and cannot reliably unpack the historical zstd filesystem;
  the SIF bytes themselves match the verified remote SHA.
- **Correction**: build a newer squashfs-tools `unsquashfs` from upstream on
  the local host, install it ahead of `/usr/bin`, and rerun the same v23-based
  SIF build without changing source or dependency identities.
- **Lesson**: a matching Apptainer version still needs a compatible
  squashfs-tools helper; validate the helper version before blaming SIF data.
## 2026-09-14 — Spec186 Tiger profile retained login-node Apptainer path

- **Area**: T009/T010 Tiger runtime-version boundary.
- **Symptom**: the login host reported `/usr/bin/apptainer` 1.3.4, while the
  Spec186 contract requires 1.5.3 on every compute-node SIF invocation. The
  profile and two static tests still named `/usr/bin/apptainer` as the compute
  executable.
- **Root cause**: the earlier preflight receipt described a historical compute
  path, but the current Tiger login environment exposes only the 1.3.4 system
  package; the project-owned 1.5.3 executable had not yet been made explicit.
- **Correction**: the project tools directory rejected the unstripped binary
  at its per-user quota, so a stripped copy of the same verified 1.5.3 build
  was staged at `/home/tma1/.local/bin/apptainer-1.5.3` and verified by both
  version and SHA. Tiger profiles, runtime policy, quickstart, and tests bind
  that path; login remains SSH/Slurm metadata only.
- **Lesson**: a version string in a receipt is insufficient; the candidate must
  bind the executable path that the allocated compute node will actually run.

## 2026-09-14 — Spec186 local bootstrap copy drifted from the sealed v23 SIF

- **Area**: T006 bootstrap SIF identity.
- **Symptom**: the local `.codex-tmp/spec186-template-base-v23.sif` retained
  the expected byte count but its SHA changed from the remote sealed value
  `44b44d...` to a different digest before the next build attempt. Local
  `unsquashfs` failures therefore could not be attributed to a verified copy.
- **Root cause**: the bootstrap path was treated as a mutable scratch input;
  no immutable pre-build hash check prevented an unnoticed byte change. The
  exact mutating process was not established.
- **Correction**: stop the build, discard the drifted input, recopy the v23
  SIF from Tiger, and require the remote and local SHA-256 values to match
  before rendering a new definition.
- **Lesson**: every retry must verify the complete base SIF digest immediately
  before extraction; matching size or an earlier receipt is insufficient.

## 2026-09-14 — Spec186 local zstd helper rejects a valid v23 file block

- **Area**: T006 SIF materialization.
- **Symptom**: after replacing the drifted bootstrap with the exact remote SHA,
  local Apptainer 1.5.3 still stopped at
  `opt/venv/lib/python3.10/site-packages/triton/_C/libtriton.so` with zstd
  error code 20. Tiger's own `unsquashfs 4.4-git.1` extracts that file and the
  same full SIF SHA is verified on both hosts.
- **Root cause**: the local and Tiger extraction environments do not produce
  the same result for this historical zstd block; changing helper versions or
  copying the helper alone did not close the difference. The local path is not
  a trustworthy materialization boundary for this base image.
- **Correction**: stop local SIF retries and move the complete 1.5.3 build to
  the Tiger compute environment, where the sealed base was produced and its
  SquashFS extraction already succeeds. Keep the base digest and source seal
  unchanged.
- **Lesson**: a successful small-file probe is insufficient; validate the
  complete bootstrap extraction in the same 1.5.3 environment that will run
  the candidate, and record the failing path before switching hosts.

## 2026-09-14 — Spec186 r38 rootless builder lacked subordinate IDs and compatible fakeroot

- **Area**: T006 SIF build on Tiger compute.
- **Symptom**: the first r38 allocation stopped before `%post` with
  `newgidmap: write to gid_map failed: Operation not permitted`; the Tiger
  account has no `/etc/subuid`/`/etc/subgid` entry. A root-mapped retry then
  reached the pinned Ubuntu 20.04 base but its embedded fakeroot helper
  required `GLIBC_2.33`/`GLIBC_2.34`, while the base provides an older glibc.
- **Root cause**: the user-space Apptainer 1.5.3 executable had no setuid
  starter and the default fakeroot path was incompatible with the base image.
- **Correction**: add an explicit Tiger root-mapped build mode to the
  repository build entrypoint. It uses the same Apptainer 1.5.3 binary with
  `--ignore-subuid --ignore-fakeroot-command`; the `%post` runs as uid 0 in
  the root-mapped namespace. The mode is opt-in via
  `SPEC186_APPTAINER_ROOT_MAPPED=1` and does not alter privileged builds.
- **Lesson**: version alignment alone does not establish build capability;
  record subordinate-ID, starter, and base-glibc prerequisites before a full
  SIF build.

## 2026-09-14 — Spec186 r38 v23 base omitted the locked Rust builder inputs

- **Area**: T006 builder dependency closure.
- **Symptom**: after adding the ONNX SDK, the compute build passed the ONNX
  checks and stopped at `test -x /opt/rust-prefix/bin/cargo`.
- **Root cause**: the historical v23 dependency base contains ONNX Runtime and
  a Python environment, but not the locked Rust 1.90.0 toolchain or the
  offline Cargo registry required by the rendered recipe.
- **Correction**: restore the already verified Rust 1.90.0 toolchain and
  tokenizers Cargo registry as builder-only inputs in the compute staging
  area; the final two-stage definition keeps the inputs inside the same
  1.5.3-built image and records their hashes in the build receipt.
- **Lesson**: a base SIF name is not a dependency closure. Check every
  explicit `/opt` prerequisite before starting native compilation.

## 2026-09-15 — Spec186 r38 final stage lost the NumPy wheel input

- **Area**: T006 final two-stage SIF assembly.
- **Symptom**: r38 completed all 284 native targets, both Python extensions,
  import checks and `ldd` checks in the builder, then failed in the final
  `%post` with `AssertionError: NUMPY_WHEEL_COUNT`.
- **Root cause**: the final stage intentionally receives only `%files from
  builder`; it has no `/build-input/wheels` directory. The final-stage
  script nevertheless tried to reopen the builder-only NumPy wheel instead
  of consuming the already checked private DSOs.
- **Correction**: carry the builder's
  `/opt/venv/lib/python3.10/site-packages/numpy.libs` directory through the
  stage boundary and validate the exact three expected DSOs in the final
  image. The wheel remains a builder input and is not copied into the runtime
  image.
- **Lesson**: every final-stage check must use files declared in that stage's
  `%files` inputs; do not reference source or wheel paths removed at the
  builder cleanup boundary.

## 2026-09-15 — Spec186 r38 preflight encoded the old NumPy stage contract

- **Area**: T006 cheap definition preflight.
- **Symptom**: after carrying `numpy.libs` through `%files from builder`, the
  corrected r38 definition was rejected before the retry with
  `SPEC186_PREFLIGHT_NUMPY_FINAL_RPATH_RESTORE_MISSING:1`.
- **Root cause**: the repository preflight counted the old wheel-extraction
  Python block twice. The final stage now consumes the builder payload through
  an explicit stage-boundary copy, so only the builder block legitimately
  contains the wheel destination expression.
- **Correction**: update the preflight contract to require one builder
  destination plus the final-stage `%files` input and copy markers. No native
  compilation is needed for this correction.
- **Lesson**: static gates must describe the current multi-stage ownership
  boundary; changing a definition without updating its gate creates a false
  pre-build failure.
## 2026-09-15 — Spec186 r38 final replay omitted Spec162 repo helpers

- **Area**: T006 exact-SIF replay closure on the Tiger compute node.
- **Symptom**: r38 completed the 284-target native build, Python wheels,
  native import/`ldd` checks, and final SIF creation with Apptainer 1.5.3,
  but the final SIF preflight returned `SPEC175_SIF_RUNTIME_INVALID` because
  `/opt/ndnsf-di/replay/repo/specs/162-itiger-qwen36-generation/jobs/run-repo-node.py`
  and `register-qwen36-repo.py` were absent.
- **Root cause**: the sealed workspace archive contained both maintained
  Spec162 helpers, while the replay staging block copied `Experiments`, tools,
  tests, examples, and package wrappers but never copied the `specs/162...`
  subtree into the image.
- **Correction**: copy the source-sealed Spec162 subtree into
  `/opt/ndnsf-stage/replay/repo/specs/` and add a static template assertion;
  rerun the candidate before accepting any SIF or YOLO result.
- **Lesson**: source-sealer inclusion and image replay inclusion are separate
  boundaries; exact-SIF preflight must verify every runtime file at its final
  in-image path before GPU execution.

## 2026-09-15 — Spec186 r38 GPU launch referenced missing staged inputs

- **Area**: T009/T010 exact-SIF GPU launch input binding.
- **Symptom**: after the final SIF build, native import and `ldd` closure
  passed, but Apptainer rejected the first YOLO launch before MiniNDN startup:
  `mount source /project/tma1/ndnsf-di/identities/spec186 doesn't exist`.
  The configured `spec186-yolo-case-bundles/Y-A` directory was also absent on
  the project filesystem.
- **Root cause**: the ad-hoc runtime wrapper used planned paths rather than
  the currently staged project paths; the case and identity directories had
  been cleaned or renamed while the wrapper retained their old names.
- **Correction**: use the complete staged
  `candidates/spec180-runtime-b6710fd6/models` input tree (including the
  verified `yolo26n.onnx` digest), remove the nonexistent identity bind because
  the runner bootstraps per-process keychains, bind a durable state directory,
  and persist the final SIF outside `/tmp` before launching.
- **Lesson**: a valid SIF cannot compensate for stale external binds; validate
  every host input path and persist the image before starting a GPU campaign.

## 2026-09-15 — Spec186 r38 persistent intermediate path was not rendered

- **Area**: T006 persistent Tiger SIF build inputs.
- **Symptom**: the persistent intermediate SIF completed successfully, but the
  final build stopped before container creation with
  `LOCAL_SIF_BASE_MISSING path=/tmp/spec186-r38-intermediate-base.sif`.
- **Root cause**: the wrapper moved `INTERMEDIATE` to project storage while
  the already rendered final definition still referenced its former `/tmp`
  `From:` path.
- **Correction**: update the rendered final definition to the same persistent
  project-storage path and reuse the verified intermediate SIF on the next
  allocation.
- **Lesson**: moving an artifact variable is insufficient for a rendered
  definition; the definition's `From:` identity and the wrapper path must be
  checked together before dispatch.

## 2026-09-15 — Spec186 r38 canonical catalogue wrapper stopped before MiniNDN

- **Area**: T009 single-node launch after the compute-side SIF build.
- **Symptom**: job `212306` completed the 284-target native build, final SIF
  creation and in-container native import with Apptainer 1.5.3, then returned
  `SPEC180_CASE_RESULT status=WAITING_EXTERNAL_INPUT
  error=CANONICAL_CATALOGUE_VERIFY_FAILED` before MiniNDN startup (Slurm exit
  78). No protocol, numerical, CUDA-role or cleanup result was produced.
- **Root cause boundary**: the runner converts the original exception into the
  generic catalogue error. Direct `cryptography` import and direct
  `ModelFamilyAdapter` catalogue diagnostics pass in the same final image, so
  this is not evidence of a failed SIF build or invalid catalogue by itself;
  the runner-specific path/module/input exception remains unresolved.
- **Correction**: preserve the immutable r38 SIF and receipt, capture the raw
  `_validate_package` exception before retry, and require local-first SIF
  promotion or a separately sealed compute-build exception. Do not blind-rebuild
  or promote this job to a GPU result.
- **Lesson**: a wrapper status is not a root cause. Keep build/import,
  catalogue-adapter diagnostics and MiniNDN/GPU qualification as separate gates
  and record the first uncaught exception before changing candidate inputs.

## 2026-09-15 — Spec186 replay source shadowed sealed native bindings

- **Area**: T009 canonical-catalogue validation inside the sealed SIF.
- **Symptom**: the corrected raw-exception probe (job `212356`) showed
  `ImportError: cannot import name '_ndnsf' from partially initialized module
  'ndnsf'` from the replay checkout's `pythonWrapper/ndnsf/__init__.py`.
  The earlier probe (`212355`) failed in its own diagnostic loader because it
  did not register the dynamically loaded module in `sys.modules`; that was a
  probe defect, not candidate evidence.
- **Root cause**: the replay runner inserted
  `NDNSF-DistributedRepo/pythonWrapper` and `pythonWrapper` before the venv
  site-packages in both its launcher path and child `PYTHONPATH`. The pure
  Python source package therefore shadowed the installed `_ndnsf.so` and
  `_py_repoclient.so`, despite the final SIF import check passing.
- **Correction**: select source wrapper roots only when the active interpreter
  lacks the corresponding compiled extension; make SIF site-packages the first
  child path entry; add a regression test covering both bindings. Rebuild and
  reseal the candidate before qualification.
- **Lesson**: a final `import ndnsf._ndnsf` check does not cover every replay
  path. Validate the exact entrypoint's import ordering under the same SIF and
  treat source/package shadowing as a build/runtime closure defect.

## 2026-09-15 — Spec186 build preflight did not inspect the declared base

- **Area**: T006 definition/base capability preflight.
- **Symptom**: static review found that `build-local-sif.sh` invoked
  `preflight-development-sif.py` while `base_sif` was still empty. The
  preflight therefore checked the definition and archives but skipped its
  read-only base NumPy import, even for a `Bootstrap: localimage` candidate.
- **Root cause**: the shell parsed the definition's `From:` path only after
  the preflight and before `apptainer build`; the ordering made the optional
  `--base-sif` argument silently disappear.
- **Correction**: resolve and hash the actual localimage base before
  preflight; add source-consumer archive checks and base-owned executable,
  SDK, Rust and header capability checks; cover the ordering and diagnostics in
  `test_development_runtime_template.py`.
- **Lesson**: optional preflight arguments must be derived from the rendered
  candidate before the gate runs. A gate that can silently downgrade to a
  weaker mode is itself a release defect.

## 2026-09-15 — Spec186 Waf defaulted to a developer-only Rust dependency path

- **Area**: T006 native dependency and packaging boundary.
- **Symptom**: root `wscript` silently searched `.codex-tmp/spec182-t001-dependencies`
  for the Rust tokenizer toolchain and Cargo cache. A clean host or a sealed
  builder without that historical directory failed later, while a developer
  checkout could appear portable only by accident.
- **Root cause**: the tokenizer bridge helper treated an old Spec182 staging
  directory as the default provider instead of requiring the candidate to
  declare its toolchain and cache. Static inspection also confirmed that the
  current Waf graph combines DI mechanism, ONNX, YOLO and Qwen sources, and
  creates the ONNX-dependent assembly worker before the examples guard; the
  Python package split does not yet select independent C++ profiles.
- **Correction**: require `NDNSF_RUST_PREFIX` and `NDNSF_CARGO_HOME`, default
  only the bridge target to the regular `build/` tree, add a regression test,
  and record the source/target map in
  `Experiments/TigerCluster/docs/dependency-boundaries.md`. Keep ONNX,
  Protobuf, Rust and assembly-worker checks until an explicit profile/target
  graph is implemented and clean-built.
- **Lesson**: a dependency check is useful only when its provider is explicit
  and its ownership matches the selected target. Packaging metadata and Waf
  sources must be audited together before narrowing a SIF profile.

## 2026-09-15 — Spec186 capability preflight omitted exported Rust paths

- **Area**: T006 source/definition/base capability preflight.
- **Symptom**: the rendered builder definition checked Rust and Cargo with
  `NDNSF_RUST_PREFIX` and `NDNSF_CARGO_HOME`, but the preflight's literal-path
  parser returned no Rust/Cargo predicates. A missing tokenizer toolchain could
  therefore pass the cheap capability gate.
- **Root cause**: `_base_capability_tests()` matched only unquoted literal
  `/usr` or `/opt` paths and did not expand variables exported earlier in the
  same builder shell.
- **Correction**: parse stable `export NAME=/absolute/path` assignments,
  expand them in `test -x/-f/-d` predicates, and add regression assertions for
  cargo, rustc and the Cargo registry source directory.
- **Lesson**: static checks must model the small shell language used by the
  definition; checking only literal tokens is insufficient for declared
  capability variables.
## 2026-09-15 — Spec186 r61 rejected stale r55 host gate before build

- **Area**: T006 current source-seal and host-gate cross-check.
- **Symptom**: `build-local-sif.sh --strict-host-source-seal` for the r61
  definition with the retained r55 manifest stopped before base preflight with
  `SOURCE_SEAL_REVISION_SOURCE_CHANGE`; the sealed paths included the changed
  preflight helper and `wscript`.
- **Root cause**: r55 is a tiny-ONNX host qualification for an older source
  identity and cannot authorize the current r61 native build.
- **Correction**: preserve r55 as historical evidence, require a new host-gate
  run for the current source/app identity, and do not start SIF compilation
  with the stale manifest.
- **Lesson**: a structurally valid host manifest is not reusable across source
  or build-helper changes; strict source-seal comparison must run before any
  expensive SIF operation.
