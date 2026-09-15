# Research: YOLO MiniNDN SIF+APP Fast Path

## Decision

沿用现有 TigerCluster 入口完成 source handoff、candidate SIF、外置 APP、pair runner 和 MiniNDN harness；不建立第二套镜像构建器。YOLO 是首个垂直切片，QWEN 延后。

## Rationale

现有入口已经覆盖输入封存、Apptainer 1.5.3 检查、candidate/APP manifest、运行时挂载和失败边界。真正缺少的是可验证的 regular base SIF，而不是另一套脚本。把本机 MiniNDN 放在 TigerCluster 之前可以先发现 ABI、路径、KeyChain、NFD 和请求接线问题。

## Alternatives considered

- 直接复用历史完整应用 SIF：拒绝。它没有经过当前 base/APP pair contract，且本机链接目标已经不存在。
- 先做 QWEN：拒绝。tokenizer、streaming state 和模型 staging 会扩大首个批次。
- 直接在 TigerCluster 构建：拒绝。会把调度和镜像配置问题混为产品结果，也违反 local-first gate。

## Known external input

本机没有 regular base SIF；当前 images/spec180-runtime-r119.sif 是 dangling symlink。它必须在构建前由独立基础层交付或生成任务提供，不能用旧链接绕过验证。
