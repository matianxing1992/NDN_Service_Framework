# Incremental Build Audit

**Branch / baseline**: Experimental / `00b0cd501d7d8112d4b1158a52e4b302f05cea7a`
**Date**: 2026-11-11
**Scope**: 首轮静态诊断及随后获授权的修复；以下原始计数是修复前基线。

## Conclusion

增量缓存本身仍有效。当前Core/DI/Repo的42/99/16个编译任务签名全部匹配，
对应对象文件均存在。小改动的成本主要被重复源码目标、默认测试范围和公共头文件
依赖放大。现已完成修复与实际增量验证：改动一个普通生产.cpp只执行一次编译；
恢复后的无改动构建为零编译、零链接，耗时6.76秒。不能把原始潜在任务图称作
每次都执行的全量重建。

当前配置为core/repo/di/uav、WITH_TESTS=True、shared=True、static=False，输出仍是
本checkout的build目录。原六路并行不受本次分析影响。

## Confirmed Findings

### 1. 同一生产源码被多个target分别编译

只读展开当前注册图：227个task generators，484个不同C/C++源文件，2,703个编译任务。
这是潜在任务图规模，不是本次执行量，也不表示每次修改都会运行全部任务。

| Source | Compile targets |
| --- | ---: |
| ndn-service-framework/ServiceProvider.cpp | 6 |
| ndn-service-framework/ServiceUser.cpp | 5 |
| DI/NativeProviderHandler.cpp | 16 |
| DI/NativePlanning.cpp | 17 |
| DI/NativeExecutionPlanJson.cpp | 17 |
| AD/onnx/NativeOnnxRecipeAssembler.cpp | 19 |
| AD/onnx/OnnxRuntimeModelRunner.cpp | 17 |

DI=`NDNSF-DistributedInference/cpp/ndnsf-di`，AD=`NDNSF-DistributedInference/cpp/adapters`。
另一个同名的DI/OnnxRuntimeModelRunner.cpp只是兼容include，未把它的14个任务算成
上表中真实ONNX实现的编译次数。

根wscript:1005–1083先建立四个DI对象组，再把相同源码直接交给安装库编译。
四组共99个编译任务，当前图中没有任何use消费者；仓库脚本/测试/示例检索也只找到
这四处声明。安装DI库另外编译99个文件。因此这些对象组不是有效对象复用。

tests/wscript:862–873的生产源码集合被多个历史测试程序重复使用；1680–1741的
unit-tests直接编译整个Core/DI及部分Repo/UAV，达到358个源文件，integration-tests
也有184个。部分隔离、故障注入和sanitizer变体确实需要不同编译，不能全部混成同一对象。

### 2. 默认构建把日常开发和完整资格目标混在一起

scripts/build-safe.sh:7–8默认是build --jobs=6，没有targets；当前WITH_TESTS=True，
根wscript:998–999会登记测试，普通构建范围因此包含大量历史probe、测试和sanitizer目标。

完整图只找到430个已有编译UID签名；不能把其余任务都算成某次编辑引发的缓存失效，
它们也可能从未构建，或因目标编号变化产生了新UID。三套主要生产库的157个UID均命中。

无targets的图加载还在tests/wscript:1837–1847遇到spec192-repo-client缺少固定
ndn-cxx源码树的错误。明确请求其他target时已有跳过分支。这说明默认资格范围还会
让无关fixture前置影响普通开发；无需为此修改ndn-cxx。

### 3. 稳定数据类型和运行时实现放在同一公共头

NativeExecutionPlan.hpp:4为RedistributionSpec等数据引入AsyncDataflowRuntime.hpp；
后者同时定义TensorBundle、DependencyEdge、RoleSpec、RoleExecutionContext以及
线程/队列/锁等调度器私有状态。NativeModelRunner.hpp:4又引入整个该头。

对162个生产.cpp的项目内include图做静态估算：

| Header | Transitive .cpp users |
| --- | ---: |
| AsyncDataflowRuntime.hpp | 79 |
| NativeExecutionPlan.hpp | 66 |
| NativePlanning.hpp | 52 |
| NativeModelRunner.hpp | 45 |

这些数字不是编译器实测：分析未展开预处理条件，唯一basename回退可能产生误差。
但源码已直接证明依赖方向不够精简；修改调度器私有字段也可能牵连规划代码。
真实公共数据布局变化引发消费者重编译是必要的，不能以隐藏依赖来避免。

### 4. 巨大翻译单元让一次必要重编译也很昂贵

