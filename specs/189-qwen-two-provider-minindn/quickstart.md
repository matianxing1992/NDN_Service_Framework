# Spec189 Quickstart

## Resume

从 [tasks.md](tasks.md) Current Checkpoint 开始，按
B189-0 → B189-1 → B189-2 → B189-3 → B189-5 执行。
当前 B189-0 已关闭；host guard/lifecycle safety 有定向证据但作为跨批次门保留。B189-1a/1b
和 B189-3 的局部出口已有证据，但完整资格仍保持 PARTIAL。
新 native counters 随 T003/T006 交付，T009 前统一核对，不能形成安全门循环依赖。
不重做已验证组件，不把它们当 full-path PASS。
Spec188 的 YOLO 结果只证明轻量公共入口；它不覆盖 Qwen 的分阶段材料、hidden-state
handoff、ORT runner 或跨 Provider stream liveness。r155-r161 已在新候选上越过
ACK/Selection、source-cache、assembly/runner 边界；r161 的首个生产失败是 local ONNX
adapter 在 edge-local role 绑定前使用 full lineage validation。`validateCore()` 已
编译并安装；r162 已在授权入口后受控停止，旧 r163 Provider-only cache-compatibility
版本又在 ACK 前暴露 prepare-time disk boundary。现在 requester metadata-only receipt
接缝已完成静态门、受影响 DI closure 编译和安装；r164 已尝试验证无 run-scoped
encrypted Repo 的 materialization，但在 requester/Provider 启动前因 host
`RESOURCE_BOUNDARY:diskFree` 停止（`diskFreeBytes=4232839168`）。获得足够磁盘空间后
再使用新的 run ID 继续；不能只增加 timeout。
实际命令在实现批次唯一 evidence 中冻结；本页不发明尚不支持的新 CLI。

## Safe preparation

核对 failure-log、最新 raw run、global receipt/binary hash。
每次真实模型准备前都要通过现有 host preflight；持续采样/受控停止由最终 runner
执行，资源门不再单独占一个任务。
复用已校验的 system-wide canonical source cache；native prepare 发布原子层/shared 材料，
Repo commit 且可读后返回 PreparedModel，不预装最终两段 runner。

## Build and check

复用已验 build tree/全局依赖，受影响 target 增量构建默认 -j4。若修改共享 DI
header，允许重建整个受影响 DI installable closure；本次 lineage header 变更重建了
196 个 DI Waf tasks，但未重建 Core/Repo/UAV。只改单个 `.cpp` 时保持 target-local
增量边界。
保留现有 Repo/PreparedModel/placement/assembly selectors；
Waf target `spec189-two-provider-oracle` 已存在（源码 `examples/Spec189TwoProviderOracle.cpp`，
产物 `<build>/examples/spec189-two-provider-oracle`）。progress/lifecycle selectors
已覆盖 per-provider/role allowlist 和 monotonic freshness；lineage core/edge validation
focused change 与 requester/provider cache-compatibility implementation 已安装；r164
仅留下 admission disk-boundary 证据，真实 materialization/terminal 运行仍待完成。
每小任务静态门、批末组合审查后统一 C++ 构建测试，CLI 本身不是 oracle。

## Run and repeat

前置 runtime 出口及安全门通过后，用维护的 MiniNDN runner，
root PATH 含 /usr/sbin:/sbin，两个 CPU Provider，唯一 run directory。
同 requester 进程 prepare 一次，同 handle 连续两独立 request；
各自走 ACK→planner→Selection→Repo 范围材料→native assembly→NDN handoff→
独立输出校验→drain。事件按因果偏序，不按 Provider 日志全序。

保存 publication 零增量、资源峰值/post-drain 与实际结果；
保持 candidate 内容摘要，新 run-id 重复上述场景。
两次均成功才 QWEN_TWO_PROVIDER_PASS；分类失败仍 PARTIAL。
本地 MiniNDN 使用宿主机已安装的 NDNSF/Core/Repo/DI 库和本机构建的 C++ APP；
不启动 SIF/Apptainer，也不把 SIF/Tiger 作为本地资格前置条件。

## Temporary cache-compatibility diagnostic

仅为当前 Qwen3-0.6B 内存诊断时显式增加
`--cache-compatibility-mode`。该选项要求 system-wide source cache 已命中并通过
identity/hash/size 校验；requester 仍完成 authenticated prepare/manifest/ACK/Selection/
grant/placement，但交付 metadata-only receipt，不创建 run-scoped encrypted Repo。仅在
authenticated Selection 之后让 Provider 从已验证 plain source cache 读取 graph/initializer，
跳过该阶段的 Repo material fetch；已建立 `ProtectedRuntime` 的 protected role 还可复用
按目录 SHA-256 验证的 assembled `model.onnx`，不会再创建第二份 protected staging。
缺缓存、校验失败或 protected role 没有 `ProtectedRuntime` 必须停止，不能 fallback。
普通 protected Repo qualification 仍不得走该诊断路径。该模式不计入正式 Repo 或
`QWEN_TWO_PROVIDER_PASS`，运行证据必须单独标注 diagnostic。
