# Spec184 Promotion Candidate Contract

**Status**: FRESH_LOCAL_PARTIAL / candidate frozen for local qualification only; no candidate promoted
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

## Previous Local Candidate Record

The following record froze a local native build for the pre-`DYNAMIC-LOOP` convergence and bounded
owner qualification review. It is a historical development candidate, not a promotion decision:
full unit coverage and one `PO-001-stream` owner case were clean, while the full integration sweep
and the remaining process/no-Python qualification had open rows.

| Member | Bound value |
| --- | --- |
| `candidateId` | `sha256:f7ee8f65a67a375f993e4db3a7558f22e996b8b170415b7d1325be89e3f32441` |
| `source` | `Experimental` product source commit `760724683a1d73ccc4498a25fc314074e230714a`; tree `20464239295733fd96e5c4ff0181ec827fb00d8c`; product source clean at commit; pre-existing `docs/failure-log.md` and integration marker are outside this candidate |
| `built runtime` | `unit-tests` `sha256:7a8375e44999a4447f8352cb3c5092a21f7426d5bd472ff7adf0db122016b422`; `integration-tests` `sha256:aad4846909b088f6285a3dcf6352482fec416ee89ce1822fbdf839783d4c8212`; `DI_NativeOnnxAssemblyWorker` `sha256:336ce668a0924ba03aafe6f077bc1b54cb77e4f6d5c6bd4200fb6fbd58ce31b7`; requester/provider/shared runtime rebuilt in `build-spec184-b3-examples` with hashes recorded below |
| `requester/provider/shared` | `DI_NativeRequester` `sha256:19e5affcd01ec2082fab3275be74d16aef95e7fa00e73e567879cea02c878ea4`; `di-native-provider` `sha256:9de63ae36bde70e13a90df88d5b495195c8854199dca904805a5cc6670b9e097`; `libndnsf-distributed-inference.so` `sha256:0956548999e0e9279802bc05e981621fdb04c80d9fe31e9f6060370065bb2881` |
| `test/replay harness` | `run-spec182-native-closure.py` `sha256:1578ec962de2d9742c537213579b80dc9a3553b6091143a11de8376bccd7d9af`; frozen registration manifest `sha256:855c29a0076807e4af6ee1f6445e2b6419bf3fa441399e49ea1879c5f459e4b3`; current runner manifest `sha256:2e18ac968911a722050e9ae04cf6fb69a493fbad15873a6c7c8aaaaadc6b8860`; owner result `sha256:a71da8553d398ea56a82f02b0059cfd711d73f85f869bbf23f07357018c351b4`; node context `sha256:395df68cc41d60899db76e469bd0a13ca9d7ba295b37a69c7d0b06d00c78324c`; trace `sha256:b952007353dcc7e04a0a06f508020c4d07e3b882ddf640b18b0ae6225026f45e`; stdout marker log `sha256:ffa3497adf522401c85fd65af75c0c80ec9b3ac599267e92acb8bbf539987025`; Python harness regression `71 passed` |
| `submission bundle` | `LOCAL_ONLY / not packaged`; SIF/Tiger bundle remains `TRANSFERRED` to the external owner and is not part of local qualification |
| `effective configuration` | `/usr/bin/g++ -B/usr/bin`, `/usr/bin/ld.bfd`, system Boost 1.71; Waf locks `.lock-spec184-b5` `sha256:36812dce931d032390502ef1838a48abc308cf67e21f59b0905a05cdfe02de83` and `.lock-spec184-b3-examples` `sha256:87806c87778639f8bb877cab268ab9035026743c26f441d3bc8e4fb44c1be284`; NAC-ABE, NDN-SVS and ONNX prefixes are the pinned paths in the build evidence |
| `local external artifacts` | assembly fixture `sha256:5e035fccaa7fecc0fc5fe272a19ec52c5c7e3163f83a21405dfef9e2a6e30165`; tokenizer vectors `sha256:6f44a2e62bebccd4dee2ff214d4d65f99528899e6863705eb1ec431b098ed34b`; tiny tokenizer `sha256:87e7e9653a645297f0b9ae6d51b1c497781219f56c094d8224d5500c7d44fd65`; SIF/Tiger/model campaigns are `TRANSFERRED` or `NOT_RUN` |
| `validation contract` | qualification matrix `sha256:8eb349fcfd97d7fe7dc99b7938be15834100df2e83afd300b7a3dfec21f03d6c`; Spec/plan/tasks hashes `2dd2316d24d5b387a04802ff6d90cecdbcb2e6cac7748f120f9ae4754b7c4045`, `ddf233a83f8fe8efcc1194b02a9b0e7990775433d062d2d218f39ac52c841bab`, `07081da742ca735f261d79a48efbbef9e3ea91ae07be8c0e28575036c462b803` |

