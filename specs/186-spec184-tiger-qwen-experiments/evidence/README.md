# Spec186 evidence index

These receipts are the durable record for the current candidate boundary. A
document with `WAITING_EXTERNAL_INPUT` or `BLOCKED_AFTER_BOUNDARY` is evidence
of the stopping point; it is not a qualification result.

| Artifact | Scope | Current boundary |
| --- | --- | --- |
| `baseline-inventory.md` | exact source, host tools, model inventory | Tiger/GPU/Qwen3 inputs absent locally |
| `portability-audit.md` | caller paths, ownership and cross-host assumptions | external staging and route receipt pending |
| `design-code-convergence.md` | implementation-to-contract audit | implementation `PASS`; runtime qualification blocked by exact SIF/external receipts |
| `native-abi-closure.md` | Waf, ONNX/NAC-ABE, import/help, RPATH/`ldd`, app bundle | native host closure PASS; exact source-sealed base SIF still pending |
| `pre-dispatch-boundary-20260912.md` | all eight candidate prepare/check receipts | zero remote side effects; no runtime started |
| `pre-dispatch-repair-20260913.md` | all eight candidate receipts after collector identity repair | stale collector mismatch removed; missing external runtime/model inputs still fail closed with zero remote side effects |
| `pre-dispatch-runtime-inputs-20260913.md` | all eight candidate receipts after source/app path binding | provider-repair source seal and content-addressed app bundle are bound; missing base/model or local visibility still fail closed |
| `pre-dispatch-r5-20260913.md` | all eight candidate receipts after corrected r5 bundle staging | r5 candidate digests and zero-side-effect gates are recorded; exact source-sealed base SIF remains absent |
| `minindn-yolo-boundary-20260912.md` | fresh Y-A/Y-B/Y-N attempts | environment preflight stopped before startup |
| `minindn-qwen06b-inventory.md` | Qwen3-0.6B model/backend inventory | compatible model tuple absent |
| `tiger-preflight-20260912.md` | Tiger Slurm/GPU/project/Apptainer preflight | compute GPU available; SIF/runtime composition still pending |
| `local-apptainer-20260912.md` | local Apptainer release switch | local runtime is now 1.5.3 and aligned with Tiger compute; exact Spec186 SIF still pending |
| `standalone-yolo-reference-20260912.md` | Same-model YOLOv8n ORT CUDA reference | CUDA substrate PASS only; old SIF, not Spec186 qualification |
| `t006-ort126-and-host-gate-20260913.md` | temporary ORT-1.26 base and current host M01 recovery probe | in-container loader closure PASS; host framework/ONNX development prefix missing, so T006 remains blocked |
| `t006-nac-abe-abi-20260913.md` | Waf/setuptools NAC-ABE class-layout mismatch and repair | explicit prefix forwarded; native identity and `NativeServiceUser` lifecycle pass; exact SIF/ONNX/Tiger inputs remain pending |
| `application-bundle-r5-20260913.md` | refreshed stripped application bundle and Tiger staging | r5 tree digest `687610de…07129` matches local/remote; all files read-only; runtime qualification still waits for the exact base SIF |
| `application-bundle-r6-20260913.md` | current-source native collaboration repair bundle and Tiger staging | r6 tree digest `04c2dd64…531d73` matches local/remote; all files read-only; exact 1.5.3 base SIF remains pending |
| `runtime-version-policy-20260913.md` | explicit local/compute Apptainer policy and refreshed profile identities | all eight profiles bind 1.5.3; login node is metadata-only; current r6/source/collector digests are bound |
| `host-m01-r12-route-boundary-20260913.md` | current-source root MiniNDN retry | native ABI issue absent; repository `/STATUS` route barrier timed out before application request |
| `minindn-stream-collaboration-r53-20260913.md` | real four-Provider streamed tiny-ONNX collaboration regression | terminal-only stream grant and non-terminal dependency execution pass; YOLO qualification remains open |
| `host-m01-r55-g3-20260913.md` | current-source G3 host M01 manifest and validation | r55 M01 manifest `sha256:51039878…27c893a` passes with source/app identity bound; tiny-ONNX host gate only, no YOLO/SIF qualification |
| `closure-handoff-20260912.md` | immutable prior implemented/wired/executed/measured snapshot | historical boundary retained for audit |
| `closure-handoff-20260913.md` | current implemented/wired/executed/measured reconciliation | r6 staged and 1.5.3 aligned; branch remains `IN_PROGRESS` at the repository route/base-SIF/model gates |
| `static-wiring-audit-20260913.md` | static argv/env/bind/profile-to-runner audit and repairs | lifecycle, nested profile, terminal-evidence and run-root boundary defects fixed; YOLO canonical inputs remain fail-closed external prerequisites |
| `static-experiment-cycle-20260913.md` | static review → executable pre-dispatch/scheduler experiment → repair → re-review | all eight checks fail closed with zero remote side effects; scheduler identity drift is rejected before run-root creation |
| `direct-target-audit-20260913.md` | direct-objective and critical-path audit | MiniNDN + TigerCluster YOLO is the primary target; static/build/preflight are prerequisites and Qwen is conditional |

Runtime receipts belong in `Experiments/TigerCluster/results/<run-id>` or the
declared project-storage counterpart. Secrets, models, SIFs and large logs do
not belong in this index or in Git.
