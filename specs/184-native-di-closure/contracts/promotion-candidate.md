# Spec184 Promotion Candidate Contract

**Status**: FROZEN_LOCAL_PARTIAL / candidate identity recorded; no candidate promoted
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

当前 `DEV-865e1ee2` 仅用于把 B1–B4 focused evidence 和 T006 行绑定到同一开发 checkpoint。
它没有完整的 source manifest、effective dependency/configuration、model/tokenizer、harness
和 submission bundle 摘要，因此不能用于 T007 promotion；T006 必须在 fresh convergence
audit 前生成真正的 ordered candidate digest。

## Current Local Candidate Record

The following record freezes the current local native build for convergence review. It is a
development candidate, not a promotion decision: full unit coverage is clean, while the full
integration sweep and process/no-Python qualification still have open rows.

| Member | Bound value |
| --- | --- |
| `candidateId` | `sha256:02ec5ea062946cedef44c01fe0e7cb23c2e90ef2edbed78f9ce0bd3bb5d4f0f2` |
| `source` | `Experimental` product source commit `760724683a1d73ccc4498a25fc314074e230714a`; tree `20464239295733fd96e5c4ff0181ec827fb00d8c`; product source clean at commit; pre-existing `docs/failure-log.md` and integration marker are outside this candidate |
| `built runtime` | `unit-tests` `sha256:7a8375e44999a4447f8352cb3c5092a21f7426d5bd472ff7adf0db122016b422`; `integration-tests` `sha256:aad4846909b088f6285a3dcf6352482fec416ee89ce1822fbdf839783d4c8212`; `DI_NativeOnnxAssemblyWorker` `sha256:336ce668a0924ba03aafe6f077bc1b54cb77e4f6d5c6bd4200fb6fbd58ce31b7`; requester/provider/shared runtime rebuilt in `build-spec184-b3-examples` with hashes recorded below |
| `requester/provider/shared` | `DI_NativeRequester` `sha256:19e5affcd01ec2082fab3275be74d16aef95e7fa00e73e567879cea02c878ea4`; `di-native-provider` `sha256:9de63ae36bde70e13a90df88d5b495195c8854199dca904805a5cc6670b9e097`; `libndnsf-distributed-inference.so` `sha256:0956548999e0e9279802bc05e981621fdb04c80d9fe31e9f6060370065bb2881` |
| `test/replay harness` | `run-spec182-native-closure.py` `sha256:1578ec962de2d9742c537213579b80dc9a3553b6091143a11de8376bccd7d9af`; frozen manifest `sha256:855c29a0076807e4af6ee1f6445e2b6419bf3fa441399e49ea1879c5f459e4b3`; Python harness regression `71 passed` |
| `submission bundle` | `LOCAL_ONLY / not packaged`; SIF/Tiger bundle remains `TRANSFERRED` to the external owner and is not part of local qualification |
| `effective configuration` | `/usr/bin/g++ -B/usr/bin`, `/usr/bin/ld.bfd`, system Boost 1.71; Waf locks `.lock-spec184-b5` `sha256:36812dce931d032390502ef1838a48abc308cf67e21f59b0905a05cdfe02de83` and `.lock-spec184-b3-examples` `sha256:87806c87778639f8bb877cab268ab9035026743c26f441d3bc8e4fb44c1be284`; NAC-ABE, NDN-SVS and ONNX prefixes are the pinned paths in the build evidence |
| `local external artifacts` | assembly fixture `sha256:5e035fccaa7fecc0fc5fe272a19ec52c5c7e3163f83a21405dfef9e2a6e30165`; tokenizer vectors `sha256:6f44a2e62bebccd4dee2ff214d4d65f99528899e6863705eb1ec431b098ed34b`; tiny tokenizer `sha256:87e7e9653a645297f0b9ae6d51b1c497781219f56c094d8224d5500c7d44fd65`; SIF/Tiger/model campaigns are `TRANSFERRED` or `NOT_RUN` |
| `validation contract` | qualification matrix `sha256:bd26c91f167a6451b28585130a22e92fe6828417767306e20683795cc1f7bfc0`; Spec/plan/tasks hashes `2dd2316d24d5b387a04802ff6d90cecdbcb2e6cac7748f120f9ae4754b7c4045`, `ddf233a83f8fe8efcc1194b02a9b0e7990775433d062d2d218f39ac52c841bab`, `ca2559620a0a965b59971f7630296c1124b9b41ca118b9576a07496ac10943a0` |

The ordered candidate digest is computed over the rows above plus the exact harness/build/log
members recorded in this table. Full-unit output is
`.codex-tmp/spec184-b5-full-unit-20260911.log` (SHA-256
`143ecc81f846b9ef888e37563560aecd2d67bfae88f5378c9e34ae6902d167e3`); the full integration
sweep is `.codex-tmp/spec184-b5-full-integration-20260911.log` (SHA-256
`5244434ca84d77f2b6ae6909d237d96cf1f3a34b9282b14a090e85e173701793`) and ended with 48 failures.
The candidate therefore remains `FROZEN_LOCAL_PARTIAL`; any source, configuration, harness,
contract or external artifact change invalidates this identity before the next qualification run.

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