The ordered candidate digest is computed over the rows above plus the exact harness/build/log
members recorded in this table. The corrected owner run is
`.codex-tmp/spec184-b5-owner-probe-20260911-r2/result/runner-result.json` and evaluated `PASS`
for the bounded `PO-001-stream` case. Full-unit output is
`.codex-tmp/spec184-b5-full-unit-20260911.log` (SHA-256
`143ecc81f846b9ef888e37563560aecd2d67bfae88f5378c9e34ae6902d167e3`); the full integration
sweep is `.codex-tmp/spec184-b5-full-integration-20260911.log` (SHA-256
`5244434ca84d77f2b6ae6909d237d96cf1f3a34b9282b14a090e85e173701793`) and ended with 48 failures.
The candidate was `FROZEN_LOCAL_PARTIAL` at capture time; any source, configuration, harness,
contract or external artifact change invalidates this identity before the next qualification run.

The 2026-09-11 `DYNAMIC-LOOP` workflow revision changed the Spec184 validation contract after
this candidate was frozen. The recorded `candidateId` and its runtime evidence remain a historical
record of the prior contract, but are stale for new qualification decisions. A fresh convergence
audit must recompute the ordered digest from the new `spec.md`, `plan.md`, `tasks.md` and matrix
hashes before T007 resumes; no runtime result is silently re-bound to the old candidate.

## Fresh Local Candidate Record

This record binds the current C++ fixes, parser sample, candidate target closure and fresh native
unit/integration sweep. It is a local qualification candidate; the current-user owner preflight,
sanitizer leak and external SIF/Tiger rows remain open, so it is not a promotion decision.

