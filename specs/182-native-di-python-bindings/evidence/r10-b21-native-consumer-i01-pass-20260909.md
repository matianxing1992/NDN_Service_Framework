# R10-B21 I01 Native Consumer Positive Case — 2026-09-09

## Scope and boundary

R10-B21 runs the first real native C++ consumer through the owner-alive
MiniNDN composition. The executable is the existing same-source
`build-nac182/spec182-installed-consumer` target from the T002-A installed
library boundary. A generated candidate manifest declares its executable and
resolved 31-file ELF closure, absolute `bwrap`/`strace`/`nsenter` tools,
requester node binding and the independent stdout marker
`SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK`.

This closes only the I01 positive native-consumer/closure boundary. It does not
exercise a requester/provider DI request, model planning, grant/selection,
I02-I08 counterexamples, maintained callers, no-Python business execution or
T016 qualification.

## Source and review trace

- Source checkpoint: `4900f14a` (R10-B20 collector evidence), with the existing
  T002-A native consumer binary from the recorded `build-nac182` candidate.
- Generated runner manifest: `.codex-tmp/spec182-r10-b21-native-consumer-manifest.json`.
  It contains 31 artifacts; the run was not made reproducible by committing
  build outputs or machine-local dependency binaries.
- Owner output: `.codex-tmp/spec182-r10-b21-native-consumer-owner/`.
- Official review skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`.
  SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.
- Review result: **No findings.** The read-only review covered manifest
  identity/hash mapping, owner-alive lifecycle, namespace FD handoff, runner
  command, marker oracle, trace/evidence evaluation and cleanup.

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `Experiments/NDNSF_DI_NativeClosure_Minindn.py` owner and `main --execute-owner --runner-manifest`; canonical runner `main --case I01` | Root command below; `rg -n -- "--execute-owner|runner-manifest|_execute_runner_case" Experiments tests` | Owner kept requester/provider contexts alive until runner evaluation; canonical runner was the only executor. |
| `implementation and wire` | `covered` | `run_case`, `make_launch`, `collect_trace`, `evaluate_case`; generated executable/shared-library artifact mapping | `file`, recursive `ldd`, manifest inspection; runner result command | Held `/proc/self/fd/12` entered requester namespace; absolute ELF loader/libs were individually mounted; no Python policy violation. |
| `test/harness/oracle` | `covered` | I01 manifest, stdout marker, trace/evidence evaluator; `tests/python/test_spec182_native_closure.py` | `pytest -q tests/python/test_spec182_native_closure.py`; inspect `runner-result.json` | 31 focused Python cases pass; marker and all required evidence yield evaluator `PASS`; no business result is fabricated. |
| `build/source closure` | `covered` | `build-nac182/spec182-installed-consumer` and its 31 resolved ELF artifacts | `file`; `ldd`; SHA-256 entries in generated manifest; T002-A installed-consumer evidence | Existing native target and dependency closure were consumed without rebuilding; candidate binary is not a portable source release artifact. |
| `migration/evidence` | `covered` | `case-manifest.json` I01 registration, `node-context.json`, `runner-result.json`, `result.json` | `validate_design.py`; raw output inspection | Durable evidence records I01 `PASS`; I02-I08, DI request/result, caller migration, no-Python and T016 remain open. |

## Root execution

```text
sudo -n env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  --manifest .codex-tmp/spec182-r10-b19-20260909052040-owner.json \
  --output .codex-tmp/spec182-r10-b21-native-consumer-owner \
  --execute-owner \
  --runner-manifest .codex-tmp/spec182-r10-b21-native-consumer-manifest.json \
  --runner-case I01
exit code: 0
```

Observed in `runner-result.json`:

- evaluator `status=PASS`, `failures=[]`;
- process `returncode=0`, `timedOut=false`, duration about 510 ms;
- observation `complete=true`, `integrityViolations=[]`,
  `policyViolations=[]`;
- evidence: `business-oracle`, `cleanup`, `endpoints`, `exec-map`,
  `identity`, `namespace`, `process-tree`;
- four observed PIDs, one successful native exec and two exit events;
- captured stdout exactly contains `SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK`;
- launch begins with `/usr/bin/nsenter --net=/proc/self/fd/12`, followed by
  `/usr/bin/strace` and `/usr/bin/bwrap`.

The owner wrote `node-context.json` before launching the runner and cleaned up
MiniNDN in its `finally` path after the evaluator returned.

## Validation and closure

`pytest -q tests/python/test_spec182_native_closure.py` remains green with 31
cases. Python compilation, `git diff --check` and the design validator passed
in the preceding R10-B20 batch; no native rebuild was needed for this execution
batch.

Batch retrospective:

- `static`: no introduced regression; manifest, call path, namespace identity,
  marker independence, trace/evidence and cleanup were reviewed.
- `compile/link`: existing T002-A native target and resolved ELF closure were
  consumed; no new compile/link attempt was required.
- `runtime/test`: owner-alive MiniNDN run and canonical runner returned a real
  I01 `PASS` with complete trace and all required evidence.
- `unobserved`: DI business request/result, all negative isolation cases,
  maintained caller routes, no-Python business proof and T016 qualification.

Closure decision: `CLOSED_FOR_VALIDATION` for I01 native consumer execution;
T014 remains partial and T016 remains `PARTIAL/UNQUALIFIED`.
