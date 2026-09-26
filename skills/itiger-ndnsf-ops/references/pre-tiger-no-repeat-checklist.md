# NDNSF-DI Tiger no-repeat checklist

Use this checklist immediately before every live Tiger submission. It is a
failure-prevention index distilled from Codex work history and then checked
against durable repository evidence. Conversation recollection identifies
candidates; repository records, executable checks, and current candidate
artifacts decide PASS/FAIL.

Spec175 uses one checked-in script/configuration tree for every gate. A run
record may select only the profile's registered, actually consumed deltas:
candidate/output/model identities and the stage-readiness device mapping.
Provider/GPU counts, seed, Slurm resources, working directory, helper paths,
and launch order are fixed by the profile and job files. If one of those values
must change, create a new profile/job and source seal; do not override it via
the environment or a second wrapper.

The experiment matrix is data, not a set of copied scripts. Use the same
repository launcher for every row:

```text
packaging/ndnsf-di-container/jobs/spec175/submit.sh \
  <gate> <proven-profile.json> <run-record.json>
```

Before any remote side effect, render the complete command and effective
environment, compare it with the proven profile, and run the negative mutation
tests. Reject unknown fields, unconsumed exports, helper/cwd/configuration
drift, direct `sbatch`, `--export=ALL`, and unallowlisted changes locally with
zero SSH, upload, staging, or scheduler effects. If source, dependency,
helper, configuration, or evidence-schema bytes change, invalidate the old
seal/SIF/results and restart at the earliest affected gate. Historical success
logs are diagnostic unless the complete source/profile/runner/SIF/gate/model
identity tuple is identical.

## One-page operator contract

Before preparing a run record, copy the profile's rendered configuration to the
evidence directory and verify this table. It is intentionally short so an
operator can use it before every experiment without reconstructing the whole
deployment from memory.

| Category | Fixed by the profile | Run-record delta allowed |
|---|---|---|
| Launcher | submit entry, helper/config hashes, Apptainer 1.5.3, `--cleanenv`, `--export=NONE` | none |
| Runtime | bundle `cwd`, mounts, identities/HOME/PIB/TPM, routes, readiness, cleanup | none |
| Workload | Provider/GPU count, topology, seed map, timeouts, memory/walltime, model runtime | none, except the registered gate case/seed map for local G3 |
| Identity | source seal, profile digest, runner/config/SIF/model digests | candidate/run/output and model/artifact identities only |
| Stage placement | stage-to-device mapping | stage-readiness only, and only if the tracked wrapper consumes it |

If a requested change is not in the final column, stop. Create a new profile and
source seal; do not add an environment variable, edit a `.sbatch` file in the
staging directory, or start another job. A variable counts as consumed only if
the checked-in wrapper parses it, validates it, emits it in the effective
configuration, and repeats it in the result manifest.

Run the validator after all required evidence has been produced:

```bash
python3 ~/.codex/skills/itiger-ndnsf-ops/scripts/validate-pre-tiger-checklist.py \
  --manifest /absolute/path/pre-tiger-checklist.json \
  --gate control \
  --output /absolute/path/pre-tiger-checklist-validation.json
```

The validator hashes the exact SIF and every listed evidence file. Run it once
per submission identity; retain its JSON instead of repeatedly rehashing the
same multi-gigabyte SIF in ad-hoc commands.

For Spec175, the candidate-closure validator is the first repository-owned
check, before model preflight, checklist processing, command discovery, SSH,
upload, remote mutation, or `sbatch`:

```bash
packaging/ndnsf-di-container/bin/spec175-candidate-closure \
  --manifest "$CLOSURE_MANIFEST" --gate G4T \
  --expected-sif "$SIF" --expected-sif-sha256 "$SIF_SHA256" \
  --output "$(dirname "$CLOSURE_MANIFEST")/candidate-closure-validation.json"
```

It requires the seven candidate-tuple digests, the declared ownership-plane
transition and restart gate, all G0--G4 terminal manifests, and one bound file
for every executable/configuration closure kind. A missing or changed input is
a local rejection; do not discover it through a Tiger allocation.

The candidate closure is necessary but not sufficient. The repository's
Spec175 launch validator must also render the complete command and sealed
environment from the checked-in proven Tiger profile, compare every field, and
produce the hash-bound `proven-baseline-exact-delta` report. The operator
checklist only verifies that report's presence and digest; it must never be
replaced by a prose or synthetic `PASS`. Direct `.sbatch`, ad-hoc wrappers,
ambient variables, and `sbatch --export=ALL` are rejected before any external
side effect.

Every accepted case result must contain
`ndnsf-di-spec175-terminal-evidence-v1`, with `status=PASS`, a result written
after all owned children exited, complete child exit codes, no unexpected
signal/abort, and an empty `survivingOwnedProcesses` list. The harness's
explicit bounded SIGINT shutdown, and any bounded force-termination after the
graceful-drain window are recorded as intentional teardown. A success marker,
Response, benchmark line, or expected negative without this record remains
incomplete evidence.