| Member | Bound value |
| --- | --- |
| `candidateId` | `sha256:f5c6fd40b26b38737b3e35ae742b2f9efd106cfebafa2b78eeffa00f7e2b036f` (digest over the ordered member map below; recompute if any listed member changes) |
| `source` | `Experimental` baseline commit `f7079c52b8deebc3fd0811d821324700f47b809a`; explicit candidate diff includes `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp` (`sha256:37984face728f908337ece2c59b7c96cd288969bad84a6f8ae6468d691100149`), `NativeEpochCoordinator.cpp` (`sha256:fb0a2513c33f1f26d4805ee1721cd172bb9c06b4f3c40982b818bf2946d04866`) and parser test (`sha256:3e9bc6df090479e18bab1269f1976a549fcf5a4f77cdf9bb90e13a5cd0cb733f`); pre-existing `docs/failure-log.md` and integration marker remain outside the candidate diff |
| `built runtime` | `build-spec184-b5-candidate`; `unit-tests` `sha256:985057943a5fd28d5aaaa3d7198cdbb035247c5b2f9a146650884cdb8681aae7`; `integration-tests` `sha256:8388ab586e2c3d544364065d67b757cc091ae185e6e73b1385a975d3651088b4`; `DI_NativeOnnxAssemblyWorker` `sha256:e44bd0a31205d3106461d6d48884da70adb4e8acc8ef5fea8753573964aff283`; `DI_NativeRequester` `sha256:7ca9f17803649539df78eaa8d03fa66a84ca1fa6e5098b8d21a1b426b5ef4c50`; `di-native-provider` `sha256:adcb740d4116a66204ab9b9721dbfd4b6364d08cfebdad01b2dfee78bb0691a2`; `libndnsf-distributed-inference.so` `sha256:b0f0204c3b93b246c2d16e8afcfbaf0c65ab814b283f66ee8ed85252aa5d9351`; `libndn-service-framework.so` `sha256:27c988b523058d1e14de10ed5c0e8f64d74d57678491bee81b0c71777718734b` |
| `test/replay harness` | `run-spec182-native-closure.py` `sha256:1578ec962de2d9742c537213579b80dc9a3553b6091143a11de8376bccd7d9af`; frozen registration `sha256:855c29a0076807e4af6ee1f6445e2b6419bf3fa441399e49ea1879c5f459e4b3`; fresh runner manifest `sha256:4212ca6f81913f30c10c23b1a5d3fcfa6d6a172de295b6e5a67fd3b2c23a7826`; root owner result `sha256:e65fc1b507fe40cc275601b031c724b99f64a254ecaaa51272a7845f8e5509fd`, runner result `sha256:7cc45f6f9d4487ffe45de90dff37c6bc2e28e030295d69e6b31aa2c48186a0b9`, node context `sha256:6f0cc75d38e21c746393a95a31ba6154fd906640172c480740e9117024b0d50b`, trace `sha256:703a98c5020812bf3aa83d295104c0f8024164b9a4fd68bb09d6d5634bcf6503`; parser test source remains bound by the `test.parser` member |
| `submission bundle` | `LOCAL_ONLY / not packaged`; SIF/Tiger remains `TRANSFERRED` and is not locally qualified |
| `effective configuration` | `/usr/bin/g++ -B/usr/bin`, system Boost 1.71, `.lock-spec184-b5` `sha256:d1eb664437f0f6f21449d2ea22d50aa42053923a22e8b0fc453b287d9b9c5482`; NAC-ABE, NDN-SVS and ONNX prefixes pinned in the build command; candidate-first `LD_LIBRARY_PATH`; worker lookup requires `NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate` |
| `external artifacts` | frozen observed-offer vectors and local assembly/tokenizer fixtures retained; real model, SIF and Tiger artifacts remain `NOT_RUN` or `TRANSFERRED` |
| `validation contract` | current Spec184 `spec.md`, `plan.md`, `tasks.md`, qualification matrix and caller matrix; ordered member-map digest is recorded above; fresh unit log `sha256:127d751be7522d0f7b3c8b3968bae393ae7d08082debf0b6d6bb65dfe6fcbb4d`, integration log `sha256:f0fccf442f6de69ab6a5e585e1eb84eb470aa1fad49e2316d6ca0210c2962027`, parser log `sha256:2106e52606f7e82e04974d92c2872015a196f6fdfe02f095c4c01502e7a53d55` |

The fresh full C++ unit and integration executables both exited `0`. The fresh owner probe used
the same runner manifest but stopped before MiniNDN setup at `MININDN_REQUIRES_ROOT` (uid 1000),
exit `2`; its result is `.codex-tmp/spec184-b5-owner-probe-20260911-r4/result.json` with
SHA-256 `004cfa0a88d00667c2c520c163a5c1461ec40a81fe3d39cacb703075aaf93fc0`. The prior root owner
PASS is retained only under the previous candidate record. Its I02 unsuppressed ASan/UBSan
`87,522`-byte / `720`-allocation LeakSanitizer result was initially classified as fixture-owned;
the later source review corrected the boundary to the production callback cycle and is recorded in
the following candidate. No external owner qualification is implied.

## Fresh Local Candidate Record (I02 Ownership Repair)

This record supersedes the previous fresh local record for new qualification
decisions after the production callback-cycle repair. It binds the repaired
`ServiceProvider.cpp`, rebuilt candidate binaries, fresh native sweeps and the
independent I02 sanitizer result. It remains `FRESH_LOCAL_PARTIAL`: process/no-
Python breadth, inherited negative rows, real-model/MiniNDN coverage, Python
retirement and external SIF/Tiger execution are still open.

