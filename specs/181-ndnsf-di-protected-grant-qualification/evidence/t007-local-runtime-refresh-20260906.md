# Local Runtime Identity Refresh

**Status**: PASS (focused application source closure; T007 remains BLOCK)
**Evidence layer**: implemented inspection / executed (native build and application preflight)

## Subject and Boundary

隔离 checkout 已从 `1ba99000` 更新至 `a51f87b3`，tracked tree
干净，仅保留既有未跟踪 build-system-j2。旧 receipt 和构建日志
已冻结于 [native projection R3](t007-native-plan-closure-20260906.md#committed-native-build-r3)，
不覆盖其 raw run。主工作区原有 128 个 tracked 修改和 626 个
untracked 条目保留；本次不合并 Git 分支。

当前原始目录为 ignored workspace temporary directory 的
`spec181-local-runtime-refresh-20260906-r1/`。使用维护
`scripts/spec180_native_build.py build --jobs 2`，构建 root 指向该
隔离检出，Python 为 `/usr/bin/python3`，显式 PATH 为
`/usr/bin:/bin:/usr/local/bin`，PYTHONPATH 仅为该检出的 pythonWrapper。
CFLAGS/CXXFLAGS/CPPFLAGS/LDFLAGS 保持未设置，沿用已配置 Waf，
setup toolchain 仍由维护 owner 固定；不增加 LD_LIBRARY_PATH。

验收为真实 native 构建、扩展导入/映射库和新 Waf identity receipt；
之后在实际应用 child 环境做只读 verify。此为 T007 构建/runtime
闭包核查，不是正式模型/MiniNDN、SIF 或 Tiger 资格。

## Native Build R1

维护构建 exit 0，Waf **3.203 s**，随后实际编译/链接 Python 扩展，
完成导入/库映射校验并输出 `SPEC180_NATIVE_IDENTITY_OK`。新 receipt
已复制到 R1 `native-build-receipt.json`，SHA-256 为
`3da40b6db991934431123f15ca5970189649624c073190911d646b1239397387`。
其来源是 `a51f87b3b218f0fcd26a1ee2da821d644fb5656e`，不是主工作区脏树。

## Application Preflight R1

`inspect-application-preflight.py` 使用维护 runner 的真实
`validate_inputs`/受保护 Y-B 入口，计划在实际 native guard 成功后
停止；网络启动被显式禁止。输入引用既有本地模型与 key map，
运行状态/输出及 probe PIB/TPM 为本次独有目录，不修改 operator PIB。

实际在 `_validate_package` 的 catalogue verify 阶段 exit 1：
`ImportError: cannot import name 'build_yolo26n_adapter' from 'ndnsf_distributed_inference.adapters.yolo'`，
包装为 `CANONICAL_CATALOGUE_VERIFY_FAILED`。这是干净源码检出的
Python adapter 交付缺失，尚未执行 native guard 或启动网络，
不能分类为协议或模型失败。R1 脚本/日志与 native receipt 均保留。
下一步核对提交中的 adapter 包与主工作区缺失源，补齐一致源码后
重新导入；T007 保持 BLOCK。

## Application Preflight R2

选入主工作区已存在的四个 YOLO 包文件：`__init__.py`、
`candidates.py`、`graph.py`、`reference.py`；它们此前均未进入 Git。
审查确认这些文件负责既有 adapter 导出、签名目录、ONNX 图端口
及固定数值 oracle，没有引入另一套公共 runtime。R2 在隔离检出
加这些明确源差异后，实际 catalogue verify 已通过。

下一边界在生成 case policy 导入 Repo 客户端时发生：
`ModuleNotFoundError: No module named 'py_repoclient._py_repoclient'`。
该检出尚未构建 Repo 自身 Python 扩展；没有 native guard 或网络
启动。R2 脚本/日志保留，不能将本次结果当作完整源码闭包或资格。
下一步核对 Repo 维护构建入口和源码依赖，以同一检出构建该扩展，
同时验证选入的 YOLO 文件；不从脏主工作区复制未知来源 `.so`。

## Repo Build Selection R3

Repo 维护入口为 `NDNSF-DistributedRepo/pythonWrapper/setup.py`，
源/头文件在隔离检出的 tracked tree 中。系统 pkg-config 的 SVS
默认前缀为 `/usr/local`，而 framework receipt 选择
`/home/tianxing/NDN/ndn-svs` 与其 `build`；构建时必须保持该配对。
R3 使用 `/usr/bin/python3 setup.py build_ext --inplace`，显式
`NDNSF_LIBRARY_DIR` 为隔离 build-system-j2，CC/CXX/LDSHARED 使用
`/usr/bin` GCC/G++ 的 `-B/usr/bin`。CFLAGS 前置上述 SVS 源/build
include，LDFLAGS 前置该 build 的 `-L` 与 RPATH。实际编译/链接
向量记录于 R3 `repo-build.log`，最终 exit 0，结果见下一节。

## Application Preflight R3

Repo 维护构建 exit 0，实际编译/链接向量确认 selected SVS 的 include、
library search 与 RPATH 排在安装默认值之前，framework 来自同一
隔离 build-system-j2。之后实际 Repo import 与 case policy 生成
通过；下一失败发生于公共 `SplitCandidate` 构造：YOLO adapter
传入 `selection_priority`，提交内契约却缺少该字段，包装为
`CASE_RUNTIME_PUBLICATION_RUNTIME_IDENTITY_FAILED`。没有启动网络。

R3 application 日志/脚本与 Repo build 日志保留。下一步只补齐
已被 adapter 消费的公共候选优先级契约及其校验，核对既有工作区
差异，不能把无关生成/量化扩展整批带入此源闭包单元。

## Application Preflight R4

隔离检出补齐 SplitCandidate 的五个 adapter 字段及角色校验，保留
原 hybrid 验证规则；公共 digest 自动覆盖 dataclass 全字段，已提交
coordinator 按签名 priority 排序，并消费 ingress/egress/后处理。
R4 已越过候选构造，随后在 SDK 导入 Provider 时因公共 adapter 缺
`MAX_INLINE_INPUT_BYTES` 而 exit 1；尚未到 native guard 或网络。
原始目录 `spec181-local-runtime-refresh-20260906-r4/` 保留脚本/日志。

已提交 Provider 和 coordinator 分别依赖 inline 限额及
`InputTransportMode`，主工作区既有 base.py/包导出差异正是缺失的
公共输入传输契约。下一步闭合该依赖并验证实际应用导入；T007 BLOCK。

## Application Preflight R5

补入公共 ApplicationInput 传输模式与包导出后，R5 exit 1：
`LargeDataReference` 尚未进入已提交 repo_reference.py。该模块的
既有差异提供同一 native publication 的元数据绑定与输入引用校验，
已提交 Provider/client/facades 均消费它。下一步补齐该公共转换
owner，不新增 Repo 操作服务。R5 脚本/日志保留；无网络启动。

## Application Preflight R6

Repo 引用 owner 补齐后，R6 已完成 SDK/Provider 导入；运行时目录
发布在 `PreSplitCatalogSnapshot.from_mapping` 缺失处 exit 1。
已提交 runner 调用此公共转换，主工作区 contracts.py 的全部差异
只涉及该类型的字段校验和双向转换。下一步纳入该配套契约并重验。
R6 脚本/日志保留；尚未到 native guard 或网络。

## Application Preflight R7

公共目录转换补齐后，R7 通过 runtime publication，首次进入实际
process_specs；旧本地 helper `python_cmd` 不接受 runner 已使用的
`repo`/`py_dir` 参数而 exit 1。现有 legacy helper 差异仅参数化
源码目录、输出/cache 和节点身份，供公共 runner 复用。下一步
纳入配套 helper，保持其默认旧调用兼容；R7 日志保留，无网络启动。

## Application Preflight R8

本地 helper 补齐后，R8 已通过实际 publication、process_specs 与
native guard；guard 成功后主动停止，未启动网络。后续逐个导入
真实应用入口时，user.py 依赖 SDK 的 `ProviderOfferTrustVerifier`
包导出尚未提交，exit 1。下一步补齐已存在 verifier 的公开导出。
R8 脚本/日志保留；native guard 通过不能代替全部应用入口导入。

## Application Preflight R9

R9 验证导出后发现 verifier 实现本身也未进入 SDK provider.py，
导入 exit 1，日志保留。补齐实现需同时核对其候选/身份/签名/ACK
绑定与既有定向测试；只加包导出不足以关闭源依赖。该文件其余
差异为 public APPProvider.start 委托及准确的生命周期说明，均为
已有应用的公共入口；下一步一起验证该文件的配套实现。

## Application Preflight and Focused R10

R10 实际 application preflight exit 0：publication/process_specs/
native guard 及 user/provider/controller/repo_node 的真实导入全部
通过，输出 `APPLICATION_PREFLIGHT_IDENTITY_OK` 与依赖文件摘要。
没有启动网络；测试对象仍为 a51f87b3 加明确选入的源差异。

同一隔离检出的六文件定向检查 **9 failed / 36 passed（1.38 s）**。
失败分别是已提交 coordinator 使用 `DIRequestEnvelopeV2.input_transport`
而字段未提交，以及 InferenceApplication.request 未提交公共 task
参数。这是输入契约两端配套缺失；下一步补齐相应 wire/APP 委托，
保留 R10 focused.log 后重验。实际 import PASS 不等于行为通过。

## Focused Closure R11

两端公共请求契约补齐后，相同六文件 **45 passed（0.74 s）**；
新增候选 digest/priority/角色边界回归 **12 passed（0.44 s）**。
实际生产 preflight 再次 exit 0，记录 `networkStarted=false`，
真实 user/provider/controller/repo_node 与六个运行依赖导入通过。
R11 `application-preflight.json` SHA-256：
`27cd99f431627fc0b092ae9c62ab95b819bbcba512eb871367518382041b1b32`。
同源 Repo 扩展 SHA-256：
`93deffb77772a18e5b3c77ee7e264352221bf4179e47d462887922f2a28b1609`。

### Selected Source and Reproduction

本单元选入四个 YOLO 包模块、公共 SplitCandidate 字段与验证、
ApplicationInput/Repo 引用/wire/public request 配套、目录转换、
Provider-offer verifier 与 SDK 导出，以及本地 helper 显式参数。
沿用同一 planner/native helper/Repo owner。splitter.py 的 tensor-degree-one
HybridPlan 放宽差异不在本单元，主工作区其他修改保持原状。

定向检查在隔离 checkout 使用 `/usr/bin/python3 -m pytest -q`；
PYTHONPATH 依次为该 checkout 的 NDNSF-DistributedInference、
NDNSF-DistributedRepo/pythonWrapper、pythonWrapper。PIB/TPM/transport
均指向 R11 专有路径，未使用 operator 身份。六文件为：

```text
tests/python/test_spec180_generic_request_api.py
tests/python/test_spec180_yolo_ack_planning.py
tests/python/test_spec180_provider_offer_trust.py
tests/python/test_spec180_catalog_resolver.py
tests/python/test_ndnsf_di_model_family_adapter.py
tests/python/test_ndnsf_di_presplit_catalog.py
```

新增检查为 `tests/python/test_spec181_candidate_source_closure.py`。
生产路径检查命令为 `/usr/bin/python3 <R11>/inspect-application-preflight.py`；
脚本显式设置完整应用环境，使用实际 runner 的 catalogue、runtime
publication、process_specs 和 native guard，在网络启动前停止。
运行目录一次性使用，不覆盖或复用旧 case/state。

### Remaining Boundary

57 项定向检查和应用预检只关闭选入的 Python 源依赖单元；测试时
对象为 a51f87b3 加明确差异，最终提交仍须核对同源 checkout。
native receipt 来自同一 native 源构建，Python 模块入口摘要不等于
整个第三方 distribution 的文件证明。T007 仍需最终源/运行环境
闭包审查；T005/T008 正式网络矩阵、T009 交付与 T012 关闭未完成。
本轮无 SIF/Tiger 操作、Git 合并或正式 MiniNDN 执行。