## Historical failure registry

| Class | Previously observed failure | Required prevention |
|---|---|---|
| Release-route drift | Docker, OCI, remote materialization, temporary SIFs, and local complete SIFs were mixed into one workflow. | Normal route is one complete locally built application SIF; Tiger verifies, stages once, and executes only. |
| SIF-only release identity | A physically valid SIF was combined with a repaired host runner, submit bundle, model/config, or lower-gate PASS from another subject. | Treat source seal, exact SIF, host replay, submit tree, effective config, model artifacts, and validation contract as one immutable tuple; record plane invalidation and restart gate. |
| Wrong build ABI | Host `Python.h`, host venv/site-packages, or a host-built `_ndnsf.so` was proposed or copied into a Python-3.10 SIF. | Container-bound native code is built inside the sealed SIF builder; compare build/runtime Python, SOABI, extension suffix, compiler, glibc, hashes, RPATH, and `ldd`. |
| Partial link repair | `di-native-plan-onnx-smoke` was fixed while sibling `di-native-onnxruntime-smoke` still lacked NDN-CXX/NDN-SVS dependencies. | Enumerate and link the complete Provider/ONNX target census; rerun the full build after any target fix. |
| Host linker contamination | Linuxbrew `ld` selected unrelated system GTK/UAV libraries. | Record the compiler/linker roots and require the sealed container toolchain plus complete library-lock closure. |
| Stale but readable SIF | Import or `READY` passed using a SIF whose source/native hashes did not match the checkout. | Bind source, locks, definition, build record, SIF and bundle to one candidate; physical readability never substitutes for identity equality. |
| Post-seal source change | The Provider lazy-load correction fixed the `206918` crash boundary, but the previously qualified SIF still contained the old Provider code. | Recompute the source seal after every source, host-runner, packaging-validator, lock, or definition change. Reject the old SIF and its G0--G4 evidence before any new Tiger submission. |
| Missing staged helper | Job `206904` reached Tiger without `run-qwen-stage-readiness.py` in the staged submit tree. | Resolve every transitive helper from the selected wrapper, bind its source and staged SHA-256, require executable/readable remote presence, and fail before SSH/upload or `sbatch`. |
| Submit environment-name mismatch | Job `206794` exported `REMOTE_MODEL_ROOT`, while the SBATCH script required `SPEC175_REMOTE_MODEL_ROOT`. | Run the selected submit/wrapper chain with `set -u` in a process-only preflight and compare every producer variable with the exact consumer name. |
| Controller readiness before service start | Job `206795` used a Controller wrapper that never called `controller.start()`, so no Controller route became active. | Run the repository functional-bundle preflight; require `controller.start()`, no competing `run()`/`start_background()`, readiness after start, and a live event loop/route after the probe. |
| Implicit CPU-memory request | Job `206905` relied on Slurm's small default memory and was OOM-killed before stage readiness. | Render and inspect the actual SBATCH request. Use the qualified 96-GiB envelope for the three-stage Qwen3.6-27B readiness subject unless a new measured envelope is explicitly qualified. |
| Bundle/cwd mismatch | Job 197169 reached a healthy CUDA Provider but relative artifacts resolved outside the bundle. | Verify mounts, every artifact and sidecar, and require every Provider launch to `cd "$BUNDLE"`. |
| Identity isolation/bootstrap | Shared or incorrectly mapped HOME/PIB caused locks, empty permissions, NFD failures, or certificate bootstrap decryption failure. | Use `apptainer --home` with distinct HOME/PIB/TPM, ordered Controller/bootstrap/User readiness, controller certificate, bootstrap tokens, and a nonempty permission snapshot. |
| Bootstrap-token set mismatch | The Controller generated tokens for only Controller/User/Provider identities, while the multi-Provider wrapper unconditionally extracted missing repository tokens under `set -e`; the wrapper exited, its Face/routes vanished, and Providers reported Nack 150/`NoRoute` before any request. | Derive the required identity set from policy plus every wrapper child command; compare it with the generated token file before launch. Require every extraction to be classified, keep the Controller alive after the route probe, and assert route/Face presence before Provider bootstrap. |
| Shared HOME/PIB after identity repair | Job `206917` fixed the policy identities but all children still used `/evidence/home`; one Provider failed with `PIB database cannot be initialized: database is locked`. | Require a statically validated, unique HOME/PIB assignment for Controller, User, every Provider, and every repository/probe child before allocation; reject shared paths even when the token set is complete. |
| Provider pre-ready native crash | Job `206918` used unique HOME/PIB paths but all three Qwen ONNX Providers segfaulted after `constructor_done`, before `LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY`. | Run the exact-SIF one-stage ORT probe and the three-process ORT probe against the same model root before another Provider allocation. If both pass, classify the remaining fault as NDNSF Provider initialization/preload integration and require a source-level regression/reseal. |
| Readiness false positive | Providers emitted `READY` while the request, Selection, response, imports, or permission path later failed. | Require the complete Request -> ACK closure -> provider-specific Selection -> execution -> Response lifecycle and every process exit. |
| Success marker without child exit | Current host G3 M09-r3 wrote `NDNSF_DI_SPEC175_REPO_PUBLISH_PASS` and all four stage receipts, but the publisher child remained alive; the parent timed out in `publisher_proc.wait()` and no case-result was emitted. | Require a zero publisher exit, a newly written case-result, and clean controller/repository teardown after every success marker. If PASS is present but the child is alive or the result is missing, classify the run as incomplete harness/lifetime failure, preserve it, and block aggregation. |
| Publisher close regression | The repaired M09-r4 passed only after `spec175_repo_bootstrap.py` explicitly closed the native ServiceUser/Face in an idempotent `finally` path before the success marker. | Keep explicit close in both success and exception paths; test close idempotence; require the driver to observe close, zero child exit, fresh case-result, and no remaining publisher/controller child before counting the repetition. |
| Normal-case native teardown abort | Exact-SIF M01-r2 and M03-r3 received a valid Response and benchmark summary but then printed `terminate called without an active exception`, exited `-6`, and emitted no case-result. | Do not count a response or benchmark line as PASS. Reproduce normal M01/M03 teardown, join every native Face/client/worker before process exit, release the Python GIL while a pybind11 stop/shutdown call joins native work, add an idempotent shutdown regression, and rerun the complete G0--G3/SIF/G4 chain after any repair. |
| Repository publication coverage race | Exact-SIF M13-r2 failed before the pipeline request with `repo-store-insufficient-cover: candidateCount=0 successfulCount=0 eligibleCount=0 requestedReplicas=1`; the repository later accepted a request after the publisher's ACK window had expired. | Verify a committed catalog and nonempty eligible coverage, then run a bounded non-mutating request/ACK round trip from the same publisher identity over the exact NFD route. Record request ID, provider, route/Face, send/ACK timestamps, and probe exit before releasing the publication barrier. A permission/registration marker, fixed sleep, or longer artifact timeout is insufficient. Preserve the failed trace; require the live probe, publisher zero exit, coverage evidence, and a fresh case-result before counting the repetition. |
| Repository service route readiness | Repository permission and registration markers were present, but the first real publisher request reached the Provider only after the one-shot ACK window closed. | Separate local visibility from network readiness. Require an explicit unique service-level probe that receives a validated repository ACK before publication; classify a timeout as `REPO_SERVICE_ROUTE_NOT_READY` and stop before mutating the store. |
| Pre-network rejection misclassified as missing-handle failure | M13-r1 rejected a forged checkpoint in the User coordinator before native Request creation; the generic negative cleanup assertion then failed because no invocation handle existed. | Label each negative as pre- or post-network. For pre-network rejection require no native invocation, events, or completion and do not cancel; for post-network failure require exactly one native cancel, child teardown, and a case-result. |
| Wrapper/status loss | A trailing shell command, missing `exec`, unsupported timeout/function combination, or aggregate grep hid child failure or leaked Providers. | Syntax/dry-run every wrapper; record each child status and marker; require zero remaining job-local Providers. |
| Wrong substrate layer | MiniNDN/NLSR/OVS checks were incorrectly required inside the SIF, or host applications were counted as exact-SIF evidence. | Host owns topology/namespaces/routing; exact SIF owns NFD and NDNSF applications. Tiger runs neither MiniNDN nor an image build. |
| Apptainer drift | Login-node, compute-node and local Apptainer versions or HOME semantics differed. | The allocated compute node is authority; require local/target semantic version parity and explicit Apptainer path. Current qualified pair is 1.5.3. |
| Resource envelope | Job 182780 verified an 18.53-GB transfer and then hit a memory-cgroup OOM; SIF materialization also previously exited 137. | Before allocation, bind requested memory, projected peak CPU RAM/GPU VRAM, SIF/model bytes, scratch capacity, fsync probe and walltime; retain OOM as resource evidence, not protocol evidence. |
| Disk duplication | Repeated SIF/model/intermediate copies exhausted local or project space. | Content-address external models, retain one active SIF, enumerate exact disposable paths, and verify free space before building/uploading. |
| Model/runtime drift | Qwen3 configuration was paired with unsupported Transformers versions, while the deployment design requires ONNX Runtime only. | Freeze canonical ONNX/tokenizer/adapter hashes, run architecture conformance in the exact SIF, forbid runtime PyTorch/Transformers, and require the selected ORT provider. |
| CUDA false positive | `--nv` or `nvidia-smi` passed while ORT used CPU fallback. | Require allocated GPU UUID mapping, ORT CUDA provider evidence and `cpuFallback=false`. |
| Route/dataflow stall | A Provider could be ready while `backbone-to-head0` or another dependency was never published/fetched. | Validate the exact prefix routes/strategies and require producer/fetch/consumer evidence for every planned dependency edge. |
| Retry hid native failure | Spec175 M09 produced `returncode=-11`, followed by a passing isolated rerun. | Preserve both records; classify the signal, add a focused regression, and reseal lower gates. A later PASS never erases the first native exit. |
| Historical-success drift | New launchers kept the same names but changed effective `cwd`, helper bytes, inherited environment, mounts, or rendered Slurm command relative to successful Jobs 189483, 201039 and 201045/201046. | Render from one checked-in proven profile, compare every field, allow only registered gate parameters, submit the exact validated bytes, and first run a bounded current-SIF one-node control before model staging. |
| Evidence-boundary inflation | CPU M01 `distributed_ms`, ORT session probes, launch failures, and stage-readiness results were at risk of being described as GPU prefill/decode or G6 functional evidence. | Every result record must name its gate, real components, excluded claims, and terminal child statuses. Diagnostic probes never satisfy a later functional/performance gate. |
| Stateful export contract drift | Qwen3.6-27B export attempts used derived `head_dim`, external data outside the graph directory, non-tensor `Sequence` values, wrong state-axis order, or a device tensor where the exporter expected CPU NumPy data. | Freeze the model configuration and canonical state vectors; validate graph-by-filename with colocated external data, tensor-only value types, per-stage numerical prefill/decode parity, and CPU/device state separation before promotion. |
| Reference-token oracle drift | Job `206773` compared the CUDA chain with the staged Transformer second token (`7812`) instead of the first token actually produced by the canonical ONNX Runtime chain (`96445`). | Generate `referenceTopToken` from the same CUDA-ORT path used by readiness; retain offline Transformer tokens as diagnostics only and fail on an unexplained mismatch. |
| Non-atomic artifact promotion | Jobs `206563`/`206666` exposed `.partial` roots, absolute paths, incomplete ledgers, or quota failures after export had already passed. | Promote into a new final content-addressed root only after all files and sidecars exist; rewrite manifests/ledgers to final root-relative paths, verify every entry, then remove named intermediates. |
| Slurm export-value encoding | Job `206740` passed `SPEC175_STAGE_DEVICE_IDS=0,1,2` through comma-separated `--export`, so the runner received only `0`. | Do not put comma-bearing values in the export list; use the sealed default or a validated environment file and inspect the rendered environment before allocation. |
| Duplicate/pending subject confusion | Alternative readiness jobs were submitted while another subject was pending or later canceled, risking duplicate allocations and false repetition counts. | Maintain one active terminal subject per candidate/gate; cancel or classify pending alternatives explicitly, and never count pending/canceled jobs as repetitions or PASS evidence. |
| Source-seal/archive closure mismatch | The current G0 seal includes a modified in-scope packaging checklist, but the prepared SIF `workspace.tar` omits that path; strict host-source validation correctly stopped the build before producing a candidate. | Before local SIF build, diff the G0/G3 dirty source paths against the archive file list. Embed runtime-owned files or explicitly classify host-only files; regenerate source/lower manifests after changing either boundary. Never disable strict source matching. |
| Mixed host/runtime packaging subtree | Treating the whole `packaging/ndnsf-di-container/` prefix as host-only can silently omit a replay driver or workload that the exact-SIF gate needs. | Classify by exact path, not directory prefix: host-only validators/checklists may be excluded, but runtime-owned replay helpers and workloads must be archived and hash-checked. Any unclassified omission remains a hard failure. |
| Timeout-terminal teardown abort | Exact-SIF G4 case `M08-r2` returned 134 (`SIGABRT`) after the expected `EventTimeout`, printed `terminate called without an active exception`, and emitted no case-result file. The public stream was already terminal, so a naïve `cancel()` returned without canceling the native request. | Treat the signal and missing result as a runtime lifecycle failure, not an expected negative. Require failed-handle cancellation to post exactly one native cancel even after `_fail()`; release the Python GIL while pybind11 joins native Face work; synchronously join `client.shutdown(wait=True)` before the case returns; retain a focused idempotence/teardown regression and rerun the complete M08 triplet before G4 promotion. |
| Smoke result overclaim | An exact-SIF M01 smoke completed several Provider events and printed `distributed_ms`, but the record did not establish GPU/ CUDA-ORT use or distinguish prefill from decode. | Label every smoke with model, execution provider, GPU UUID, token count, and gate. Require CUDA-ORT plus allocated-GPU evidence for a GPU claim; require at least two generated tokens and separate prefill/decode markers before reporting LLM timing or throughput. |
| Candidate-plane drift | Runtime SIF, host replay harness, and Tiger submit bundle were repaired independently, then mixed with stale hashes or stale lower-gate evidence. | Record `changedPlane` (`sealed-runtime`, `host-replay`, or `tiger-submit`), old/new identity, invalidated gates, and restart gate. A submit-only change may reuse the SIF only when it is explicitly outside the sealed planes; runtime/host-gate changes require fresh source/lower evidence. |
| Duplicate/stale replay identity | Two G4 directories carried different source/SIF identities and statuses (`42/42 PASS` for an older candidate versus `41/42` with `M08-r2 SIGABRT` for the current candidate). | Bind every report to candidate/source-seal, exact SIF, manifest, and effective-configuration digests. Select one active terminal manifest; reject cross-candidate prose, copied PASS files, and contradictory status summaries. |
| Missing current-candidate audit | An operator saw an older 42/42 replay or Tiger control and treated it as authorization after the source/SIF changed. | Before submission, write a candidate-bound audit record naming the four identity digests, terminal G0--G4 statuses, exact failure boundary/owner, and restart gate. Any missing field, stale lower-gate result, or prose/manifest contradiction blocks the submission. |
| G3 repetition-manifest key collision | The current 42-process host matrix was complete, but the manifest generator rejected three repeated `--run-root M01=...` entries as `invalid or duplicate case`; the repetition layout was supplied with the wrong argument shape. | Supply exactly one unique `CASE=DIRECTORY` per M01--M14. The directory must contain `r1`/`r2`/`r3`, or be a `{rep}`/`{index}` template. Validate manifest status, case set, repetition count, source seal, topology, and all case-result hashes before G4. |
| G3 seed/campaign drift | A complete 42-process host matrix passed, but each repetition used a different ad-hoc seed; the strict manifest correctly rejected the subject because Spec175 fixes workload seed `1750001` and fault seed `1750002`. | Derive the seed from the fault map before launch: M01--M04 and M10--M14 use `1750001`; M05--M09 use `1750002`. Require `seed` and `campaignId=spec175-Mxx-<seed>` in every result before aggregation. Preserve varied-seed runs as diagnostics only and rerun a fresh 42-process fixed-seed matrix. |
| Exact-SIF replay seed drift | The first G4 candidate replay passed `1750001` to every case; M05--M09 therefore did not run their registered fault subject, and the replay was interrupted before a terminal manifest. | The replay driver must derive `1750002` for M05--M09 from the fixed fault map, record each effective seed, and expose workload/fault seeds in the manifest. Reject the partial output root and start a fresh exact-SIF replay after the driver/test contract passes. |
| Exact-SIF repository route race | The corrected fixed-seed replay reached 41/42; `M12-r2` received the Repo request but emitted no ACK and timed out in the pre-publication route probe, while a fresh same-input M12 rerun passed. A later host `M10-r2` proved the same one-way symptom: every STATUS Request reached and validated at the Repo Provider, but the newly constructed publisher User received no ACK. | Preserve the failed trace. Treat the repository publisher as a fresh SVS User: construct its `ServiceUser`, apply the fixed post-join initial-Sync interval, and require `REPO_USER_SVS_SETTLED -> route probe PASS` before mutation. Then rerun the complete 42-entry matrix from a fresh root; never splice an isolated pass into the failed aggregate. |
| Provider readiness or misleading log order | M14-r1 in the 2026-08-30 host matrix reached the first request with only three Providers. A merged log appeared to put `LLM_PIPELINE_PROVIDER_READY` before native registration, but Python stdout and ndn-cxx logs use different streams, so line order is non-authoritative. | Require `ServiceProvider.start()` (or an equivalent synchronous seam) to register handlers and start the native event loop before READY is emitted. Prove readiness with the call-return barrier or a separate machine-readable record for every Provider; never infer it from merged stdout/stderr order. Treat an actual early marker as a launch-order failure, rerun the affected case from a fresh root, and do not count the Provider as ready. |
| Incomplete SVS group fanout | Current-source probes missed first-window ACKs even when the group prefix and multicast strategy appeared on every node. Exact comparison later showed router `a` could retain the group prefix while omitting one member face, so the prefix-only snapshot was a false PASS. | Enumerate the actual SVS member set. Require one distinct anchor-router group next hop for every member and one reverse member-to-anchor route; record face IDs and fail on any missing expected next hop. Then apply the fixed post-join initial-Sync interval and verify low-volume Request/ACK lifecycle closure without changing timeouts. Use `NDN_LOG='*=WARN:ndn_service_framework.TimelineTrace=WARN:ndnsf.di.RuntimeEvidence=WARN'`; global TRACE is bounded diagnostic-only. Any launcher/source correction requires a fresh G0 and downstream chain. |
| Machine-readable evidence split by merged streams | A real M01 business flow passed, but a Provider timing number was corrupted when an ndn-cxx WARN line entered a multi-insertion `std::cout` record after stdout/stderr were merged. | Build each evidence record completely, then submit it once through one `NDN_LOG_*` component. Keep `NDN_LOG_NOFLUSH` unset, reject every malformed marker, and run a focused real MiniNDN case before resealing. Never make the parser truncate or guess a damaged numeric field. |
| Route-probe retry identity drift | A transient repository startup race can tempt an operator to reuse one request ID or splice a later pass into a failed repetition. | Permit only a bounded initial-attempt-plus-two-retry budget; generate a fresh request ID per attempt, retain all attempt reports, and release publication only after a validated ACK. A retry never replaces a failed matrix repetition. |
| MiniNDN/Mininet startup stall | A local M13 repetition once timed out during host configuration after an interrupted run, while another same tuple reached the route probe and a later clean run passed. | Before each root process assert no owned `mnexec`, NFD, replay process, or namespace remains. After external termination run scoped `mn -c` and retain its output. Classify topology/host-configuration stalls and pre-publication route-probe timeouts as setup failures; retry at most once with the identical profile/seed/topology/timeouts/workload in a new empty directory. Never splice that retry into the failed directory or change protocol parameters. |
| Unreadable result tree | Recursive evidence inspection encountered root-owned state directories and `Permission denied`, which can be mistaken for missing or failed case results. | Create the G3 output tree with a readable owner/group and verify runner-log/case-result readability before aggregation. Any unreadable path, missing runner log, or missing case-result is an evidence-boundary failure; preserve the record and block G3/G4 until access is repaired. |
| Host substrate preflight run unprivileged | `spec175-host-substrate-preflight` returned `HOST_NAMESPACE_PRIVILEGE_MISSING` when invoked as a normal user, before any replay or SIF execution. | Treat this as a preflight-context failure, not a runtime result. Re-run the identical command in the authorized root context with one-line `sudo -E env ... python3 ...`; do not enter G4 or submit to Tiger while the host-substrate JSON is FAIL. Keep the failed JSON and the authorized PASS JSON separately. |
| Current exact-SIF replay incomplete | The 2026-08-30 candidate-bound G4 manifest is FAIL (38/42): M01-r2 and M03-r3 abort after a valid Response, M08-r2 times out during terminal cleanup, and M13-r2 has zero eligible repository candidates before the request. | Use the terminal manifest as authority. Preserve all four traces, repair each owner separately, require a fresh case-result plus zero child exits, and regenerate the complete G0--G4 chain after any source/runner/repository/teardown change. Never merge an older 42/42 replay or replace one failed result file. |

