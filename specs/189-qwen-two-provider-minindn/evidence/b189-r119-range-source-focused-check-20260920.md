# Spec189 r119 range-source focused-check boundary

## Result

`UNQUALIFIED`: the first r119 focused-selector invocation did not validate the
new range-backed publication repair. The integration and activation selectors
were invoked with `NDNSF_SPEC182_BIN_DIR=/usr/local/bin`, while the worker
fixture produced by the same build was still in `build-spec189-oracle/`; all
worker-dependent cases stopped at fixture discovery. The Repo publication
selector did run and exposed an existing caller mismatch: the Repo publisher
still used `MaterialPayload::bytes.size()` for external initializer range-view
payloads, producing `repo-publication-identity-mismatch`.

## First boundaries

- integration material consumer: worker fixture discovery, before assertion;
- ONNX activation cases: worker fixture discovery, before assertion;
- `Spec189RepoPublication/MaterialManifestPublishesWithOwnedTransactionsAndRejectsCorruption`:
  `repo-publication-identity-mismatch`, caused by the Repo publisher treating
  range-view payloads as empty owned vectors.

This is a focused validation/integration boundary, not a MiniNDN protocol or
model result. No MiniNDN process was started by this selector command and no
qualification status changed.

## Changed gate and repair

The changed gate is the producer material ownership path. The candidate repair
adds an authenticated `NativeCanonicalByteRangeSource`, attaches a verified
Repo range reader to the loaded initializer, emits external initializer chunks
through that reader after source inspection, and makes both protected and Repo
publishers consume bounded `copyBytes()` ranges. The Repo publisher's
`bytes.size()` checks were corrected to `byteSize()` and its writes now use
`copyBytes()`.

The next validation must rebuild the affected targets, use
`NDNSF_SPEC182_BIN_DIR=build-spec189-oracle`, rerun the named C++ selectors,
install the verified candidate, and only then attempt a fresh MiniNDN run.

Raw selector output is `.codex-tmp/spec189-r119-material-selector.log` and
`.codex-tmp/spec189-r119-onnx-repo-selector.log`.

## Follow-up selector boundary

After rebuilding and rerunning with `NDNSF_SPEC182_BIN_DIR=build-spec189-oracle`,
the material consumer selector passed `2/2` and the ONNX extraction,
assembly, and activation cases entered their assertion bodies. The Repo
publication selector still found two source-level gaps: the newly attached
range reader used the strict Core identity helper against a manifest whose
backend identity representation was not byte-for-byte stable, and selected
external initializer references fetched only the header, not the authenticated
`chunkPayloadIds`. The first appeared as `repo-publication-identity-mismatch`
in `RepoSourceMissReusesValidatedInitializer`; the second appeared as
`DI_NATIVE_ONNX_MATERIAL_INITIALIZER` in the material publication case.

The next bounded repair changes the range reader to compare the pinned
object/generation/digest identity before a bounded Repo range read, and makes
post-Selection selection include every authenticated initializer chunk. The
rerun remains a focused C++ boundary; no MiniNDN qualification is claimed.

The next rerun passed the material consumer `2/2`, ONNX extraction/assembly/
activation selectors, and the source-range test. It then reached the
post-Selection Repo selector and found one remaining contract gap:
`selected material payload differs from manifest reference`. The published
material index authenticates chunk IDs through a shared-initializer reference,
while the chunk digest/size lives in the root's authenticated `materialObjects`
records; the selector still required every chunk to appear as a direct
reference. The next fix must accept a chunk through its owning authenticated
reference and compare its digest/size to the root object record.

## Follow-up selector repair result

After the chunk-reference binding repair, the affected C++ build passed
`434/434`. With `NDNSF_SPEC182_BIN_DIR=build-spec189-oracle`, the material
consumer selector passed `2/2`, and the combined ONNX extraction, assembly,
activation, and Repo publication selectors completed with `*** No errors
detected`. `RepoSourceMissReusesValidatedInitializer` exercised the bounded
Repo range source, and `MaterialManifestPublishesWithOwnedTransactionsAndRejectsCorruption`
completed the post-Selection material checks.

This is a focused native boundary result only. It does not establish installed
runtime provenance, MiniNDN request-chain completion, model execution, terminal
response, repeat-round behavior, or qualification PASS. The next gate is to
review the affected diff, rebuild/install the exact runtime consumers, verify
their loaded library paths and hashes, and then run a fresh guarded MiniNDN
candidate.

Raw selector output is `.codex-tmp/spec189-r119-material-selector-v4.log` and
`.codex-tmp/spec189-r119-onnx-repo-selector-v4.log`; the build output is
`.codex-tmp/spec189-r119-build-range-source-v4.log`.

## r120 launcher invocation boundary

The first fresh runtime attempt after installation did not enter MiniNDN. The
launcher reached candidate preparation and generated the provider PIBs, then
failed while deriving the provider signing-key prefix with
`provider key prefix unavailable: /example/ndnsf-qwen06b/provider-0`.
Inspection of the preserved r120 PIB shows the provider key is present. The
actual boundary was the invocation environment: `/usr/bin/python3` under
`sudo env` could not import the user-installed `ndn` package, and the
launcher’s optional `ndn.encoding` import was caught by its decode loop.

