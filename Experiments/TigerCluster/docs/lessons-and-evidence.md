# Tiger实验经验与证据快照

这是从Codex历史、现有skill及本地实验汇总提炼的可交付记录，核对到2026-11-13。
记录的是历史候选的范围，不是当前分支或新SIF的PASS。推荐路径见[portable guide](portable-deployment.md)。

## 已核实的结果与限制

| 历史结果 | 条件与可用结论 | 不能推出 |
| --- | --- | --- |
| Qwen3-0.6B GPU | Spec220 prepared-index-p0-pass-r1及prepared-index-root-r1：同候选SIF 271cd58c…，两Provider、独立oracle及非强制退出 | 改过源码/镜像后仍合格；其他模型GPU通过 |
| Qwen3-0.6B 两物理节点GPU | Spec217 b217-11-gpu48-qualified-replay：32/32输出、实际两节点 | 与旧CPU动态INT8是纯backend对照 |
| Qwen3-4B CPU r34 | assembled-cache命中、32token、exit0及checkpoint；当时独立oracle仍PENDING | GPU资格、oracle PASS、冷组装路径都已通过 |
| Qwen3.6-27B INT4 CPU | Spec222 int4-cold-warm-r3：同节点双Provider，一次allocation四会话八请求，独立逐token参考、checkpoint推进及自然清理 | GPU、多主机、Repo网络模型分发、原始KV数值比对或TP资格 |

27B CPU采用已准备文件cache，保留原生调用、规划、授权及activation的NDN路径。
build记录本身仍为BUILT_UNQUALIFIED；其后实际运行报告才建立上述资格。

## 27B INT4 CPU资源与复用基线

固定条件：cold输入25/输出32，追加24，warm起始context81/输出32；
一次allocation 10 CPU/40 GiB，Provider实际ORT intra-op=1。

| 请求类别 | 样本数 | wall/s | 业务ORT/s | 业务ORT/wall |
| --- | ---: | ---: | ---: | ---: |
| 首次进程启动Cold | 1 | 279.412 | 143.644 | 51.41% |
| 进程常驻的新会话Cold | 3 | 146.484 ± 2.057 | 145.303 ± 2.091 | 99.19% |
| Warm | 4 | 143.121 ± 1.593 | 142.026 ± 1.626 | 99.23% |

±为会话样本标准差；占比为同类ORT总和/wall总和。
业务ORT不包含Session初始化与runtime warmup。
Provider step独立MaxRSS约22.236/17.505 GiB，不能相加当同时峰值，
不能据此承诺每Provider 15 GB，也不能直接推广到更长输出或不同线程配置。
这些结果可指导资源估算；不是新作业必须申请40 GiB的规定。

## 反复失败的原因与最小处理

| 首个失败/误判 | 提炼后的做法 |
| --- | --- |
| 源码已改，实际使用旧安装库/SIF | 冻结source/config/release；核对真实装载文件，重验受影响边界 |
| 编译完成但打包失败，从头再编译 | 封存并验证builder checkpoint，复用编译，仅重新打包和候选准入 |
| 0.6B通过就直接跑昂贵27B GPU | 同候选小GPU冷暖/独立oracle/自然退出先过，大模型仍验图与资源 |
| 模型/量化改变，Authority仍指旧FP16材料 | 同步source、initializer、manifest与profile身份，不扩大权限 |
| 文件命中被称Warm | 分开source/fragment、assembled、Session、KV四层；分开首次启动与新会话 |
| journal ready、旧对象计数被当成目标Repo可读 | 检查本次exact root和版本；用生产API跨实例/重启读回 |
| /project约200GiB写满却把df当个人额度 | quota单独查，EDQUOT保留；没有固定200GB上限的确证 |
| 把cache_root放在会被容器scratch遮蔽的/scratch | 在实际SIF/节点核对挂载；可复用数据使用明确可见路径 |
| RO源文件根同时被RW父目录覆盖 | 输入/配置RO，objects/output单独RW；保持原生overlap拒绝 |
| 冷组装缺unrankedInputNames，但cache-hit正常 | 同一runner元数据必须贯穿miss/hit两条路径，分别验证 |
| 无GPU就等整天，而CPU/内存有余量 | 每阶段查全群/预约；CPU准备和CPU推理申请CPU资源 |
| 只看epochs=33，就认为TP完成 | 按run/request/role过滤；旧PP累计行不能覆盖本轮TP失败 |
| 提前发Interest被认定为互等根因 | 找真正future.get/wait和缺失绑定；fetch begin不证明计算未开始 |
| 不断改timeout重复同一确定性失败 | 定位第一边界，静态检查caller/consumer，再做最小回归 |
| 推理有32token但独立oracle排队 | 标INFERENCE_COMPLETE / ORACLE_PENDING，不写全通过 |
| Slurm COMPLETED或leader退出被当清理完成 | 检查角色进程组/cgroup，保留needs_closeout与原节点身份 |

## 来源与迁移说明

本次读取Codex主会话01a15c90-8992-7b22-aca2-c65911a46211的Tiger相关消息。
2026-11-11T23:00:11Z的四组cold/warm结果取代同会话此前“只测Cold”的状态。
模型跨实验复用、/project持久Controller材料、十分钟复查、禁止改MemorySwap是用户明确要求。
没有把完整聊天、凭据、private handle、模型或原始大日志复制进Git。

本地原证据（fresh checkout可缺少；运行指南不依赖它们存在）：

- specs/222-tiger27b-reusable-deployment/evidence/int4-cold-warm-r3/report.md、
  requests.csv、summary.json、slurm-accounting-r3.json。
- 同目录上级 filesystem-int4-result.md、filesystem-27b-cpu.md、
  filesystem-sif-build-r3.json、qwen3-4b-progress.md、
  tiger-resource-inventory-20261108-1745.md。
- specs/220-warm-path-reuse/evidence/prepared-index-p0-pass-r1/report.md、
  prepared-index-root-r1.md。
- specs/217-portable-di-deployment/evidence/b217-11-gpu48-qualified-replay.md。
- results/ndnsf-di-paper-cpu/及ndnsf-di-dispatch-20261027/的本地汇总，用于计量边界复核。

现行说明已收敛：portable-deployment、sif-build、local-validation、
local-development、installation。旧Spec170/175镜像号、Qwen2.5规模阶梯、
容器Python默认探针、固定节点/资源申请和巨型no-repeat checklist不再是默认流程。
历史版本保留在Git中，原skill references路径仅作迁移提示。
旧docs/runtime-workflow和安装/本地实验指南转为短入口；论文工程、活动Spec与原证据保留原位。

公开站点资料：
[iTiger Wiki](https://github.com/itiger-cluster/itiger-cluster.github.io/wiki)、
[Running A Model](https://github.com/itiger-cluster/itiger-cluster.github.io/wiki/Running-A-Model)。
2024页面中的home建议不是本次账号quota测量；本次未进行新的SSH、资源查询或推理验收。