ServiceProvider.cpp为23,890行/1.22 MB，ServiceUser.cpp为19,167行/0.97 MB；
NativeProviderHandler.cpp也有4,896行。当前-O2 -g下，前两者单个对象约159/125 MiB。
这证明源码和产物体积大，不是已测得的编译耗时或峰值内存。

Waf本地实现还用按目录递增的task-generator idx命名对象（TaskGen.py:26–31、
Tools/ccroot.py:24–26）。在同一目录的构建脚本前面插入target，可能使后续对象编号
变化并重编；普通.cpp内容修改不会自己改变这个编号。固定target身份应纳入后续验证。

## Recommended Changes, in Order

1. **先统一对象所有权。** 安装库与四个DI对象组只保留一套编译：优先让安装库真正
   链接现有对象组，并补齐各组真实include/use/flags；不要保留对象组又重列source。
   若分组没有实际用途，则由安装库直接拥有源码并移除孤立对象组。不要为去重再建
   一套构建系统。OA02等必要独立进程的源码闭包保留。
2. **收敛测试编译。** 普通生产API测试链接已构建的Core/DI/Repo；需要HAVE_TESTS、
   故障注入、无Repo/无ORT、sanitizer的对象按真正不同的编译契约复用。特别核对宏、
   ABI、符号和instrumentation，不能把未插桩对象当作sanitizer覆盖，也不删原有测试。
3. **分开默认开发范围和资格范围。** 一个现有build目录内，默认开发只构建生产库/
   应用，相关测试显式按target选择；完整回归及sanitizer仍有维护入口。改变配置只做
   一次，不在每次编辑后反复configure/clean。保留六路及当前系统内存/swap设置。
4. **拆开稳定types和调度实现。** 将TensorBundle/依赖/必要context等契约移至轻量
   头，NativeExecutionPlan只依赖所需types；线程/队列/锁留runtime私有定义。
   保持命名空间与语义，原include作兼容入口；修正实际消费者的显式include。
   有真实隔离收益的类可用PImpl，避免为每个struct增加包装层。
5. **最后按行为拆大.cpp。** 沿现有request/ACK-selection、collaboration transport、
   streamed invocation等职责，把私有实现移至对应.cpp；保持公开接口和生命周期。
   优先拆经编译测量确认昂贵且经常修改的部分，不按行数平均切，也不做全库重写。

当前即可采用现有target选择缩小构建范围，例如：

```sh
./scripts/build-safe.sh build --targets=ndnsf-distributed-inference,di-native-provider,DI_NativeRequester,DI_NativeArtifactAuthority,NDNSF_RepoPacketServer
```

本轮已执行生产DI库、运行时应用和相关原生回归的显式目标构建及Waf正式安装；
MiniNDN运行仍消费正式安装的应用和库。
ccache目前未安装，可作为后续辅助；它不能解决重复所有权或头文件耦合，不应成为首项工作。

## Verification for an Implementation

- 同一候选第二次无改动构建：编译/链接任务均为0。
- 修改一个普通.cpp：每个必要编译变体只编译一次，只重链接实际消费者。
- 修改runtime私有实现：不重编规划/模型无关代码；改公共类型时保留必要消费者重编译。
- 新增一个测试target：既有生产对象身份不变，不出现全图编号漂移。
- 分别记录编译与链接数、wall/CPU、峰值内存、产物大小；完成必要原生回归及安装后
  MiniNDN CPU cold/warm，确认接口/授权/取消行为不变，再推送到Tiger。

## Method and Limits

复用scripts/spec201-build-closure.py中的restore/load_envs/recurse/post思路，只在内存
展开图；exec_command和store被替换为拒绝函数，无编译器执行和构建缓存落盘。
补齐独立诊断所需的Options绑定/日志初始化。为展开其余注册目标，明确选择DI target，
使已有逻辑跳过缺依赖的spec192 fixture，再post全部已注册generator进行计数。
因此2,703是该已注册图规模，不是一次targeted build的执行量。另做WITH_TESTS=False
的内存对照，得到340任务/233源文件；未修改持久配置。

## Reviewed Implementation Scope

复核方案后，本轮采用最小可验证改动：

- 四个DI对象组真正提供安装库对象，保留原库的对象index/UID，避免迁移本身强制重编
  未变源码。各组使用原安装库的编译契约；shared/static均复用同一份PIC对象。
- 默认build/install选择可安装产品和metadata；保留所有显式测试target及
  `--targets=*`，安装仍post公共headers和pkg-config。默认路径跳过无关缺依赖fixture。