Durable examples are under:

- `specs/170-reusable-layer-artifacts/evidence/gpu-build-failure-20260813.md`
- `specs/170-reusable-layer-artifacts/evidence/spec170-full-build-runs-20260816.md`
- `specs/170-reusable-layer-artifacts/evidence/current-sif-r23-local-20260819.md`
- `specs/168-itiger-di-deployment-fidelity/evidence/tiger-large-single/182780-v93-qwen36-large-single-oom.md`
- `specs/175-ndnsf-di-streamed-invocation/evidence/t023-g4-failure-20260825l.md`
- `specs/175-ndnsf-di-streamed-invocation/evidence/t022-g3-current-20260825n.md`
- `specs/170-reusable-layer-artifacts/evidence/tiger-d0.md`
- `specs/170-reusable-layer-artifacts/evidence/tiger-d2b.md`
- `specs/170-reusable-layer-artifacts/evidence/tiger-d2h.md`
- `specs/175-ndnsf-di-streamed-invocation/evidence/t026-tiger-failure-206910-206915.md`
- `specs/175-ndnsf-di-streamed-invocation/evidence/t026-ort-native-probes-206919-206920.md`
- `specs/175-ndnsf-di-streamed-invocation/evidence/t025-g5-export-failure-20260828.md`
- `specs/175-ndnsf-di-streamed-invocation/evidence/tiger-stage-readiness.md`