This is an invocation/Python-environment failure, not a provider, protocol,
resource, or model result. Raw launcher output is
`.codex-tmp/spec189-r120-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r120/`.
The retry must preserve the same installed candidate and explicit digests while
adding the verified user-site `PYTHONPATH` to the root launcher environment.

## r121 MiniNDN resource boundary

The r121 retry used the verified user-site `PYTHONPATH`, the installed
candidate, and a fresh run root. It passed launcher key initialization,
MiniNDN startup, both Provider readiness/offer decisions, and both
`NDNSF_DI_GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY` checks. The unchanged
host guard then stopped at `RESOURCE_BOUNDARY:MemAvailable` before requester
ACK/Selection. The run sampled 298 records: minimum available memory was
`1003737088` bytes, maximum owned swap was `524922880` bytes, maximum
swap-I/O delta was `1107390464` bytes, and aggregate RSS peaked at
`9811668992` bytes. At the threshold, the provider assembly worker child
reached approximately `5112135680` bytes RSS while its provider parent was
approximately `2050000000` bytes RSS. Cleanup passed and no process remained.

The requester recorded only `CANCELLED`; there is no `ACK_CLOSED`, Selection,
`ASSEMBLY_STARTED` completion, runner readiness, execution, terminal response,
repeat round, or oracle result. This is a native parent/worker ownership
resource boundary, not a protocol or qualification PASS. The next bounded
repair must release the parent-side model and initializer buffers immediately
after the worker request pipe is fully written, while retaining them until
that write barrier and preserving scrub/cleanup behavior.

Raw launcher output is `.codex-tmp/spec189-r121-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r121/`.

## r122 parent-source release repair gate

The bounded repair adds an optional `sourceToReleaseAfterWrite` argument to
the OA02 transport. The default four-argument behavior remains
non-destructive for existing callers and tests. Production
`NativeCanonicalOnnxAssembler` passes its mutable canonical source; after the
complete request frame is written and the parent closes the input pipe, OA02
scrubs and releases the parent model, initializer, selected payloads, manifest,
and range-source references. The source is not released before that write
barrier, and the worker response, recipe digest, authorization checks, and
cleanup semantics are unchanged.

The affected targeted build completed `538/538`. The material consumer
selector passed `2/2`; the combined ONNX extraction, native assembly,
activation, and Repo publication selector ran `30` cases with
`*** No errors detected`. The five installed runtime targets have exact
build/installed SHA-256 equality; the corrected closure check found no
unresolved libraries and no build-tree or `.codex-tmp` dependency. These are
static/focused/install gates only. The next gate is a new guarded MiniNDN run
with the same candidate inputs and a new r122 run root.

Raw outputs are `.codex-tmp/spec189-r122-targeted-build.log`,
`.codex-tmp/spec189-r122-material-selector.log`,
`.codex-tmp/spec189-r122-onnx-repo-selector.log`, and
`.codex-tmp/spec189-r122-runtime-identity-v2.log`.

## r122 MiniNDN disk boundary

The fresh r122 run used the repaired installed candidate and entered MiniNDN.
Both Providers reached `NDNSF_DI_NATIVE_PROVIDER_READY`, but the requester
did not produce `ACK_DECISION` or Selection before the unchanged host guard
stopped at `RESOURCE_BOUNDARY:diskFree`. The preserved resource stream has 45
samples: minimum disk free was `4001157120` bytes against the unchanged
`4294967296`-byte limit; minimum available memory was `6973997056` bytes,
maximum aggregate RSS was `3527479296` bytes, maximum owned swap was `4096`
bytes, and swap-I/O delta was `180342784` bytes. Cleanup passed and no process
remained. The r122 run directory is approximately `2.5G`, with the requester
subtree holding that large working set; the broader host filesystem was at
98% use when inspected.

There is no requester ACK/Selection, Provider grant verification,
`ASSEMBLY_STARTED`, layer fetch, runner, execution, terminal response, repeat
round, or oracle result. This is a host disk-budget boundary, not a protocol,
model, or resource PASS. The next step is a bounded disk-artifact/working-set
review and an authorized space-preserving run setup; the guard must remain
unchanged and all prior failure evidence/raw boundaries must remain preserved.

Raw launcher output is `.codex-tmp/spec189-r122-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r122/`.

## Space-preserving artifact handling

The host disk boundary was investigated without changing the guard. The
canonical initializer is already immutable and shared by hard links for the
validated r114/r118/r121 requester copies. The r118 and r121 Repo payload paths
were independently hashed against the canonical initializer and then replaced
with hard links to the same inode; their original paths and bytes remain
available, while approximately `2.2G` of duplicate blocks was released. The
r122 staging `.part` file had a different digest and was deliberately not
deduplicated. After this bounded operation, root free space was
`7007141888` bytes. No source, build, installed binary, or guard configuration
changed.

## r123 oracle-digest invocation boundary

