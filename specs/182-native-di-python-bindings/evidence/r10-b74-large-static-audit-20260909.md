# R10-B74 Large Static Audit — 2026-09-09

本批对当前 Spec182 的生产 C++ 目录、维护中的 Python 调用方、构建出口和进度证据
做只读大范围审计。没有修改 skill，也没有把静态通过当作跨进程或资格验收。

## Scope

| Lane | Audited boundary |
| --- | --- |
| Production entry/callers | `NativeInferenceClient::request`/`dispatchOperation`/`beginCoreRequest`, `APPClient` native facade, maintained LLM/YOLO callers, `DI_NativeRequester`, `di-native-provider` |
| Implementation/wire | runtime exact schema, tokenizer/generation binding, request envelope, Core callbacks, Provider host registration, worker/lifetime paths |
| Build/source closure | Waf requester and Provider targets, `ldd`/RUNPATH/NEEDED, current `Experimental` HEAD `bc2180f9` |
| Static tools | CodeGraph call/source trace, Python AST parse, Cppcheck 1.90 (`63` native production `.cpp` files), targeted text/metadata consistency checks |
| Progress/evidence | `tasks.md` P1–P7 and R10-B71/R10-B73 records, `docs/failure-log.md`, compatibility manifest provenance |

## Results

### Confirmed clean checks

- Python AST parse covered `4307` non-generated `.py` files with `0` syntax errors.
- Current `DI_NativeRequester` and `di-native-provider` targets built successfully from the
  system-first toolchain with Waf `-j4` in `24.188s`; no competing build was running and the
  post-first-line `vmstat` samples showed no sustained swap-in/out.
- `ldd` reported no missing libraries for either binary. This only proves the current host can
  resolve the libraries; it is not a sealed deployment or SIF closure.
- Existing source-bound R10-B31/B33/B37/B73 selectors remain the local in-process evidence
  boundary. No task card was advanced by this audit.

### Findings requiring follow-up

1. **High — Provider serve failure can hang forever.** In
   `examples/DI_NativeProviderExecutable.cpp:1879-1887`, the detached install task catches
   assembly/permission errors, marks provisioning failed, and signals completion. The main
   thread then unconditionally enters the `while (true) { face.processEvents(); }` loop at
   `1917-1928`; it never observes the failed state or exits non-zero. A failed Provider can
   therefore look alive to an owner and prevent a bounded requester/Provider test from
   producing a terminal failure.

2. **Medium — Duplicate CLI option.** `parseArgs` handles `--bootstrap-token` at
   `examples/DI_NativeProviderExecutable.cpp:751-753` and again at `802-804`. The second
   `else if` is unreachable (Cppcheck `multiCondition`). This is currently behaviorally
   masked because the first branch works, but it makes option auditing and future changes
   unreliable.

3. **Medium — Compatibility provenance is stale.**
   `specs/182-native-di-python-bindings/contracts/compatibility-manifest.json` records
   `sourceCommit=45f93d809b1dee3f8dc06e763cb132765cf72b47`, while the current checkpoint is
   `bc2180f9`. The manifest must be regenerated after the final source checkpoint before
   migration or retirement claims.

4. **Medium — Native is still opt-in at maintained entry points.** The maintained Python
   inventory still contains direct legacy `distributed_inference`/`request_streaming` calls,
   and `APPClient.request`, `request_streaming`, and `generate` require
   `_automatic_planner`. The explicit native facade rejects planner fallback, but it does
   not yet make the maintained callers native by default. This keeps P3–P5 open.

5. **Medium — Current binary closure is host-bound.** `ldd` resolves Provider dependencies
   from `/opt/onnxruntime`, `/usr/local/lib`, `/home/tianxing/NDN/ndn-svs/build`, and the local
   NAC-ABE install. The requester has a relative RUNPATH, while the Provider executable has
   no `$ORIGIN` framework RUNPATH. This is sufficient for the development host, not for
   T016/no-Python or sealed SIF qualification.

6. **Low — Cppcheck noise still hides small defects.** The run returned `317` diagnostics,
   mostly `44` `useStlAlgorithm` style notices. The `returnDanglingLifetime` report at
   `NativeEpochCoordinator.cpp:713` is a false positive because the vector initializer
   copies bytes from the local string. The `NativeInferenceClient.cpp:524` nested condition
   is redundant under its enclosing condition and should be simplified. The requester
   `internalAstError` at line `122` did not reproduce in the successful C++ build.

## Closure decision

`OPEN_FOR_NEXT_BATCH`. 静态审计确实发现了一个影响故障可观测性的 Provider 生命周期问题、
一个不可达 CLI 分支和多个交付/迁移边界问题；它不能证明独立 requester ↔ Provider
transport、continuation/recovery、maintained no-Python、I02–I08 或 T016/T017。当前
七个能力阶段仍是 R1–R6 `PARTIAL`、R7 `NOT_STARTED`；P1–P7 仍按 `tasks.md` 的依赖顺序
开放。下一批应先修复 Provider failure exit 和重复参数，再重新生成 manifest，随后执行
真实独立进程请求。

## Commands

```text
CodeGraph: NativeInferenceClient / dispatchOperation / beginCoreRequest /
  APPClient / DI_NativeRequester / di-native-provider call and source trace
Python AST: 4307 files, 0 syntax errors (generated build trees excluded)
Cppcheck 1.90: 63 production .cpp files, return 0, 317 diagnostics
Waf: ./waf -o build-nac182 build --targets=DI_NativeRequester,di-native-provider -j4
      PASS, 24.188s
ldd/readelf: both binaries resolve on this host; no missing entries
```