| Member | Bound value |
| --- | --- |
| `candidateId` | `sha256:311d23ecf6b7c8fa8f1f69a309a5855b3f969844279a2250d4dcf9c1557b8a98` (ordered member map below; recompute if any listed member changes) |
| `source` | `Experimental` baseline `f7079c52b8deebc3fd0811d821324700f47b809a`; candidate production files: `ndn-service-framework/ServiceProvider.cpp` `sha256:a587b5bee0fe845ba4051ce0cdea6267e2cbc767b28055c156388ecea584e419`, `NativeProviderHandler.cpp` `sha256:37984face728f908337ece2c59b7c96cd288969bad84a6f8ae6468d691100149`, `NativeEpochCoordinator.cpp` `sha256:fb0a2513c33f1f26d4805ee1721cd172bb9c06b4f3c40982b818bf2946d04866`; pre-existing integration marker is bound separately and remains unstaged |
| `built runtime` | `build-spec184-b5-candidate`; `unit-tests` `sha256:39906e3c4a4ab1729f0c09c7146a3cb7284b0bd5a5a1e1dd668b97af305f9eeb`; `integration-tests` `sha256:e1947cfb5d75524edc346da5b9c32c984c2daf8e3504e095653e13201486a385`; worker `sha256:e44bd0a31205d3106461d6d48884da70adb4e8acc8ef5fea8753573964aff283`; requester `sha256:7ca9f17803649539df78eaa8d03fa66a84ca1fa6e5098b8d21a1b426b5ef4c50`; provider `sha256:0a13e7ce70d30be51242b04d252eb0aae11cdbb8abee259bcb2997df71746b18`; DI library `sha256:b0f0204c3b93b246c2d16e8afcfbaf0c65ab814b283f66ee8ed85252aa5d9351`; framework library `sha256:a781399f6e86054a13888f62860732bf469a639a298246664a1c2ae36f50d8ed` |
| `test/replay harness` | runner `sha256:1578ec962de2d9742c537213579b80dc9a3553b6091143a11de8376bccd7d9af`; registration `sha256:855c29a0076807e4af6ee1f6445e2b6419bf3fa441399e49ea1879c5f459e4b3`; integration source with pre-existing replacement marker `sha256:fd96dcc5331aaebbbf3eb944ae594ee59f43d8148baef883be80e0c71244169d`; runner manifest `sha256:4212ca6f81913f30c10c23b1a5d3fcfa6d6a172de295b6e5a67fd3b2c23a7826`; parser log `sha256:2106e52606f7e82e04974d92c2872015a196f6fdfe02f095c4c01502e7a53d55`; I02 sanitizer log `sha256:eb8b2cb46afedaf701b53a52a3e7fd7264fcf1b0cfc7f355b461403b0d4`; tiny-ONNX batch log `sha256:cc69c5f10ff39552d60a2eb0d107f56f9ce67dbe23937d8521f2cac86881630b` |
| `effective configuration` | `/usr/bin/g++ -B/usr/bin`, Boost 1.71, `.lock-spec184-b5` `sha256:d1eb664437f0f6f21449d2ea22d50aa42053923a22e8b0fc453b287d9b9c5482`; independent sanitizer tree configured with `.lock-spec184-i02-asan-r2` and `--with-sanitizer=address,undefined`; pinned NAC-ABE/NDN-SVS/ONNX prefixes from the build command |
| `validation contract` | Spec184 `spec.md` `sha256:60f2bf1abaeebc1895209c109d37ccfe3339a6fa39158b6dd6a0c86019ceb2cb`, `plan.md` `sha256:f2c12a569d714eb777e6cd0b0d97753f77ee1227a83209a6a043667ef4aa0a1c`, `tasks.md` `sha256:88b00139addc02596b171bddc0b91ffabf5183e0112f7a4c2bf83e6917572eee`, matrix `sha256:f9700aab012d4f89e254d82057cf49c5dbf81dcffdd9bd91f4e761f5c43f4766`, caller matrix `sha256:b2b079fd5f8e658aa6b2fec6bfec389830aa1c509fac4783f1f73e6c29c9a449`; shared `speckit-code-design` `sha256:ce9d126f646530e496ffdfd558fff70f81f8da0ae9ab91af6c22c51aae812b03`, batch gates `sha256:78c0192109eb1bde372aafab28655a2e22c0abfef69f3e4df0e4789cda5720df` |
| `results` | fresh unit `.codex-tmp/spec184-candidate-unit-20260911-r4/run.log` `sha256:5a48a71a9a3d09c3f762d915bd6a98dce6947b0fc07f35025bde362577764ca5`; fresh integration `.codex-tmp/spec184-candidate-integration-20260911-r2/run.log` `sha256:ab0911629ccd515acfc9b28664f5c3a20b63d7512c5039836801b7732c2d9367`; repaired I02 selector and 16-case tiny-ONNX ASan/UBSan batch exit `0`; logs `.codex-tmp/spec184-i02-asan-20260911-r2/run.log` and `.codex-tmp/spec184-tiny-asan-20260911-r3/run.log` end with `*** No errors detected` |