The first r123 command stopped in preflight with
`SPEC189_ORACLE_BINARY_DIGEST_MISMATCH`: the supplied oracle digest omitted a
`4` (`...a378615...`) while the verified build/installed digest is
`sha256:ef12b58e7fb2e3b0f0643ab7afafe02a3786456150c178a00402a3adef7b9086`.
No MiniNDN process or request chain was started. This is an invocation typo,
not a source, protocol, resource, or model result. The next retry keeps the
same candidate, run inputs, guard, and fresh-run policy and uses the exact
verified oracle digest.

Raw launcher output is `.codex-tmp/spec189-r123-launch.log`.

## r124 MiniNDN disk boundary

The corrected r124 invocation entered MiniNDN with the same installed
candidate. It stopped before requester `ACK_DECISION`/Selection at the
unchanged `RESOURCE_BOUNDARY:diskFree` guard. Cleanup passed and no process
remained. The run root accumulated approximately `4.0G`, including the
requester encrypted Repo payload set; available memory remained healthy and
the boundary was disk capacity rather than the repaired assembly-worker
working set. There is no grant verification, assembly, layer fetch, runner,
execution, terminal response, repeat round, or oracle result.

This is a host artifact-placement boundary, not a protocol, model, or
qualification result. The next bounded change is to make the experiment run
scoped encrypted Repo path explicit so it can be placed on a separately
validated temporary filesystem without changing the guard, production
ownership contract, or candidate inputs.

Raw launcher output is `.codex-tmp/spec189-r124-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r124/`.

## r125 external encrypted-Repo path gate

The experiment runner now accepts an optional
`--encrypted-repository-path`. The default remains
`<run-root>/requester/encrypted-repo`; an external path must be outside the run
root and new or empty, is created mode `0700`, and is recorded in the
requester configuration. This changes only placement of regenerated
run-scoped ciphertext; the host guard still samples the run-root filesystem,
the production Repo/DI ownership contract is unchanged, and all logs,
identity material, configuration, and durable evidence remain under the run
root.

The focused Python regression set passed `44` tests and the runner compiled
with `py_compile`. The next MiniNDN candidate uses the separate
`/dev/shm/ndnsf-spec189-r125/encrypted-repo` path after checking its available
capacity; no guard threshold is changed.

Raw test output is `.codex-tmp/spec189-r125-runner-path-tests.log`.

## r125 external encrypted-Repo MiniNDN resource boundary

r125 使用与 r124 相同的已安装 candidate、显式 `PYTHONPATH`、全新的 run
root，并把 run-scoped encrypted Repo 放到
`/dev/shm/ndnsf-spec189-r125/encrypted-repo`。因此它越过了 r122/r124 的
根盘 `diskFree` 边界：两 Provider 都完成 `READY` 和 signed
`ACK_DECISION`，并记录了 `NDNSF_DI_GRANT_VERIFICATION` 的
`boundary=BEFORE_ASSEMBLY`。Provider-0 还创建了 assembly staging 并取回
canonical ONNX；这确认外置路径修复确实把请求推进到了 assembly 边界。

原始运行随后由未修改的 host guard 停在
`RESOURCE_BOUNDARY:ownedSwap`。284 条样本中，`availableBytes` 最低为
`2282303488`，`diskFreeBytes` 最低为 `5001625600`，aggregate RSS 峰值为
`6933381120`，owned swap 峰值为 `302174208`，超过原有
`268435456` 限额；`swapIoDeltaBytes` 峰值为 `774483968`。cleanup 为
`PASS`，没有残留进程。Provider-0 的第一 assembly staging 只证明了
`canonical.onnx` 已取回，未形成 `RUNNER_READY`、`EXECUTION_COMPLETED`
或 `TERMINAL`；没有 requester `ACK_CLOSED`、有效 Selection、第二轮
request 或 C++ oracle 结果。因此这是外置存储后的 host swap 资源边界，
不是协议、模型或资格 PASS。

该次运行的 `supervisor.json`、resource stream、Provider logs 和完整
run root 保留在 `.codex-tmp/spec189-qwen-two-provider-20260918/runs/
two-provider-global-r125/`，启动输出为 `.codex-tmp/spec189-r125-launch.log`。
`/dev/shm` 上的 1.5 GiB ciphertext 仍占用 tmpfs 内存，不能作为最终
运行存储方案。后续 bounded retry 改用已核对 SHA-256 的 ext4 空间；不
提高 guard 限额，也不删除 r125 原始证据。

## Space-preserving artifact review after r125

对 24 个仍存在的 canonical initializer payload 路径逐一按
`sha256:413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`
核对，12 个原始 inode 全部匹配；随后将所有路径安全地改为 canonical
immutable initializer 的 hard links，原路径和字节保持不变。根 ext4 可用
空间由约 `5001428992` 增加到 `20034101248` 字节，最终 24 个路径共享同
一个 inode。该操作没有删除运行证据、源码、安装 binary 或 guard 配置。

## r126 ext4-backed MiniNDN memory boundary