## Current Spec175 incident-to-gate map

Use this table when reviewing a new candidate. A repeated incident means the
named row was either absent, stale, or accepted with evidence that did not
exercise its stated oracle.

| Job(s) | Observed boundary | Checklist row that must block recurrence | Required executable oracle |
|---|---|---|---|
| `206794` | SBATCH consumed an unset `SPEC175_REMOTE_MODEL_ROOT`. | `submit-env-contract` | Process-only `set -u` launch of the exact submit/wrapper chain; producer and consumer variable names must match. |
| `206795` | Controller wrapper never activated the Controller route. | `controller-start-liveness` | Repository functional preflight plus a bounded liveness probe after `controller.start()` and after route registration. |
| `206904` | The staged tree omitted the stage-readiness runner. | `submit-tree-helper-closure` | Resolve transitive helpers, compare source/staged hashes, and assert remote readable/executable presence before allocation. |
| `206905` | Implicit CPU memory caused cgroup OOM. | `resource-envelope` | Compare tracked SBATCH text, rendered submission, `scontrol` request, and measured peak; current qualified readiness envelope is 96 GiB. |
| `206910`, `206915` | Missing repository bootstrap identities killed the wrapper and removed Controller Face/routes. | `functional-bundle-identity-closure` | Repository functional preflight plus identity-set closure; assert token extraction completes and Controller/route remain live. |
| `206917` | Shared `/evidence/home` locked the PIB database. | `isolated-home-pib-bootstrap` | Statically enumerate every concurrently launched child and reject any shared HOME/PIB/TPM path. |
| `206918` | APPProvider crashed before stage-ready during eager Qwen session preload. | `provider-pre-ready-lifecycle` | Source regression for lazy load, new source seal/SIF/G0--G4, then bounded APPProvider readiness with all child exit codes. |
| `206919`, `206920` | One-stage and three-process exact-SIF CUDA ORT session probes passed. | `onnx-native-session-probe`, `result-boundary-label` | Preserve both PASS records as runtime-boundary evidence only; they do not satisfy APPProvider, request, prefill/decode, G6, or throughput. |
| `M08-r2` | Expected `EventTimeout` was followed by SIGABRT/134 and no case-result file because the terminal public handle did not cancel the still-live native stream. | `negative-case-oracle`, `wrapper-config-child-status` | Require one native cancellation after failure, synchronous client/Face teardown before process return, a recorded terminal result, and zero signal exit; do not let the expected timeout marker erase the abort or permit G4/Tiger promotion. |
| `M09-r3` | Repo publisher emitted `PUBLISH_PASS` and four stage receipts but did not terminate; the host driver timed out waiting and produced no case-result. | `wrapper-config-child-status`, `result-boundary-label` | Assert publisher `returncode==0` after the PASS marker, require the case-result before accepting the repetition, and capture bounded teardown diagnostics. A marker alone never qualifies a case. |
| `M13-r2` | Repository request/ACK route converged after the publisher's one-shot ACK window, yielding zero eligible candidates before the pipeline request. | `repository-service-route-readiness`, `functional-bundle-identity-closure` | Verify catalog/coverage and complete a non-mutating same-identity service ACK probe before releasing the publication barrier; retain route/Face and send/ACK timestamps, then rerun the M13 triplet. |
| `206563`, `206666` | Export/promotion passed computation but left temporary roots or failed on quota while copying external data. | `promotion-hash-config-delta`, `resource-envelope` | Require atomic final-root promotion, root-relative ledger verification, quota/write probe, and durable before/after manifests. |
| `206740` | Comma-bearing stage-device IDs were split by Slurm `--export`. | `submit-env-contract`, `wrapper-config-child-status` | Render the exact environment and parse the runner's device list before allocation; prefer the sealed default over comma-bearing export values. |
| `206749` | Absolute artifact-ledger paths failed after content-addressed node-local staging. | `bundle-cwd-artifact-mount`, `promotion-hash-config-delta` | Verify the ledger against the final cache root using root-relative paths and a fresh node-local copy. |
| `206773`, `206778` | Frozen-token field came from the wrong reference stage/token. | `onnx-model-runtime-compatibility`, `result-boundary-label` | Derive the oracle from the same canonical CUDA-ORT chain as readiness; retain offline Transformer values as diagnostics only. |
| pending/canceled readiness alternatives | Multiple scheduler alternatives risked being treated as repetitions. | `wrapper-config-child-status`, `result-boundary-label` | Keep one active terminal subject; classify pending/canceled jobs explicitly and exclude them from repetition counts. |
| duplicate/stale G4 manifests | An older exact-SIF replay remained PASS while a later archive-boundary replay for a different candidate failed M08-r2. | `release-identity`, `result-boundary-label` | Compare candidate/source-seal, SIF, manifest, and effective-configuration digests; use only the active candidate's terminal manifest, and let manifest status override prose. |