The ordered candidate member map used for `candidateId` is:

```text
binary.di=b0f0204c3b93b246c2d16e8afcfbaf0c65ab814b283f66ee8ed85252aa5d9351
binary.framework=a781399f6e86054a13888f62860732bf469a639a298246664a1c2ae36f50d8ed
binary.integration=e1947cfb5d75524edc346da5b9c32c984c2daf8e3504e095653e13201486a385
binary.provider=0a13e7ce70d30be51242b04d252eb0aae11cdbb8abee259bcb2997df71746b18
binary.requester=7ca9f17803649539df78eaa8d03fa66a84ca1fa6e5098b8d21a1b426b5ef4c50
binary.unit=39906e3c4a4ab1729f0c09c7146a3cb7284b0bd5a5a1e1dd668b97af305f9eeb
binary.worker=e44bd0a31205d3106461d6d48884da70adb4e8acc8ef5fea8753573964aff283
config.waflock=d1eb664437f0f6f21449d2ea22d50aa42053923a22e8b0fc453b287d9b9c5482
log.i02_asan=eb8b2cb46afedaf701b53a52a3e7fd7264fc1ceccf1b0cfc7f355b461403b0d4
log.integration=ab0911629ccd515acfc9b28664f5c3a20b63d7512c5039836801b7732c2d9367
log.parser=2106e52606f7e82e04974d92c2872015a196f6fdfe02f095c4c01502e7a53d55
log.tiny_asan=cc69c5f10ff39552d60a2eb0d107f56f9ce67dbe23937d8521f2cac86881630b
log.unit=5a48a71a9a3d09c3f762d915bd6a98dce6947b0fc07f35025bde362577764ca5
runner.manifest=4212ca6f81913f30c10c23b1a5d3fcfa6d6a172de295b6e5a67fd3b2c23a7826
skill.batch_gates=78c0192109eb1bde372aafab28655a2e22c0abfef69f3e4df0e4789cda5720df
skill.code_design=ce9d126f646530e496ffdfd558fff70f81f8da0ae9ab91af6c22c51aae812b03
source.NativeEpochCoordinator=fb0a2513c33f1f26d4805ee1721cd172bb9c06b4f3c40982b818bf2946d04866
source.NativeProviderHandler=37984face728f908337ece2c59b7c96cd288969bad84a6f8ae6468d691100149
source.ServiceProvider=a587b5bee0fe845ba4051ce0cdea6267e2cbc767b28055c156388ecea584e419
spec.caller=b2b079fd5f8e658aa6b2fec6bfec389830aa1c509fac4783f1f73e6c29c9a449
spec.matrix=f9700aab012d4f89e254d82057cf49c5dbf81dcffdd9bd91f4e761f5c43f4766
spec.plan=f2c12a569d714eb777e6cd0b0d97753f77ee1227a83209a6a043667ef4aa0a1c
spec.spec=60f2bf1abaeebc1895209c109d37ccfe3339a6fa39158b6dd6a0c86019ceb2cb
spec.tasks=14f2363da4878f6d13dd3af49a648f87a9ec0af3606aa6840cba6077d984acc8
test.integration=fd96dcc5331aaebbbf3eb944ae594ee59f43d8148baef883be80e0c71244169d
test.parser=3e9bc6df090479e18bab1269f1976a549fcf5a4f77cdf9bb90e13a5cd0cb733f
test.registration=855c29a0076807e4af6ee1f6445e2b6419bf3fa441399e49ea1879c5f459e4b3
test.runner=1578ec962de2d9742c537213579b80dc9a3553b6091143a11de8376bccd7d9af
```

