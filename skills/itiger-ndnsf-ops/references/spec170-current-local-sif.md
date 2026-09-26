# Spec170 current local-SIF operating profile

Use this reference for the current NDNSF-DI release route (2026-08-19). The
repository's active Spec Kit files and the candidate build record remain the
authority; this file is the recurring-incident shield.

For a short pre-build handoff containing the current image identity and the
failure classifications learned this week, first read
[`spec170-update-lessons-20260819.md`](https://github.com/matianxing1992/NDN_Service_Framework/blob/main/specs/170-reusable-layer-artifacts/evidence/spec170-update-lessons-20260819.md).

For the complete source-update procedure, use the "Source-to-SIF update
contract" in the repository's `docs/NDNSF-DI-runtime-workflow.md` and the skill's
`references/spec170-update-cycle.md`. This snapshot deliberately records the
current route and candidate, not a mutable release alias.

## Current state and usability rule

The local `runtime-r23.sif` is physically usable for the exact sealed source
revision `989a9daace669a4f93496dade3176c527edb2469` and its record passes the
local build-boundary contract. It is **not** a current working-tree release:
uncommitted source/generated entries are outside that seal. Before any Tiger
submission, compare the source seal and native hashes, not only the SIF's old
import result. If they differ, stop and create a new candidate; never bind a
changed checkout or a host-built extension into r23. The exact image probes
are recorded in `specs/170-reusable-layer-artifacts/evidence/current-sif-r23-local-20260819.md`.

The post-cleanup reuse gate is recorded in
`specs/170-reusable-layer-artifacts/evidence/current-sif-reuse-gate-20260819.md`:
the SIF hash matched its record, the metadata-only build-record validator
passed, and the exact-SIF Python import passed. The filesystem had 75 GiB free
at that check. This does not change r23's scope: it remains valid only for its
sealed source revision, not for the dirty working tree.

The current no-repeat configuration is:

```text
Apptainer:       `/opt/apptainer/1.5.3/bin/apptainer` 1.5.3; target compute 1.5.3
build entry:     packaging/.../scripts/build-local-sif.sh
native builder:  inside the candidate SIF (or sealed ABI-identical builder)
image Python:    /opt/venv/bin/python; Python 3.10 site-packages
target closure:  BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL on every target
Tiger action:    verify hash, stage once, execute; never build/materialize
disk:            record `df -h` every run; 75 GiB was free after the
                 2026-08-19 cache cleanup (53 GiB before cleanup); no duplicate
                 multi-GB candidate
```

Model/runtime compatibility is a separate gate. The pinned Qwen3-0.6B probe
cannot use r23: its Transformers 4.48.2 (and the host child 4.46.3) does not
recognize `model_type=qwen3`. The temporary newer-package overlay that loaded
the model was diagnostic-only. A Qwen3 run therefore requires a new sealed
runtime lock and local SIF; do not change host Python, mount the overlay into a
release, or copy another model snapshot.

The release decision is therefore two independent checks: `SIF works for its
sealed source` and `sealed source matches the source being tested`. Passing the
first does not imply the second.

The current checkout remains outside that source boundary. Its diagnostic
regressions are recorded separately (496 C++ unit cases, 34 C++ integration
cases, 103 Python Spec170 tests, and 77/78 auxiliary authorization/security
tests). The latter is blocked by eleven frozen-inventory hash mismatches, not
by a reason to rewrite the baseline. Seal and review the intended source,
regenerate its inventory, and build a new candidate before running changed
bytes on Tiger. Until the remaining lifecycle/negative gates and T029 freeze
are closed, r23 is bounded verification only.

Before reuse or promotion, run the cheap identity gate in
[`spec170-update-cycle.md`](spec170-update-cycle.md): one `sha256sum`, the
metadata-only build-record validator, and an in-SIF `ndnsf._ndnsf` import. The
gate prevents repeated full-image reads and duplicate SIF creation, but it does
not qualify a changed checkout; source/native hash mismatch still requires a
new candidate.

## One normal route

1. Build the **complete application SIF locally** with
   `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh`.
   Use the sealed source, dependency lock, and the Apptainer version measured
   in a bounded allocation on the target partition. The current qualified
   local/compute version is `1.5.3`. Invoke the qualified
   `/opt/apptainer/1.5.3/bin/apptainer` explicitly; do not rely on an ambient
   `PATH` entry.
2. Build `_ndnsf.so`, the Provider, and every container-bound native extension
   inside the SIF build stage or an ABI-identical sealed builder rootfs. Record
   Python executable/version, SOABI, EXT_SUFFIX, headers, compiler, glibc,
   native-library roots, and hashes. A host Python 3.8/3.10 mismatch is a
   `WRONG_BUILD_BOUNDARY`, not a request to install host headers.
3. Run the full target-closure audit before sealing. Build all examples, not
   only the target that first failed. Every ONNX smoke and Provider target must
   explicitly list `BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL`; a sibling target does
   not inherit another target's `use=` list.
4. In the exact SIF, verify the final Provider and `_ndnsf.so` census, hashes,
   `import ndnsf._ndnsf`, real runner check-only/`--help`, `ldd` closure, and
   internal library lock. Reject stale base-image binaries, host RPATHs,
   unresolved SONAMEs, duplicate libraries, and host-mounted replacements.
5. Run local MiniNDN/in-process functional gates. For a network workload,
   validate policy/manifest/artifact paths, isolated HOME/PIB, unique Provider
   identities, and a readiness-to-request barrier. If a manifest uses relative
   artifact paths, every Provider launch must `cd "$BUNDLE"` before checking or
   opening them.
   Before treating the adapter gate as PASS, generate the deterministic CPU
   ONNX fixture with `tests/fixtures/spec170/generate_cpu_onnx_fixture.py`, set
   `NDNSF_DI_TEST_ONNX_MODEL`, and run the focused
   `DistributedInferenceCollectiveRuntime` test. The unset-fixture skip is not
   evidence; the enabled test must execute two real ONNX runners through the
   collective Worker, verify numerical output and CPU `ExecutionEvidence`, and
   require whole-group completion.
6. Copy exactly one hash-bound SIF plus its build record and evidence to
   project storage. TigerCluster only verifies the hash, stages once to
   writable job-local scratch, and executes with Apptainer; it does not build,
   materialize, pull Docker/OCI, or repair routes in place.

## Required release record

For every source-to-SIF update, retain one record beside the candidate with:

- candidate ID; source, lock, definition, and bundle digests;
- local/compute Apptainer versions and build entry point;
- SIF size/SHA-256, `containerNativeBuild`, `hostBinaryInputs`;
- build/runtime Python executable, `SOABI`, `EXT_SUFFIX`, compiler/glibc;
- Provider and `_ndnsf.so` hashes plus the complete library-closure report;
- exact local test commands/results and the Tiger action
  `verify-hash-and-execute-only`;
- rejected predecessor IDs and the concrete reason each was invalidated.

“Usable SIF” means usable for that exact source/lock identity only. Any later
C++, Python, dependency, native-binding, definition, or lock change creates a
new candidate; a prior import, `ldd`, or Tiger smoke cannot qualify the new
source. Keep the recurring failure classes visible in the record:
`WRONG_BUILD_BOUNDARY`, incomplete sibling Waf target closure, stale base
artifacts, missing `cd "$BUNDLE"`, shared HOME/PIB/bootstrap races, a stopped
Face event loop during large fetches, host Linuxbrew/`ld`
`HOST_TOOLCHAIN_CONTAMINATION`, and duplicate-SIF/model disk pressure.

## Sealed bounded-verification candidate (not dirty-tree source)

The prior local candidate is `runtime-r23.sif` from the 2026-08-18 local
Apptainer build, SHA-256
`5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb`, paired
with `build-record-r23.json`. Its source revision is
`989a9daace669a4f93496dade3176c527edb2469` and its source seal is
`sha256:348a797c746bf91d097f04627e0da524511cdcf885c9c22dbc94a0c770c7059e`.
Its build record says
`containerNativeBuild=true`, `hostBinaryInputs=[]`,
`tigerAction=verify-hash-and-execute-only`, and Apptainer `1.5.3`. Treat the
path and hash as a snapshot, not as a mutable alias. It is usable for that
sealed revision, but not for later uncommitted source changes. A source or
dependency change requires a new candidate identity.

## Repeated failures and the prevention rule

| Observed failure | Prevention |
|---|---|
| Host-built `cp310`/`_ndnsf.so`, missing `Python.h`, or host 3.8/3.10 drift | Stop as `WRONG_BUILD_BOUNDARY`; rebuild inside the sealed SIF ABI. |
| Only one Waf target fixed while a sibling ONNX/Provider target lacks NDN-SVS/NDN-CXX/Boost closure | Generate the complete `examples/wscript` target census and build all targets. |
| SIF contains an old extension copied from a qualified base | Remove stale Provider/framework/extensions in the definition; require final in-SIF census and hash match. |
| Provider cannot find a relative model/artifact path | Enter `cd "$BUNDLE"` and run an in-container existence/hash check before launch. |
| User starts before any Provider and waits for NAC-ABE DKEY | Start Controller, bootstrap Provider, then User; retain an explicit readiness barrier. |
| Large-data CLI blocks while the Face has no event loop | Keep the Face event loop alive while the bounded fetch runs; do not increase timeouts blindly. |
| Remote full-SIF hashing times out | Stage the verified SIF once to job-local scratch, hash the staged copy, then reuse it. |
| Login-node Docker/Buildah or advertised scratch used as compute evidence | Probe the allocated node; current Spec170 does not use Docker/OCI or Tiger-side materialization. |
| Linuxbrew/host `ld` resolves unrelated GTK/UAV/system libraries | Classify `HOST_TOOLCHAIN_CONTAMINATION`; rebuild every target inside the sealed SIF builder and rerun the complete internal closure gate. |
| Readiness passes but the request path fails | Require Request → ACK_CLOSED → Selection → Response and every child exit code; readiness alone is not PASS. |

For the DATA_V1 cancellation fault gate, normal runs keep
`data-v1-no-progress-ms=2000`. A deliberately injected 5 s stage gap must
pass a larger value explicitly and record it in the summary. The successful
12 s-bound host MiniNDN run, the earlier terminal-watchdog failure, and the
harness assertion false negative are recorded in the repository evidence file
`real-minindn-cancellation-filterfix-20260819.md`; this is protocol evidence,
not SIF or Tiger promotion evidence.

## Disk discipline

Keep the active SIF, build record, source seal, hash, and canonical evidence.
Do not create a second SIF, full model copy, or broad performance matrix merely
to retry a configuration error. Before cleanup, enumerate exact superseded
paths and bytes; never use a broad glob or age threshold. Record free space on
every run (75 GiB was free after the 2026-08-19 cache cleanup; 53 GiB was the
pre-cleanup observation), so avoid another multi-gigabyte image until an
explicit cleanup/retention decision is recorded.

Before rebuilding, follow the mandatory order in
[`spec170-update-cycle.md`](spec170-update-cycle.md): disk/retention, new
candidate identity, target-node Apptainer parity, source/build-boundary check,
complete Waf target census, one local complete SIF, exact-SIF ABI/library
closure, local functional and negative gates, then one hash-bound promotion.
Do not let a readable SIF, a single successful target, or a host linker result
replace this order.