The successful current-SIF control `206901` and three-stage readiness `206907`
are prerequisites, not waivers. They remain usable only for the exact source
and SIF identity they qualified; a post-seal Provider or launcher change makes
their promotion authority stale.

## Manifest contract

The manifest schema is `ndnsf-itiger-pre-submit-checklist-v1`:

```json
{
  "schema": "ndnsf-itiger-pre-submit-checklist-v1",
  "gate": "control",
  "candidateId": "spec175-candidate-example",
  "sif": {
    "path": "/absolute/path/runtime.sif",
    "sha256": "sha256:..."
  },
  "checks": {
    "release-identity": {
      "status": "PASS",
      "evidence": [
        {"path": "/absolute/path/source-seal.json", "sha256": "sha256:..."}
      ]
    }
  }
}
```

Relative evidence paths resolve from the manifest directory. SIF and evidence
digests must be lowercase SHA-256, with or without the `sha256:` prefix.

### Required for every `control` submission

- `candidate-source-freshness`
- `candidate-closure-manifest`
- `candidate-invalidation-matrix`
- `proven-baseline-exact-delta`
- `release-identity`
- `local-sif-route`
- `cluster-substrate`
- `target-apptainer-parity`
- `container-abi-provenance`
- `complete-target-link-closure`
- `exact-sif-library-entrypoint`
- `submit-tree-helper-closure`
- `submit-env-contract`
- `bundle-cwd-artifact-mount`
- `isolated-home-pib-bootstrap`
- `controller-start-liveness`
- `lower-gates-native-exits`
- `wrapper-config-child-status`
- `resource-envelope`
- `result-boundary-label`
- `promotion-hash-config-delta`
- `credential-secret-scan`
- `predispatch-no-side-effects`

