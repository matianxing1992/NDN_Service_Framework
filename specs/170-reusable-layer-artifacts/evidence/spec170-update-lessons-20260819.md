# Spec170 SIF update handoff (2026-08-19)

This is the short operator handoff for the next NDNSF-DI SIF update. It
records the currently usable image, the one supported build route, and the
failure classes that previously caused repeated work. The detailed contracts
remain in `docs/NDNSF-DI-runtime-workflow.md` and the iTiger skill reference.

## Current usable image

```text
SIF:            .codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif
SHA-256:        5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb
record:         .codex-tmp/spec170-container-build-20260818-r14/build-record-r23.json
sourceRevision: 989a9daace669a4f93496dade3176c527edb2469
sourceSeal:     sha256:348a797c746bf91d097f04627e0da524511cdcf885c9c22dbc94a0c770c7059e
Apptainer:      /opt/apptainer/1.5.3/bin/apptainer (local and target: 1.5.3)
runtime Python: /opt/venv/bin/python 3.10.18, SOABI cpython-310-x86_64-linux-gnu
container build: true; host binary inputs: []
Tiger action:   verify-hash-and-execute-only
Current disk:   latest `df -h .` reports 73G available (58% used); the earlier
                reuse gate recorded 75G. These are operational snapshots, not
                permission to create duplicates
```

The image passed its exact-SIF hash/record check, `ndnsf._ndnsf` import,
single-extension census, extension `ldd`, and Provider wiring check. It is
usable only for the source seal above. The current working tree contains
uncommitted changes, so r23 must not be used to claim or run the newer source.

The bounded D0/D1 repeat is operationally usable but not yet deterministic
evidence: one combined invocation recorded `dependency status=incomplete`
because the `backbone-to-head0` producer timing line had `bytes=0` and an empty
planned name, while the consumer fetched 120 bytes and the final response
succeeded. Later standalone/combined repeats passed. This is retained as a
producer-side timing/instrumentation warning; the analyzer must not be relaxed
to make the mismatch disappear.

The parser now compares the actual `data_name` URI when Python traces use
`planned_name=true|false`. The current source also serializes the native timing
and capacity diagnostic block with a mutex; this source fix is not present in
r23 and therefore requires a new source-bound SIF before the warning is closed.

## Latest model/runtime preflight (2026-08-19)

The pinned `Qwen/Qwen3-0.6B` snapshot was downloaded once into the
content-addressed Hugging Face cache (revision
`e6de91484c29aa9480d55605af694f39b081c455`; model digest
`sha256:5ce2a6d5d0e96dea66cc439b6443460660cd8d99ad1ae84e7139033349851e7a`).
The first real MiniNDN probe stopped before Controller/Provider startup because
the host child used Transformers 4.46.3 and r23 contains 4.48.2; neither
recognizes `model_type=qwen3`. This is a model/runtime compatibility failure,
not an NDNSF protocol result.

A temporary 102-MB overlay with Transformers 4.51.3 loaded the model and
generated tokens inside r23, but it was diagnostic-only: it was not sealed
into the SIF, and the same overlay failed on host Python 3.8. Do not install a
newer Transformers package into the host, bind the overlay into a release, or
copy the model into the image. The next Qwen3 run requires a new candidate SIF
with a reviewed, complete Qwen3-compatible lock/ABI, followed by the exact-SIF
import, standalone same-model load/generate, and only then the MiniNDN Gate B
sequence. Keep one cached model snapshot and record its manifest; do not create
duplicate model or SIF copies.

## No-repeat update decision card

Use this card before touching a SIF or requesting a Tiger allocation:

| Change since the recorded candidate | Action |
|---|---|
| Only externally mounted model, prompt, policy, route, or workload | Keep r23; create a new hash-bound bundle/manifest and run bundle preflight. |
| C++, Python wrapper, NDN-CXX/NDN-SVS, dependency lock, native library, compiler, Python ABI, Apptainer, or SIF definition | Keep r23 as a control; create a new candidate identity and build one complete local SIF. |
| Documentation/evidence only | No SIF; update the evidence record. |

Model/runtime changes have an additional stop: compare the model config
(`model_type`, declared Transformers version, tokenizer and file hashes) with
the exact image lock before any lifecycle run. If the architecture import or
same-model load fails, stop before MiniNDN and issue a new candidate identity;
do not treat a package overlay or a host import as a release fix.

For a new candidate, the order is fixed:

```text
df -h + exact disposable-path census
  -> candidate-input inventory + source/lock/definition/candidate identity
  -> target-node Apptainer parity
  -> sealed build-boundary and complete Waf target census
  -> build-local-sif.sh (one local application SIF)
  -> exact-SIF Python/native/RPATH/ldd/library gates
  -> local CPU/MiniNDN lifecycle and negative gates
  -> one hash-bound promotion; Tiger verifies, stages once, executes only
```

Before sealing the source, generate the deterministic candidate-input
inventory and keep it beside the candidate evidence:

```bash
python3 tools/ndnsf-di/collect_spec170_candidate_inputs.py \
  --workspace . \
  --output "$EVIDENCE/candidate-input-inventory.json"
```

The inventory covers runtime source, native/Python bindings, examples,
build/packaging inputs, relevant harnesses/tests, and Spec170 contracts. It
records the source revision, worktree-status digest, per-file SHA-256 values,
and one inventory digest. It is a pre-freeze coverage check, not a substitute
for T029's frozen manifest: model files, security/route/schedule bundles,
Gate A/B/C outputs, and the staged SIF still need separate hash-bound records.
If an intended input is missing or the inventory changes after sealing, stop
and create a new candidate identity instead of reusing the old SIF.