`sha256:311d23ecf6b7c8fa8f1f69a309a5855b3f969844279a2250d4dcf9c1557b8a98`
is the SHA-256 of these sorted lines followed by a final newline.

## Current Local Candidate Record (T007 Process Qualification Refresh)

This record supersedes the earlier local records for current qualification decisions. It binds the
fresh C++ process runs, no-Python binary inspection, and candidate-bound isolation counterexamples
to the repaired native source and current Spec184 documents. It remains `FRESH_LOCAL_PARTIAL`: the
bounded rows below pass, while broader negative/model breadth, Python retirement and external
SIF/Tiger ownership remain open.

| Member | Bound value |
| --- | --- |
| `candidateId` | `sha256:a6b4a43e100ea526f6d71ff38bf55036bab420712a1d3f19d4dcdeacb895c079` (digest over the ordered member map below; recompute if any listed member changes) |
| `source` | `Experimental` baseline `29411b5544cf7b0175dbaae2f65678a26ee80636`; production source hashes: `ServiceProvider.cpp` `sha256:a587b5bee0fe845ba4051ce0cdea6267e2cbc767b28055c156388ecea584e419`, `NativeProviderHandler.cpp` `sha256:37984face728f908337ece2c59b7c96cd288969bad84a6f8ae6468d691100149`, `NativeEpochCoordinator.cpp` `sha256:fb0a2513c33f1f26d4805ee1721cd172bb9c06b4f3c40982b818bf2946d04866`; pre-existing `docs/failure-log.md` and integration marker remain outside the source candidate |
| `built runtime` | `build-spec184-b5-candidate`; current unit/integration, Controller, Authority, requester, Provider, worker and grant-requester hashes are bound below |
| `test/replay harness` | current runner manifest, C++ counterexample fixture, process drivers, C++ registration and process qualification evidence are bound below; Python harness is orchestration-only and its source regression is bound separately |
| `owner result` | root MiniNDN `PO-001-stream` result, runner-result and node-context digests are bound below; non-root `MININDN_REQUIRES_ROOT` remains an explicit boundary |
| `submission bundle` | `LOCAL_ONLY / not packaged`; SIF/Tiger remains `TRANSFERRED` and is not locally qualified |
| `effective configuration` | `/usr/bin/g++ -B/usr/bin`, system Boost 1.71, `.lock-spec184-b5` digest below, candidate-first library path, `NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate`, root owner PATH includes `/usr/local/bin` |
| `validation contract` | current Spec184 documents, shared dynamic-gate skill references, full C++ sweeps, no-Python inspection, process results and owner evidence below |

The ordered member map used for `candidateId` is:

```text
binary.authority=b0840be47108ff78c1241fe480689e167dc175e8bf9ce146b2622be01b3d792e
binary.controller=50667da49f9f190304fc2de09114b23bc46e0afba4fba143035bd2e5db0940d1
binary.di=b0f0204c3b93b246c2d16e8afcfbaf0c65ab814b283f66ee8ed85252aa5d9351
binary.framework=a781399f6e86054a13888f62860732bf469a639a298246664a1c2ae36f50d8ed
binary.grant=02c0e24da191fbb4fa70120862966c895aaa30339bddecd94e752b690b0af853
binary.integration=e1947cfb5d75524edc346da5b9c32c984c2daf8e3504e095653e13201486a385
binary.provider=0a13e7ce70d30be51242b04d252eb0aae11cdbb8abee259bcb2997df71746b18
binary.requester=7ca9f17803649539df78eaa8d03fa66a84ca1fa6e5098b8d21a1b426b5ef4c50
binary.unit=39906e3c4a4ab1729f0c09c7146a3cb7284b0bd5a5a1e1dd668b97af305f9eeb
binary.worker=e44bd0a31205d3106461d6d48884da70adb4e8acc8ef5fea8753573964aff283
config.waflock=d1eb664437f0f6f21449d2ea22d50aa42053923a22e8b0fc453b287d9b9c5482
evidence.convergence=ddaea2ef5f1369b1972e85fdda1a12131aca9ec109c8fa04fefe910e8b5f915d
evidence.remainder_audit=c00e914b4900eb9adb71ed4176153da70f0a96acab6a16a958f65191cde6869d
evidence.t007_current=0bfa7cac542d5c1964908c7530e11946d8e426da2d68ce64a6ea6d357d04fecc
evidence.t007_process=f934facb7ee16da3bda61fecf62c0b5d17440dd889832e05dd95e4a3954e714a
evidence.task_registry=7927a0c8538761b38a5199c732403729d2fa2e878b21902d75ce6d05e04aa240
log.build=c68038b4fd6973eb55a670907c4bc896bb8f8386cab1064602be714b7cd12fe3
log.integration=e679f2d0ace15827dfd7a5a423660965b0e583df65ba31d8b72370c65b846966
log.python_harness=c6d8232bea89456a18742e6c0bc663175121ea04cd4815356c198966f223a324
log.targeted_unit=66efc8f7b0015b31c337a11daabe3a34a3f4f1dcfba497f2c822949ab0586847
log.unit=b3e2f457d26fe019760f124cb225a65414cca2bc568c0603c4fc246fbd5686cd
owner.node_context=97d548d3a12e91718400faf604629f11fb2bbf44e0d1338d73ddc1d1be4e625f
owner.result=e65fc1b507fe40cc275601b031c724b99f64a254ecaaa51272a7845f8e5509fd
owner.runner_result=d3f757c37e196de6e616554e65eaea324b3e8706efe142c2d9cd8f673ccedcd0
runner.manifest=a1e7df48caf5837d690ad1847e3490704c309af739e0df318ef3062ac8fdfd13
skill.batch_gates=78c0192109eb1bde372aafab28655a2e22c0abfef69f3e4df0e4789cda5720df
skill.code_design=ce9d126f646530e496ffdfd558fff70f81f8da0ae9ab91af6c22c51aae812b03
skill.pre_test=6b5aff9458d1da93a6b0021ca5713aba58c14006654edef28744ed445259b486
skill.review_agent=1974ac9454ca727b264e22c391d5c98e1663622e31ca01825b12a67ad34cc16b
source.NativeEpochCoordinator=fb0a2513c33f1f26d4805ee1721cd172bb9c06b4f3c40982b818bf2946d04866
source.NativeProviderHandler=37984face728f908337ece2c59b7c96cd288969bad84a6f8ae6468d691100149
source.ServiceProvider=a587b5bee0fe845ba4051ce0cdea6267e2cbc767b28055c156388ecea584e419
source.integration=fd96dcc5331aaebbbf3eb944ae594ee59f43d8148baef883be80e0c71244169d
spec.caller=b2b079fd5f8e658aa6b2fec6bfec389830aa1c509fac4783f1f73e6c29c9a449
spec.matrix=b3fdb05bd1564d9ec4c118aee1e4231d6eca162cf7f362059358a3995b66de16
spec.plan=d4600caf4413e58bb5e69397e03f985956c87eb544ae6e70034fad980e40fc11
spec.spec=724f6aa1be1c6f151e5a71888f00bd0d4fecab1e002d07ba2c2f40473415eca1
spec.tasks=7899ef700c126635adfa375317336b1a6c62384e68a2aeb165526d169b8c96e6
test.counterexample_fixture=d8467fb94ec08a1a51f27d595b2acdb74f1dbe0d1f73154a8b733f1ae6345600
test.grant_process=63879098c789f72f80bcedf3261a8e0a44beef797740848490763289807b2813
test.python_closure=0604f301277097af1bf777f858316d016e85f8f9ffcb59dd2700ec9f9f92232c
test.registration=377a5d0ff62ed64b715f9d644646040cdf16790613ae9bc43b4910957da5149c
test.runner=3022d14fa413ed128203b46ad592cf5cc0a18903eae5c34afffd052990142658
test.stream_process=a2258b4a2747ab8eab3a560ee28f6e4caf4f526b82a4d7c94b2a9e10b4368b9d
test.unary_process=0ee4ee185fbbbcb450d7c8a6b6ac5b128c2c27e34d949926d23c3e2956798cc4
```