The control job itself must then produce a durable lifecycle result. It is not
valid merely because its pre-submission manifest passed.

### Additional requirements for model/GPU submissions

- `current-sif-tiger-control`
- `onnx-model-runtime-compatibility`
- `onnx-native-session-probe`
- `cuda-no-fallback`
- `routes-stage-dataflow`
- `provider-pre-ready-lifecycle`

Functional and performance submissions additionally require:

- `functional-bundle-identity-closure`
- `repository-service-route-readiness`

The existing rows above also carry the following Spec175 subchecks; do not
replace them with a prose PASS:

- `candidate-closure-manifest`: source seal, exact SIF, host replay, submit
  tree, effective workload/configuration, model artifacts, validation contract,
  and terminal lower-gate manifests all name one candidate.
- `candidate-invalidation-matrix`: `changedPlane`, old/new identity,
  invalidated gates, and earliest restart gate are explicit; no stale PASS is
  silently retained.
- `proven-baseline-exact-delta`: the repository semantic validator binds the
  checked-in profile and fully rendered command/environment, reports every
  allowed gate-specific difference, reports zero unknown or non-allowlisted
  differences, and proves the submitted command bytes equal the validated
  bytes. A hand-written or synthetic PASS is invalid.
- `predispatch-no-side-effects`: the exact wrapper chain's mutation suite proves
  that every rejected input reaches zero SSH, upload, remote mutation, model
  staging, and `sbatch` calls.
