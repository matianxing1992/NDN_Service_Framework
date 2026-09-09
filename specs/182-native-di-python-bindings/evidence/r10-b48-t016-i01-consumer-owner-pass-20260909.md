# R10-B48 T016 I01 Installed Consumer Owner/Runner Pass 2026-09-09

## Scope and stable exit

本批用当前 MiniNDN owner 重新执行已冻结的 I01 `spec182-installed-consumer` runner case，
验证安装库 native consumer 能在 requester namespace 中启动、加载其声明的 ELF closure，
并输出独立 business marker。它与 R10-B47 的 PO-001 DI fixture 分开记录；不改变代码或
manifest，不把单个 I01 结果提升为完整 T016。

稳定出口是 canonical owner 在真实 requester/provider namespace 存活期间接 runner，staged
consumer 返回 `0`，且七类 evidence 与 trace/policy integrity 检查完整。

## Five-lane coverage matrix

| Lane | Evidence |
| --- | --- |
| production entry/callers | `Experiments/NDNSF_DI_NativeClosure_Minindn.py` → `tests/standalone/run-spec182-native-closure.py` → `.codex-tmp/spec182-r10-b21-native-consumer-manifest.json` I01 process `/bin/spec182-installed-consumer` |
| implementation and wire | installed native consumer executable and its declared shared-library closure; marker `SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK` is emitted by the staged process |
| test/harness/oracle | I01 manifest requires the marker and `identity/process-tree/namespace/exec-map/endpoints/business-oracle/cleanup`; collector validates process/node binding, trace pairing, exit and cleanup |
| build/source closure | current owner stages the manifest-declared executable/library hashes; no source changed and no native rebuild was needed in this run |
| migration/evidence | fresh raw run `.codex-tmp/spec182-t016-r10-b48-owner/` contains `node-context.json`, `runner-result.json` and `closure-run/{stdout,stderr,trace}`; no Python runtime is present in staged native artifacts |

## Review trace

按官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）的只读检查核对 I01
manifest、process role/node binding、artifact closure、business marker 和 collector evidence。
本批没有版本化 source diff，因此没有 P1/P2/P3 finding。

## Command and result

```text
sudo -n env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin python3 \
  Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  --manifest .codex-tmp/spec182-t016-r10-b17-20260909050945-manifest.json \
  --output .codex-tmp/spec182-t016-r10-b48-owner \
  --execute-owner \
  --runner-manifest .codex-tmp/spec182-r10-b21-native-consumer-manifest.json \
  --runner-case I01
```

Exit `0`. `result.json` and runner evaluation are `PASS`; staged process rc `0` in `344ms` and
stdout contains `SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK`. Observation is complete with two
successful execs, four observed PIDs, all seven evidence classes, and an empty violations list.

## Result and limits

This closes `FOCUSED_QUALIFICATION_PASS` for the installed native consumer I01 owner/runner
boundary. It does not exercise a DI request, independent requester/Provider transport, I02-I08,
PO-001-PO-014, maintained caller/no-Python migration or full T016 qualification; those remain
`PARTIAL`/`UNQUALIFIED`.

## Batch retrospective

- **Static miss:** none; the existing manifest role and marker matched the runner vocabulary.
- **Compile miss:** none; this was a no-source-change owner execution.
- **Runtime miss:** none for I01; process, marker, trace and cleanup all passed.
- **Unobserved:** DI protocol, independent Provider process, negative cases and remaining T016 matrix.

## Closure decision

`CLOSED_FOR_VALIDATION` for I01 installed-consumer owner/runner evidence; `OPEN_FOR_NEXT_BATCH` for
the remaining I02-I08 and PO cases and any independent requester/Provider transport.