The following are pre-build hard stops, not problems to discover remotely:
host `Python.h`/`cpXY` or copied `.so` (`WRONG_BUILD_BOUNDARY`), a sibling
Waf target without its own `BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL` closure,
stale binaries inherited from a base SIF, a Provider launched outside
`cd "$BUNDLE"`, a generated bundle missing a manifest-referenced relative
artifact or its `.sha256` sidecar, shared HOME/PIB or bootstrap races, a stopped Face event loop,
Linuxbrew/host `ld` contamination, and duplicate SIF/model copies. Preserve
the first failure and issue a new candidate after repair; do not patch r23 or
retry the same identity.

## Current source qualification boundary

The current checkout is not yet a candidate source for a new SIF. The local
controlled regressions on that checkout are useful diagnostics (496 C++ unit
cases, 34 C++ integration cases, and the latest 105-pass Python Spec170
contract glob), but
they are not evidence that r23 contains those changes. The auxiliary
authorization/security run passed 77/78 tests; the remaining failure is the
source-integrity check because eleven entries differ from the older frozen
baseline (seven dirty entries and four already changed at `HEAD`). Do not
rewrite `runtime-gates.json` just to make that check green. First select and
review a deliberate source candidate, seal it, regenerate its inventory, and
only then build a new SIF.

Until that source-seal step and the remaining T029/lifecycle gates are closed,
the status is **bounded verification image, not a frozen release**. A Tiger
submission using the dirty checkout, a copied host extension, or an old r23
hash is invalid even if the image imports and reaches READY.

## Only supported update route

For any C++, Python, NDN-CXX/NDN-SVS, dependency-lock, native-library,
definition, compiler, Python ABI, or image-packaged runtime change:

```text
record disk and exact disposable paths
  -> create candidate/source/lock/definition identity
  -> verify target-node Apptainer == local 1.5.3
  -> validate sealed build boundary and complete Waf target census
  -> build one complete local SIF with build-local-sif.sh
  -> compile _ndnsf.so and native code inside the SIF builder
  -> run exact-SIF import/runner/ABI/RPATH/ldd/library-lock gates
  -> run full local tests and real CPU/MiniNDN positive + negative lifecycle
  -> copy one hash-bound SIF, verify remote/staged hash
  -> Tiger stages once, cd "$BUNDLE", and executes only that SIF
```

Docker, OCI conversion, registry pulls, Tiger-side materialization, host
Python virtual environments, and host-built `.so` files are not part of this
route. A workload, policy, route, prompt, or externally mounted model change
may reuse a matching SIF, but still requires a new bundle/manifest identity
and bundle preflight.

## Current fixed configuration

| Item | Required value or rule |
|---|---|
| Apptainer | Explicit local `/opt/apptainer/1.5.3/bin/apptainer`; target compute node must report 1.5.3 |
| Build entry | `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh` only |
| Container Python | `/opt/venv/bin/python` / Python 3.10 inside the sealed builder and final SIF |
| Native closure | Every Provider and ONNX target closes `BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL` |
| Runtime inputs | One hash-bound SIF plus one read-only bundle; models remain external/content-addressed |
| Model/runtime gate | Image lock must recognize the pinned model architecture; Qwen3 is not supported by r23's Transformers 4.48.2 |
| Candidate inputs | Generate `candidate-input-inventory.json` before source sealing; retain its digest with the candidate and compare it before promotion |
| Provider launch | `cd "$BUNDLE"`, materialize relative artifacts plus `.sha256` sidecars, and use isolated HOME/PIB per role |
| Provider cleanup | Wrap each launch with `exec env` so the tracked PID is the Provider; after the workload, verify no job-local Provider remains |
| Cluster action | Hash verify, stage once, execute; never rebuild or materialize remotely |
| Storage | Record `df -h`; keep one active SIF and canonical evidence; remove only enumerated superseded paths |

## Recurring failure classification

| Observed symptom | Classification and repair |
|---|---|
| `Python.h`, `pythonX.Y-dev`, `cpXY`, unresolved Python symbols, or host `_ndnsf.so` | `WRONG_BUILD_BOUNDARY`; rebuild the extension inside the sealed SIF builder |
| One target links but a sibling misses NDN-CXX/NDN-SVS/ONNX Runtime | Incomplete target census/`use=` closure; fix all sibling targets before rebuilding |
| Import passes but an older base extension is selected | Stale-base artifact; remove old Provider/framework/extension outputs and create a new candidate |
| Relative model/artifact not found | Check both Provider cwd and generated-bundle materialization; require the artifact plus matching `.sha256` sidecar before launch and rerun bundle preflight |
| Response passes but Provider processes remain | The workload tracked a shell instead of the Provider; add `exec env`, rerun, and check the post-run process census |
| Linuxbrew/host `ld` pulls GTK/UAV/system libraries | `HOST_TOOLCHAIN_CONTAMINATION`; rebuild inside the sealed builder and rerun library closure |
| SIF import/READY passes but request lifecycle fails | SIF smoke is insufficient; classify the protocol/orchestration failure and run the real Request → ACK_CLOSED → Selection → Response gate |
| Temporary SIFs, Docker layers, or model copies consume disk | Stop; preserve the active SIF/evidence, enumerate exact disposable paths, clean only those paths, and do not create duplicate images |

Never repair these symptoms by patching an old SIF, copying a host binary, or
submitting another Tiger job. Preserve the first failure, issue a new
candidate identity after the repair, and record the command/result beside the
candidate.