The SHA-256 of these sorted lines followed by a final newline is
`sha256:a6b4a43e100ea526f6d71ff38bf55036bab420712a1d3f19d4dcdeacb895c079`.
The process evidence records the individual requester/provider markers and grant-case hashes;
the candidate binds that evidence file rather than silently combining unrelated run roots.

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

## Current Local Candidate Overlay (2026-09-11)

这是对当前本地候选的运行覆盖，状态仍为 `FRESH_LOCAL_PARTIAL`，不是 promotion 决定。
候选 receipt 为 `build-spec184-b5-candidate-r4/spec180-native-build.json`，SHA-256
`f83d4499fc7271bdc205fb56d212e9688741b754e1b964ce8ecb3ccb2c0e99b1`；`verify` exit `0`，
系统编译器为 `/usr/bin/g++ -B/usr/bin`，Waf 使用 `-j4`，并记录 `binding_reused=true`。
候选运行时哈希包括：framework `fc47f9a10f7a5bc431fa7e59bb4cea7cabd365b4e1f930a1b99df0c439ad451b`、
DI `ee40993f5a7e0cd4a1d111225ede7aee397b88fb405114e32283e87641a6cab0`、
requester `c9e8dfa7d0eddd2c570dde4e4db23f4709a64e194135634b863bedaa36b1ae99`、
artifact authority `e8540479e6a23c97aacbfb40f10f57c8d4375a68ad140e7f8f328eb90405de33`、
worker `5675aba103f7df152742d56e3287f0e48e61ffe912f6c37e756f719d7bb86a27`、
provider `408bf0e83f260cb8ab51dee8b60c3fa8a27b46633569577c95694c74ec7af9ef`。

| Local row | Evidence | Result |
| --- | --- | --- |
| YOLO Y-A | `.codex-tmp/spec184-yolo-Y-A-output-20260911-r51/` | `PASS_FOR_ROW`; C++ numerical oracle matched, terminal response and cleanup complete |
| YOLO Y-B | `.codex-tmp/spec184-yolo-Y-B-output-20260911-r37/` | `PASS_FOR_ROW`; four Provider native path, ORT CPU execution and terminal cleanup complete |
| YOLO Y-N | `.codex-tmp/spec184-yolo-Y-N-output-20260911-r50/` | `PASS_FOR_ROW`; seven boundaries and three real Provider grant mutations rejected before assembly |

The current candidate is not promoted: this host can execute only `Qwen3-0.6B` smoke/ABI
fixtures, not the contract-required `Qwen/Qwen3.6-27B`; inherited negative/retirement rows,
I05 `UNQUALIFIED`, Python retirement and SIF/Tiger external ownership remain open. Any later
source, harness, contract, artifact or external configuration change invalidates this overlay
and requires a fresh candidate identity before qualification resumes.