r126 moved the run-scoped encrypted Repo to an empty ext4-backed directory
outside the run root. It crossed the r125 tmpfs-induced swap boundary: both
Providers reached `READY`, signed `ACK_DECISION`, and
`NDNSF_DI_GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY`; Provider-0 fetched
`canonical.onnx` into its assembly staging directory. The unchanged host guard
then stopped the run at `RESOURCE_BOUNDARY:MemAvailable` before
`RUNNER_READY`, execution, or terminal response.

The 299-sample resource stream recorded minimum available memory
`1174708224`, minimum disk free `16242237440`, maximum aggregate RSS
`8260157440`, maximum owned swap `105701376`, and maximum swap-I/O delta
`1005477888`. Cleanup passed and no process remained. The peak process sample
was dominated by the assembly worker at about `3900432384` RSS, its Provider
parent at about `1327280128`, and three NFD processes between about 0.8 and
0.9 GiB each. The first boundary is therefore host working-set pressure after
real Repo/fetch admission, not a protocol, model, or qualification PASS.

Raw launcher output is `.codex-tmp/spec189-r126-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r126/`;
the ext4 ciphertext directory is
`.codex-tmp/spec189-qwen-two-provider-20260918/external-r126/encrypted-repo/`.
The next bounded change is MiniNDN-only NFD CS sizing; guard limits, native
ownership, and production Repo/DI contracts remain unchanged.

## r127 MiniNDN NFD CS sizing gate

为避免每个 MiniNDN forwarder 的默认 65536-entry Content Store 各自保留
multi-GiB canonical/large-data 工作集，运行器把 NFD CS 限制为明确的
`MININDN_NFD_CS_SIZE=4096`。这只改变本地 MiniNDN 的缓存容量；认证 Repo
仍是 source of truth，NDN fetch/eviction 路径、生产 NFD 默认值、guard 和
native ownership contract 均不变。

`tests/python/test_spec184_qwen06b_local_experiment.py` 与
`tests/python/test_spec189_resource_guard.py` 共 `44` 项通过，runner
`py_compile` 通过，`git diff --check` 通过。没有任务 checkbox 变化；
T005/T006/T007/T009 仍为 `PARTIAL`。下一步是使用 ext4-backed encrypted
Repo、全新 r127 run root 和同一已安装 candidate 的 guarded MiniNDN 运行。

## r127 low-CS memory boundary

r127 使用 `MININDN_NFD_CS_SIZE=4096` 和 ext4-backed encrypted Repo。两
Provider 仍完成 `READY`、signed `ACK_DECISION` 以及
`GRANT_VERIFICATION=BEFORE_ASSEMBLY`，但 host guard 停在
`RESOURCE_BOUNDARY:MemAvailable`。299 条样本中 available memory 最低为
`1493688320`，disk free 最低为 `12460199936`，aggregate RSS 峰值为
`8255197184`，owned swap 峰值为 `238215168`，swap-I/O delta 峰值为
`960897024`；cleanup 为 `PASS`。峰值由 assembly worker 约
`6283599872` RSS 主导，说明 4096-entry CS 虽降低 NFD 常驻，却使 fetch
工作集放大，不能算作资源或协议 PASS。没有 `RUNNER_READY`、execution、
terminal、第二请求或 oracle 结果。

Raw launcher output is `.codex-tmp/spec189-r127-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r127/`.
下一次 bounded retry 使用中间 CS 容量，保留 guard、Repo/DI ownership 和
原始 r127 证据不变。

The next candidate changes only `MININDN_NFD_CS_SIZE` from `4096` to `32768`.
The Python/guard regression set remains the required runner gate before the
fresh r128 MiniNDN run; no native rebuild or guard-limit change is required.

## r128 tokenizer-digest invocation boundary

The first r128 invocation stopped in preflight with
`MODEL_TOKENIZER_DIGEST_MISMATCH` before MiniNDN startup. A direct read-only
hash check of the tokenizer selected by the stage manifest remains
`sha256:aeb13307a71acd8fe81861d94ad54ab689df773318809eed3cbe794b4492dae4`;
there is no provider, requester, protocol, resource, or model result from this
attempt. The preserved run root contains only the preflight supervisor receipt
and resource stream. This is an invocation boundary; r129 repeats the same
candidate with the exact verified tokenizer digest and a fresh run root.

Raw launcher output is `.codex-tmp/spec189-r128-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r128/`.

## r129 intermediate-CS memory boundary

r129 used the verified tokenizer digest, ext4-backed encrypted Repo, and
`MININDN_NFD_CS_SIZE=32768`. It reached the same real admission boundary as
the earlier runs: both Providers reached READY, signed `ACK_DECISION`, and
`GRANT_VERIFICATION=BEFORE_ASSEMBLY`; Provider-0 fetched `canonical.onnx` into
assembly staging. The unchanged guard then stopped at
`RESOURCE_BOUNDARY:MemAvailable` before `RUNNER_READY`, execution, terminal,
or second request.

