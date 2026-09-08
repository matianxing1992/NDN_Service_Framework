# T003 YOLO Tensor Edge Audit

## Source Finding

基线 e9fe3399。本地 inspection 接线前审计发现 NativeGraphSnapshot 没有 tensor
edges；NativeYoloComponentSplit::enumerate 按相邻 nodes 合成 tensor 名及依赖，
资源 weightBytes 按节点数乘 MiB 平均分配。实际维护的 Yolo26Splitter._candidate
（adapters/yolo/adapter.py:198）遍历真实 graph.edges，按跨角色 consumer 集合生成
依赖，以已知 estimated_bytes 的总和除以角色数估算 weight_bytes，最低为 1；
SplitCandidate.validate_against 还要求所有跨角色 tensor 都是 legal_cut_edges。
旧 t003-b 验收仅覆盖角色、排序及部分输入拒绝，没有证明这些候选字段的等价性。
T003-B 撤回 DONE，保持 PARTIAL；历史测试 PASS 保留但不再作为完整验收。

关联审计也发现 Qwen native roleFragment 再次哈希 role/graph/artifact，而维护
QwenThreeStageSplitter.enumerate_candidates 直接使用每个角色的首个 artifact digest
作为 fragments_by_role；native backends 额外扩展 concrete provider 名，维护契约只
声明 onnxruntime family。T003-A 同样撤回 DONE；node_roles、state/candidate 字段和
整体摘要仍需完整对照，旧 range/budget 检查不能证明该卡完成。

## Repair Contract and Review

NativeGraphSnapshot 保存 NativeGraphEdge：tensor ID、producer node、consumer nodes
和 NativeTensorContract。后者保留数值/符号 shape 及可选 estimatedBytes，unknown
不得当成已知非零值。图验证检查唯一边/合法节点引用、非空唯一 consumer、拓扑方向、
tensor 名与 edge ID 一致及 legalCutEdges 引用，模型 input/output 保留同一 tensor 类型。

YOLO enumerate 按图中真实 edge 顺序生成 crossPartitionTensors；每条边的跨角色
consumer 去重、按角色名排序，只生成真实 producer→consumer 依赖，不用节点相邻
关系，也不修改 tensor 名；跨越未列为合法 cut 的边时拒绝候选。预算按维护算法
计算并拒绝 uint64 溢出。
本轮不声称 catalog/semantic interface、fragment/candidate digest 和完整请求已等价。

## Validation Plan

ABI 改动用全新 .codex-tmp/spec182-t003-tensor-edges-r1/build，冻结现有工具链/
依赖，单一 -j4 必要构建与相关 planning/preparation/sealer/publisher 单元。
使用显式分支图：非相邻 producer、一个 tensor 多 consumer、多 consumer 同 role、
同 role 内部边、未知 tensor 大小；按维护 Python 方法独立列出期望依赖/预算。
负例覆盖未知节点、重复边/consumer、逆向边、错 tensor ID、错误 cut 和预算溢出。

## Result

PARTIAL。新 ABI configure PASS（6.809s），r1 -j4 build PASS（376.755s）；
首次单元运行前修复 Qwen fragment/backend，并移除 graph shape 的额外数值限制，
保留维护接口接受的整数/符号表示。r2 增量 build PASS（18.877s），定向单元
67/67 cases、818/818 assertions PASS。分支依赖、非法 cut、引用/拓扑、预算溢出、
未知大小及 Qwen artifact/backend 均有明确断言；未以测试数量代替完整候选验收。

原始证据：
- [r1 preflight](../../../.codex-tmp/spec182-t003-tensor-edges-r1/preflight.log)、
  [configure](../../../.codex-tmp/spec182-t003-tensor-edges-r1/configure.log)、
  [build](../../../.codex-tmp/spec182-t003-tensor-edges-r1/build.log)。
- [r2 build](../../../.codex-tmp/spec182-t003-tensor-edges-r2/build.log)、
  [focused](../../../.codex-tmp/spec182-t003-tensor-edges-r2/focused.log)、
  [ldd](../../../.codex-tmp/spec182-t003-tensor-edges-r2/ldd.log)、
  [design validation](../../../.codex-tmp/spec182-t003-tensor-edges-r2/design-validation.json)。

构建命令：`PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin python3 ./waf -o .codex-tmp/spec182-t003-tensor-edges-r1/build build --targets=unit-tests -j4 -v`。
测试命令：`timeout 60s .codex-tmp/spec182-t003-tensor-edges-r1/build/unit-tests --run_test=Spec182CanonicalPublisher,Spec182V3Placement,Spec182Preparation,Spec182OfferAdmission,Spec182NativePlanning,Spec182PlanSealer,Spec182GrantClient,Spec182NativeInferenceClient --report_level=detailed --log_level=message`。
ldd 无缺失库，NAC/SVS/ORT 来自 preflight 冻结位置。Python extension 未重建，
旧 ABI 产物不作本轮证据；未运行 integration/MiniNDN/SIF/Tiger。
Context Mode project health PASS，active health 因 tasks.md 索引过期失败，
使用当前仓库文档及 CodeGraph/源码核对，没有用旧索引推断任务状态。
下一步补全候选 node/state/interface 与规范身份，然后接实际 inspection/requester。

首次 checkpoint 被本机 pre-commit 的全索引开发助手文本扫描拒绝，命中已有引用，
不是编译/行为失败。核对 hook 后使用其明确提供的 `NDNSF_LOCAL_CHECKPOINT=1`
本地 checkpoint 模式重试；禁止路径扫描仍执行，不修改或停用 hook。
