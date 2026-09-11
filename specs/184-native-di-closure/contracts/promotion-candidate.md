# Spec184 Promotion Candidate Contract

**Status**: PLANNED / no candidate promoted
**Owner**: T006 before any expensive validation

Spec184 使用一个不可变 promotion candidate 绑定所有会影响行为的输入。候选不是一个
commit 名字，也不是一个测试二进制；它必须把源码、运行时、测试 harness、配置和外部
制品绑定到同一个身份。没有完整身份时，结果只能是 `PARTIAL` 或 `NOT_RUN`。

## Candidate Identity

候选身份由下面的有序元组组成。执行时每项都要记录 canonical path、内容或版本摘要、
生成命令和时间边界；任何一项变化都会按下表使相关证据失效。

| Plane | Candidate member | Required identity | Initial state |
| --- | --- | --- | --- |
| Source | `Experimental` source and explicit worktree diff | source commit, diff path list, source manifest digest, clean/dirty boundary | PLANNED |
| Built runtime | native libraries, `unit-tests`, `integration-tests`, `DI_NativeRequester`, `di-native-provider`, and `_ndnsf.so` when affected | build directory, compiler/linker, target hashes, exported-symbol/source closure | PLANNED |
| Test/replay harness | C++ fixtures, process drivers, MiniNDN runner and collectors | source paths, selector names, harness commit/hash, owner and cleanup contract | PLANNED |
| Submission bundle | local or external launch bundle | exact file manifest, archive/SIF digest, transfer source and destination | PLANNED |
| Effective configuration | Waf/CMake cache, NAC-ABE/NDN-SVS pair, environment, topology and permissions | canonical configuration digest and resolved dependency paths | PLANNED |
| External artifacts | model, tokenizer, ONNX, catalog, keys and SIF/Tiger inputs | artifact digest, provenance and host boundary | PLANNED |
| Validation contract | Spec184 contracts, selectors, negative cases and evidence paths | contract revision, matrix digest and required child exit statuses | PLANNED |

候选记录必须同时保存 `candidateId = digest(ordered members above)`。单个成员摘要、
marker、response 或历史 PASS 都不能单独构成 candidate；不同 candidate 的证据不得合并。

## Change-plane Invalidation Matrix

| Changed plane | Evidence that becomes stale | Earliest gate to rerun |
| --- | --- | --- |
| Source, header, ABI or native implementation | affected static review, native target, dependent extension, focused result and all downstream qualification | exact source/CodeGraph review, then affected target build and selector |
| Build toolchain or dependency closure | built-runtime hashes, ABI/import evidence, native and Python results that load the runtime | native dependency preflight and fresh affected-target build |
| Test fixture, oracle, process driver or collector | selector result, harness reachability, trace/marker and qualification rows using it | test/harness static review and the smallest named focused selector |
| Effective configuration, identity, permission or topology | runtime result, source/config identity and any external submission evidence | configuration/source closure gate before execution |
| Model, tokenizer, ONNX, catalog or key artifact | model/semantic result, grant/admission result and dependent qualification rows | artifact preflight and the exact model-bound selector |
| SIF, Tiger bundle or remote submission content | external build/run/Slurm evidence and its promotion decision | external candidate preflight on the owner machine |
| Spec, plan, contract, selector or evidence rule | affected acceptance rows and any gate verdict relying on them | document consistency check and fresh design-code convergence audit |

## Promotion Rule

在运行完整 unit/integration、MiniNDN、no-Python、SIF、Tiger 或长时间 campaign 之前，
T006 必须把所有成员填入一个 candidate record，并通过 repository-owned closure gate。
候选未冻结、矩阵有 `gap`、子进程退出状态不全或 cleanup 未闭合时，禁止 promotion；
诊断运行可以保留原始边界，但不能成为 qualification evidence。