The 298-sample stream recorded minimum available memory `1170264064`, minimum
disk free `8675835904`, maximum aggregate RSS `8783286272`, maximum owned swap
`105500672`, and maximum swap-I/O delta `740933632`. Cleanup passed and no
process remained. The peak was dominated by the assembly worker at about
`5636308992` RSS, with its Provider parent at about `1333473280`; NFD was
smaller than r126/r125 but the worker still retained the full inlined source
while building the inferred model. This is a native worker working-set
boundary, not a protocol or qualification PASS.

Raw launcher output is `.codex-tmp/spec189-r129-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r129/`.
The next changed gate is the reviewed C++ certified-chain ownership repair:
move the authenticated original into shape inference and retain only compact
certified-node bytes for S6 comparison.

## r130 certified-chain move repair gate

The C++ repair moves the authenticated `original` protobuf into the S5
`inferred` object after retaining only deterministic bytes for the certified
S6 node-cover comparison. This removes the prior deep copy of the full
inlined initializer without changing S1-S7 checks, recipe/node coverage,
identity digests, ORT loading, worker framing, or protocol contracts.

The affected targeted build completed `538/538`; the material consumer
selector passed `2/2`; the combined ONNX extraction, native assembly,
activation, and Repo publication selector passed `30` cases. Exact build and
installed SHA-256 matches were confirmed for the requester, Provider, worker,
oracle, and DI library; the installed `ldd` closure had no unresolved,
build-tree, or `.codex-tmp` dependency. Raw outputs are
`.codex-tmp/spec189-r130-targeted-build-move-original.log`,
`.codex-tmp/spec189-r130-material-selector-move-original.log`,
`.codex-tmp/spec189-r130-onnx-repo-selector-move-original.log`, and
`.codex-tmp/spec189-r130-runtime-identity.log`. These are focused/install
gates only; the next step is a fresh guarded MiniNDN run.

## r130 worker-digest invocation boundary

The first r130 MiniNDN invocation stopped in preflight with
`ASSEMBLY_WORKER_BINARY_DIGEST_MISMATCH` because the manually supplied worker
digest transposed characters. No MiniNDN process or request chain started.
The read-only build/installed worker digest is
`sha256:1a97d902e0c9a2e5d8acbaf34046a9a92ae6a77d9d494a27671b2deb75b3f4b6`.
This is an invocation boundary only; r131 obtains the exact installed hashes
from the verified files and uses a fresh run root.

Raw launcher output is `.codex-tmp/spec189-r130-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r130/`.

## r131 certified-chain source-resident resource boundary

r131 used the exact installed hashes derived from the verified runtime files,
the r130 certified-chain move repair, ext4-backed run-scoped encrypted Repo,
and a fresh run root. MiniNDN startup passed; both Providers reached READY,
signed ACK offers, and `NDNSF_DI_GRANT_VERIFICATION` at
`boundary=BEFORE_ASSEMBLY`. The unchanged host guard then stopped the run at
`RESOURCE_BOUNDARY:ownedSwap` before requester ACK/Selection. The 303-sample
resource stream recorded `availableBytes` minimum `1645875200`,
`diskFreeBytes` minimum `4884680704`, aggregate `rssBytes` maximum
`8443310080`, `ownedSwapBytes` maximum `405467136`, and
`swapIoDeltaBytes` maximum `968613888`. Cleanup was `PASS`; requester-0
recorded `NATIVE_REQUEST_STAGE_FAILED code=CANCELLED boundary=request`, and
there are no Selection, runner, execution, terminal, repeat-request, or oracle
records. This is a host/native worker working-set boundary, not a protocol or
qualification result.

The r130 move of `original` into `inferred` did not remove enough resident
source ownership. The next bounded repair releases and scrubs the worker's
request model/initializer buffers immediately after `ownedSourceModel()` has
returned the authenticated inlined model, before S4-S7 shape/extraction and
ORT work continue. No guard limit, MiniNDN cache policy, or protocol contract
changes.

Raw launcher output is `.codex-tmp/spec189-r131-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r131/`.

## r132 source-release compile boundary

The first targeted build of the worker-only source release stopped before
linking because the implementation called `scrub()` on
`NativeCanonicalByteBuffer`; the helper is defined only for
`MaterialPayload`. No selector, installed binary, or MiniNDN process used this
incomplete candidate. Raw output is
`.codex-tmp/spec189-r132-targeted-build-source-release.log`. The bounded fix
uses `OPENSSL_cleanse` on `initializerBytes->asVector()` before reset; the
same targeted build must pass before installation.

The paired r132 focused run used a non-existent Boost.Test filter for the
material consumer and stopped at setup with no matching test cases. The second
command entered the named ONNX extraction, native assembly, activation, and
Repo publication selectors and passed all `30` cases. Raw outputs are
`.codex-tmp/spec189-r132-material-selector-source-release.log` and
`.codex-tmp/spec189-r132-onnx-repo-selector-source-release.log`. Enumerate the
actual test names and rerun the missing material consumer selector; no runtime
or qualification status changes.