- 仅将已确认不含特殊DI seam/sanitizer宏的两个spec181测试改为链接生产DI库；
  其余不同宏/flags变体原样保留，不为减少数量改变测试含义。
- 将原有dataflow数据类型原样移到dataflow-types.hpp，旧runtime头仍提供兼容入口；
  plan/runner只include所需types。无字段、签名或对象布局变化。
- 暂不拆ServiceProvider/ServiceUser巨大.cpp：这不是关闭已复现重复/默认范围问题
  的必要前置，且尚无单TU编译耗时定位。后续只有匹配增量测量仍指向它们时再做
  对应行为拆分，不展开预防性重构。

先加入真实只读Waf图回归：修复前两个重复编译/链接断言失败，默认build/install还
会触发无关spec192依赖错误；修复后扩展为六项并全部通过。该检查不启动编译器，也不冒充C++行为
通过。构建脚本读取已固定KVCache分支对应实现：重复所有权仍存在，selected-install
的header保护已在当前分支，直接保留并补metadata检查；未改外部Waf或NDN依赖。

预存四份skill修改、模型、单一build配置及无关未跟踪文件保留。

## Measured Verification

| Check | C/C++ compilations | Links | Wall time | Result |
| --- | ---: | ---: | ---: | --- |
| First migration build: DI runtime and three native suites | 93 | 10 | 594.35 s | Pass |
| Remaining type-only header consumers migrated | 51 | 7 | 452.25 s | Pass |
| Transport fixture lifetime correction | 1 | 1 | 71.27 s | Pass |
| Comment-only change to NativeProviderReadiness.cpp | 1 | 0 | 19.63 s | Pass |
| Remove temporary comment and restore source | 1 | 0 | 19.84 s | Pass |
| Final unchanged runtime/test target set | 0 | 0 | 6.76 s | Pass |
| Waf installed runtime/SDK targets | 0 | 0 | 7.525 s (Waf) | Pass |

All compilation used the existing build/ with six workers and no imposed memory/swap limit;
system swap settings were unchanged. The comment probe produced unchanged object content,
so Waf correctly skipped linking. This proves its compilation scope, not a link-time speedup
for a behavioral edit. No-op wall time still includes Waf startup and graph/signature checks.
The first two builds' maximum RSS values were 3,041,368 and 2,801,036 KiB respectively:
these are the largest child process reported by time, not aggregate concurrent memory peaks.
User/system CPU times, targets, configuration/source hashes and commands are retained in
[the structured summary](build-incremental-summary.json).

The current default graph has 219 C/C++ compilation tasks, versus the baseline registered
graph's 2,703. This is a scope reduction, not a measured 12x speedup or proof that all 2,703
previously executed. Default output collisions: zero. Production DI now compiles each source
once, and only AsyncDataflowRuntime.cpp directly includes the scheduler header; stable public
types remain available through the old compatibility include.

Verification: six build-graph checks; 54 native-plan cases (707 assertions), 45 protected-runtime
cases (614 assertions), and 68 tensor-transport cases (2,535 assertions) passed. The installed
SDK compiled and ran a C++ consumer outside the checkout, exercising attempt admission and
cancellation and linking /usr/local/lib/libndnsf-distributed-inference.so. The new public header
and pkg-config metadata were installed; /usr/local/bin/ndnsf-di --help also passed.

The first transport run failed with SIGSEGV in LargeTensorUsesBoundedSignedPackets at fixture
checkpoint 1606 (67/68 cases). Static inspection found ordinary signal Connections capturing
temporary fixture state; these now use ScopedConnection, matching the installed ndn-cxx
lifetime contract. KVCache has the same unscoped fixture and no corresponding fix to port.
Focused gdb did not reproduce the original crash, so its unique cause is unproven. After the
correction the full suite and three final focused repetitions (413 assertions each) passed.
The necessary original failure excerpt is preserved in the summary; the first binary hash
was not captured and that historical attempt remains failed.

Not run: full default installed-target build, static-library compilation, MiniNDN Qwen inference
or Tiger deployment. The dataflow types and runtime class bodies were moved unchanged; this
task qualifies the build/SDK and focused native behavior, not new CPU/GPU inference results.
Before deploying these changes to Tiger, perform the existing installed MiniNDN cold/warm gate.

After verifying the aggregate and failure excerpt, the task-owned /tmp/ndnsf-incremental-build-1000
logs and consumer probe were removed. Full raw logs/per-sample traces are not retained for
recomputation. Existing build outputs, models, installed artifacts and unrelated edits were kept.
