# R10-B40 T016 PO-001 Native Owner/Runner Pass

日期：2026-09-09。此批次只执行 T016 的一个 bounded acceptance case，确认 root MiniNDN owner、
namespace-bound runner、native integration executable 和业务 oracle 可以组成一个真实正向出口。
它不代表 I01--I08 或 PO-002--PO-014 完整资格通过。

## Scope and stable exit

稳定入口为 `sudo -n python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py --execute-owner`，
由 `tests/standalone/run-spec182-native-closure.py` 在 requester namespace 存活期间执行 PO-001。
registration manifest 使用 `campaignCase=PO-001`；runner manifest 由先前 raw manifest 派生，补齐
必需的 `process.role=requester` 并按当前 `integration-tests` 文件重新计算 artifact digest。修订只在
fresh raw run 使用，不修改仓库源码或冻结 fixture。

## Coverage matrix

| Lane | Result |
| --- | --- |
| `production entry/callers` | covered: owner campaign → MiniNDN requester/provider topology → canonical runner → `integration-tests` PO-001 selector |
| `implementation and wire` | covered: owner namespace/PID/NFD socket context, bubblewrap/strace launch, native R4-B6 real Provider request and terminal result |
| `test/harness/oracle` | covered: `businessOracle.stdoutMarker=SPEC182_NATIVE_DI_REQUEST_RESULT_OK`; evaluator returned no failures |
| `build/source closure` | covered for the existing native executable and declared ELF closure; no source changed and no rebuild was needed in this batch |
| `migration/evidence` | partial: this is one cross-process native acceptance case; maintained YOLO/Qwen callers, all remaining cases and no-Python matrix remain open |

## Review and result

Read-only review found no new product defect. The initial raw retries are preserved:

- `r10` stopped at `process role is invalid` because the older raw runner manifest omitted the required role;
- `r11` stopped at `artifact digest mismatch` because its integration executable digest was stale;
- `r12` used a fresh corrected manifest and completed the owner/runner path.

Command result for `r12` was exit `0`; `result.json` is `PASS`, `runner-result.json` evaluation is
`PASS` with an empty failure list, native process return code is `0`, `timedOut=false`, and the
stdout business marker is present. The result also records requester namespace inode/PID/start ticks,
NFD socket and peer identity, per-process trace/output, declared ELF mounts, and cleanup.

The observed behavior is therefore `FOCUSED_QUALIFICATION_PASS` for PO-001 only. T016 remains
`PARTIAL` until the registered I01--I08 and PO-001--PO-014 matrix, full same-source suites, no-Python
and maintained-caller boundaries are executed and reviewed.

## Raw evidence

- `.codex-tmp/spec182-t016-r10/` — stale process-role preflight boundary.
- `.codex-tmp/spec182-t016-r11/` — stale artifact-digest preflight boundary.
- `.codex-tmp/spec182-t016-r12/` — successful root owner/runner result and trace.

Closure decision: `OPEN_FOR_NEXT_BATCH`; next batch must execute the next independent registered case
with a manifest generated from the current build identity, preserving the same owner and evidence
requirements.