The exact material selector subsequently entered both cases but failed
`ExternalInitializerUsesBoundedChunksAndReassemblesAfterSelection`: payload
cleanup zeroed a shared initializer backing that the source still owned, so
the post-Selection round-trip comparison differed. The separate ONNX/Repo
selector passed all `30` cases. The bounded C++ repair changes `scrub()` to
leave shared backing/range views intact and clear only payload-owned bytes;
rebuild and rerun this selector before installation.

r132c passed the affected targeted build `538/538`, the exact material
consumer selector `2/2`, and the ONNX extraction/assembly/activation/Repo
selector `30/30`. The worker source-release and shared-backing cleanup gates
are focused C++ PASS only; T003/T005/T006/T007/T009 remain PARTIAL and no
MiniNDN qualification claim is made. Raw outputs are
`.codex-tmp/spec189-r132c-targeted-build-shared-scrub.log`,
`.codex-tmp/spec189-r132c-material-selector-shared-scrub.log`, and
`.codex-tmp/spec189-r132c-onnx-repo-selector-shared-scrub.log`. Next is exact
installed-target identity verification followed by a fresh guarded run.

The affected DI library, Provider, assembly worker, requester, and C++ oracle
were then installed from `build-spec189-oracle`. Every build/install SHA-256
pair matched exactly. The installed worker, Provider, requester, and oracle
`ldd` closure had no unresolved, build-tree, or `.codex-tmp` dependency. This
is an installed-runtime identity gate only; the next candidate is a fresh
guarded MiniNDN run with run root
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r132/`.
Evidence is `.codex-tmp/spec189-r132c-runtime-identity-v3.log` and
`.codex-tmp/spec189-r132c-ldd-closure.log`.

## r134 disk-free boundary and staging-artifact review

r134 restored mode `444` on the shared canonical initializer inode and entered
MiniNDN with a fresh run root. Both Providers reached READY, but the unchanged
host guard stopped at `RESOURCE_BOUNDARY:diskFree` before requester
ACK/Selection. The 39-sample stream recorded minimum available memory
`6469636096`, minimum disk free `3847790592`, aggregate RSS peak
`3527966720`, owned-swap peak `53248`, and swap-I/O delta `54505472`; cleanup
passed. No grant verification, ACK/Selection, assembly, runner, execution,
terminal, repeat request, or oracle record exists. This remains a host
artifact-placement boundary, not a protocol or qualification result.

Raw launcher output is `.codex-tmp/spec189-r134-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r134/`.
The next bounded step is exact digest verification of preserved large Repo
staging artifacts and byte-preserving hard-link deduplication of exact matches;
raw run paths and bytes must remain recoverable.

The r134 staging `.part` was independently verified as the canonical
initializer with SHA-256
`413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd` and size
`1503264768`, then atomically hard-linked to canonical inode `4377094`.
Exact-match r126/r127/r129 payload paths received the same path-preserving
operation. The r91 and r132 staging paths were not deduplicated because their
digests differed from the canonical object, and remain available as failure
evidence. The canonical inode has `84` links and host free space is
`9853657088` bytes. This does not change any task status; r135 must verify the
new staging/resource boundary with a fresh run root.

## r135 native stream-gap boundary

r135 crossed the staging disk gate and ran the exact installed native
candidate. Both Providers reached READY, emitted signed `ACK_DECISION` offers,
and recorded `GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`. The first native round
failed at the requester stream boundary with
`NATIVE_STREAM_FAILED boundary=stream` and Core message
`stream event gap exceeded retry budget`. The 348-sample resource record
ended in `drained`: minimum available memory `2122010624`, minimum disk free
`4563804160`, RSS peak `8165933056`, owned-swap peak `117878784`, and
swap-I/O delta `117878784`.

Provider-0 retained a protected assembly cache cipher and ORT profile, but
there is no authenticated Selection, complete assembly/runner, execution,
terminal response, repeat request, or C++ oracle result. The cache files are
diagnostic artifacts only and do not prove a protocol stage completed. The
first confirmed production boundary is Core's stream event gap after provider
grant verification. Raw launcher output is
`.codex-tmp/spec189-r135-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r135/`.
Next is a trace-enabled fresh run that records Selection-status queries,
authenticated progress, exact stream retry names/timeouts, and Provider
assembly events without changing limits.

Before the trace retry, the preserved r135 Repo payload was independently
verified as the canonical initializer digest and atomically hard-linked to
inode `4377094`; its original path and bytes remain, now mode `444`. The
canonical inode has `86` links and host free space is `7570694144` bytes. This
space-preserving operation does not alter the r135 stream boundary or any task
status.

## r136 trace run disk-free boundary

r136 used the trace-enabled command and started both Providers, but the
unchanged host guard stopped the fresh run at `RESOURCE_BOUNDARY:diskFree`.
The 283-sample resource record ended in `drained`: minimum available memory
`5972131840`, minimum disk free `3786682368`, RSS peak `4455481344`,
owned-swap peak `103866368`, and swap-I/O delta `103866368`; cleanup passed.
The requester trace recorded `43` stream-retry expressions and `42` timeout
callbacks for the selected Provider-1 event prefix. Provider logs had no
Selection-status/progress trace and only the pre-assembly grant boundary;
Provider-0 retained a `752097499`-byte staging `canonical.onnx` and root
metadata. There is no Selection/assembly completion, terminal response,
repeat request, or C++ oracle result. Raw launcher output is
`.codex-tmp/spec189-r136-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r136/`.
This is a host disk boundary that interrupted the trace experiment, not a
protocol result. Before r137, verify exact payload identity and preserve the
run; the next trace must add lifecycle markers without changing limits.

The preserved r136 Repo payload was independently SHA-256 verified as the
canonical initializer and atomically hard-linked to inode `4377094`; its
original path and bytes remain, now mode `444`. The shared inode has `88` links
and host free space is `5289746432` bytes. The non-matching-sized r136
`canonical.onnx` staging artifact remains untouched. This is space-preserving
evidence maintenance only; r137 still requires fresh trace and resource
evidence.

Before r137, the nine preserved `752097499`-byte `canonical.onnx` staging
paths were independently verified against digest
`sha256:258f367628b69a66eccf0973386b2eb80e2fce74cd64c2172d4f00a6e06abb94`.
The r112 path was retained as inode `4853426`; the other eight historical
paths, including r136, were atomically hard-linked to that inode. All paths
and bytes remain available, the anchor now has `9` links, and root ext4 free
space is `11306508288` bytes. No open deleted file was found. This is
space-preserving evidence maintenance only; r137 still requires fresh-run
trace and resource evidence and no qualification status changes.

## r137 native stream-gap boundary

r137 used a fresh run root, the unchanged installed candidate, the unchanged
resource/stream limits, and the prior selection/assignment diagnostics. The
parent command set `NDNSF_TIMELINE_TRACE=1`, but the maintained Qwen launcher
did not propagate timeline-trace variables through `env_for()`, so that
observation control was not active in child processes. The launcher was
repaired to forward `NDNSF_TIMELINE_TRACE`, its sample-rate companion, and
`NDNSF_STREAM_PACKET_TIMELINE_TRACE`; the Python syntax check passed.

The run passed MiniNDN startup, both Provider READY/ACK decisions, and both
`GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY` checks. Provider-0 reached
native assembly and retained a protected `752308868`-byte model cipher and a
`961277`-byte ORT profile. The requester recorded `89` stream-retry trace
lines for the Provider-1 event prefix and ended the first native round with
`NATIVE_STREAM_FAILED: stream event gap exceeded retry budget`. Provider-1
produced no Selection/assembly artifact. The 347-sample resource stream ended
`drained`: minimum available memory `2480660480`, minimum disk free
`6009753600`, aggregate RSS peak `8092606464`, owned-swap peak `197283840`,
and swap-I/O delta `590598144`; the host guard did not stop the run and
cleanup was `PASS`. There is no authenticated Selection, complete runner,
execution, terminal response, repeat request, or C++ oracle result. Raw
launcher output is `.codex-tmp/spec189-r137-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r137/`.
This is a native stream boundary, not a protocol/model qualification PASS.

## r138 trace-propagation disk boundary

r138 was a fresh run after the launcher repair and verified that
`NDNSF_TIMELINE_TRACE`, its sample-rate companion, and
`NDNSF_STREAM_PACKET_TIMELINE_TRACE` reached both Provider environments. It
passed MiniNDN startup, both Provider READY/ACK decisions, and both
`GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY` checks. Provider-0 fetched a
`752097499`-byte staging `canonical.onnx`; no authenticated Selection,
assembly completion, runner, execution, terminal response, repeat request, or
C++ oracle result was observed. The unchanged host guard stopped at
`RESOURCE_BOUNDARY:diskFree`; 287 samples recorded minimum available memory
`5828329472`, minimum disk free `3729842176`, aggregate RSS peak
`4453687296`, owned-swap peak `103759872`, and swap-I/O delta `296140800`.
Cleanup was `PASS`, and the requester recorded cancellation at the request
boundary. Raw launcher output is `.codex-tmp/spec189-r138-launch.log`; raw
run root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r138/`.
This is a host disk boundary before Selection, not a protocol/model
qualification PASS. The next changed gate is exact space-preserving
deduplication of the verified initializer and staging payloads, then a fresh
run with the same limits.

