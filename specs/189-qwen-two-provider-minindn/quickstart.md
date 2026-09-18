# Spec189 Quickstart

## Resume

从 [tasks.md](tasks.md) Current Checkpoint 开始，按
B189-0 → B189-4 → B189-1 → B189-2 → B189-3 → B189-5 执行。
不重做已验证组件，不把它们当 full-path PASS。
实际命令在实现批次唯一 evidence 中冻结；本页不发明尚不支持的新 CLI。

## Safe preparation

核对 failure-log、最新 raw run、global receipt/binary hash。
T008 的 preflight/持续采样/受控停止先于真实模型准备。
复用 canonical source；native prepare 发布原子层/shared 材料，
Repo commit 且可读后返回 PreparedModel，不预装最终两段 runner。

## Build and check

复用已验 build tree/全局依赖，受影响 target 增量构建默认 -j4。
保留现有 Repo/PreparedModel/placement/assembly selectors；
Waf target `spec189-two-provider-oracle` 已存在（源码 `examples/Spec189TwoProviderOracle.cpp`，
产物 `<build>/examples/spec189-two-provider-oracle`），但事件假设待修正。
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
不启动 SIF/Tiger。