- `repository-service-route-readiness`: a bounded non-mutating request from the
  real publisher identity receives a validated repository ACK over the exact
  route before publication; markers and sleeps do not count.

- `onnx-model-runtime-compatibility`: declared Qwen dimensions (`head_dim`),
  colocated external data, tensor-only graph values, canonical state vectors,
  per-stage prefill/decode parity, and a CUDA-chain `referenceTopToken`.
- `promotion-hash-config-delta`: atomic final-root promotion, no `.partial`
  paths, root-relative checksum ledger, complete sidecars, and post-copy
  `sha256sum -c`.
- `submit-env-contract` and `wrapper-config-child-status`: no comma-bearing
  values in Slurm `--export`, exact variable-name closure, parsed device-count
  check, fresh output root, and explicit classification of pending/canceled
  alternatives.
- `bundle-cwd-artifact-mount`: every relative artifact is resolved after
  `cd "$BUNDLE"` and is rechecked with its sidecar inside the exact SIF.

For a Qwen ONNX functional submission, the candidate evidence must also name
the one-stage and three-process exact-SIF ORT session-construction probes. A
probe PASS is necessary to distinguish a Provider integration crash from a
runtime/model load failure, but it is not a functional T026 PASS.

The repository-owned `ndnsf-di-spec175-functional-preflight` is the semantic
authority for the current Spec175 functional bundle. It validates the frozen
model/workload/role contract, Controller `start()` and event-loop ordering,
Provider commands, external model mounts, and the absence of a caller-supplied
Provider-role map. Run the skill's independent
`validate-functional-bundle-closure.py` as a second identity/HOME audit when
the staged bundle contains `policy.yaml`; that report must bind the exact
`controller-wrapper.sh` and `policy.yaml` hashes and report an empty
`missingBootstrapIdentities` list. Neither report substitutes for the other
when both input forms are present.

The current-SIF control evidence must belong to the same SIF digest. A model,
bundle, or workload may have its own content identity, but every intentional
difference from the registered control must appear in the configuration-delta
evidence.

## Interpretation

The operator validator checks completeness, existence, nonempty files and hash
binding. It does not reimplement the repository's semantic profile comparator.
Consequently, `proven-baseline-exact-delta` must cite the real comparator output
and its digest; a fabricated `PASS` file can satisfy syntax but not engineering
acceptance. The active Spec, its executable preflights, and their oracles remain
authoritative.