After r138, both r137/r138 canonical Repo initializer payload paths were
independently verified as digest
`sha256:413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`
and retained on initializer inode `4377094`, now with `93` links. The r138
`canonical.onnx` staging path was independently verified as digest
`sha256:258f367628b69a66eccf0973386b2eb80e2fce74cd64c2172d4f00a6e06abb94`
and retained on inode `4853426`, now with `10` links. All paths and bytes
remain available; no open deleted file was found, and root ext4 free space is
`7488299008` bytes. This is space-preserving evidence maintenance only and
does not change the r138 disk boundary or qualification status.

A further exact SHA-256 check found the r131 canonical Repo initializer
payload at the same digest; it was atomically hard-linked to initializer inode
`4377094`. The original r131 path and bytes remain, the inode now has `94`
links, and root ext4 free space is `8991244288` bytes. The r91/r122/r132
`.part` files had different digests and were deliberately left untouched.
This is space-preserving evidence maintenance only and does not change any
qualification status.

## r132 disk-free boundary and space-preserving artifact review

r132 entered MiniNDN and both Providers reached READY, but the unchanged host
guard stopped at `RESOURCE_BOUNDARY:diskFree` before requester ACK/Selection.
The 44-sample stream recorded minimum available memory `6371221504`, minimum
disk free `3850231808`, maximum aggregate RSS `3521368064`, owned swap `0`,
and swap-I/O delta `117432320`; cleanup passed. No grant verification,
Selection, assembly, runner, execution, terminal, repeat request, or oracle
result exists. Raw launcher output is `.codex-tmp/spec189-r132-launch.log`;
raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r132/`.

The candidate contained two independently verified, byte-identical
1,503,264,768-byte canonical initializer paths with SHA-256
`413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`.
`canonical-initializer.bin` and `canonical-repo-initializer.bin` retain both
paths, mode `600`, and now share inode `4377094`; no raw evidence or source
was deleted. Root free space increased by `1503268864` bytes. This is a
space-preserving artifact operation, not a runtime or qualification result.
The next fresh candidate uses run root r133.

## r133 immutable-source preflight boundary

The first r133 invocation stopped before MiniNDN with
`MODEL_CANONICAL_INITIALIZER_SOURCE_NOT_IMMUTABLE`. The earlier hard-link
operation had set the shared canonical initializer inode to mode `600`; the
launcher correctly requires the source to have no write bits. No process or
request chain started. Raw output is `.codex-tmp/spec189-r133-launch.log`.
The bounded repair restores read-only mode on the verified shared inode before
retry; no candidate bytes, guard limit, or protocol contract changes.

## r139 authenticated-Selection disk-free boundary

r139 used the repaired launcher, fresh run and external roots, the exact
installed native candidate, unchanged resource/stream limits, and
`NDNSF_NDN_LOG='*=TRACE'` with the existing Selection/status/timeline
diagnostics. The unchanged host guard stopped at `RESOURCE_BOUNDARY:diskFree`;
cleanup passed. The 315-sample record had minimum available memory
`2534084608`, minimum disk free `4123873280`, aggregate RSS peak
`8117018624`, owned-swap peak `156700672`, and swap-I/O delta `458199040`.
Raw launcher output is `.codex-tmp/spec189-r139-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r139/`.

The run crossed the authenticated Selection boundary. The requester emitted
`NDNSF_DI_NATIVE_ACK_CLOSED` and `NDNSF_DI_NATIVE_SELECTION_COMMITTED`; both
Providers accepted the signed Selection and recorded
`GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY`. Provider-0 emitted
placement-bound role validation, dependency preparation, assembly admission,
and `ASSEMBLY_STARTED`; both Providers emitted `EXECUTION_ENTERED`. The
requester received signed `SELECTION_STATUS_QUERY` replies from both
Providers. Provider-0 retained a `752097499`-byte staging `canonical.onnx`
and a `752308868`-byte protected assembly cipher.

No `RUNNER_READY`, `EXECUTION_COMPLETED`, terminal response, repeat request,
or independent C++ oracle result was recorded. The requester cancellation and
Provider-1 connection-reset shutdown followed the host guard stop. This is a
real Selection/assembly-admission/resource boundary, not a protocol or
qualification PASS; T005/T006/T007/T009 remain `PARTIAL`. The next changed
gate is bounded review of materialization/resource ownership before another
fresh guarded run, with no raised limit and no raw-evidence deletion.

After r139, the preserved Provider-0 staging `canonical.onnx` was independently
verified as digest
`sha256:258f367628b69a66eccf0973386b2eb80e2fce74cd64c2172d4f00a6e06abb94`
and atomically hard-linked to existing canonical staging inode `4853426`.
The r139 path and bytes remain available, the inode now has `11` links, and
root ext4 free space is `4875534336` bytes. The unique protected ciphertext
and raw logs were not changed. This is space-preserving evidence maintenance
only and does not change the r139 boundary or qualification status.

## r139 source-staging repair static re-review

The first read-only review of immutable snapshot
`.codex-tmp/spec189-r139-source-staging-review-v1/` returned
`NOT_STATIC_PASS`. It identified the missing `NativeModelRunner.hpp` dependency
in the snapshot closure, possible masking of the first assembly error when
`ProtectedRuntime::cancel()` throws, and a post-return artifact/cache owner
handoff gap plus silently unreported cleanup failures. No rebuild or rerun was
performed from that snapshot.

The bounded repair preserves the original exception while attempting protected
zeroization, carries protected artifact-directory ownership through
`NativeModelRunnerSpec::lifetime` into the Provider cache cleanup callback, and
emits `NDNSF_DI_PROVIDER_ARTIFACT_CLEANUP_FAILED` for best-effort owner/cache
cleanup failures. The corrected snapshot and read-only re-review remain open;
all native/product tasks stay `PARTIAL`.
