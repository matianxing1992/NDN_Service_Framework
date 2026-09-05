# T008 — Y-B 保护纪元 grant 往返（接线与调试进度）

**Layer**: implemented（requester seam、Python Provider 保护纪元路径、
grant 发布/获取链的 7 项修复）;executed（unit 层闭环 + MiniNDN 调试
序列）;无 measured 声明。

Date: 2026-09-05. Source HEAD: `8fe17c63`（fetch 错误消息暴露）。

## 已接线（全部提交）

1. planner/client/facade 透传 `grant_binding_provider` 与
   `protection_epoch`（`66f6ce55`、`46604d99`）;
2. user.py `_build_grant_seam`：进程内权威 + 发布（`66f6ce55`）;
   authority identity 用 requester 前缀（`b815f316`）;publisher
   错误透传（`eeddbb5d`）;
3. Python Provider 保护纪元路径 + 按身份解析 recipient 密钥 +
   `--dynamic-provisioning` + `--local-model-path`
   （`66f6ce55`、`46604d99`、`93a8c058`）;
4. grant 名组件安全化（`2ab1f89d`）;grant_view 的预认证 manifest
   fallback（`8fc712f0`）;Merge 角色保持无装配身份（`007d5eb6` 回退）;
5. 发布链：`NDNSF-DI` 命名空间放行 + CS 放置（`f21d0665`、
   `eeddbb5d`）;ServiceUser identity 前缀路由注册（`c0a887fe`）;
   Provider fetch forwarding hint 到 User 节点（`1232a043`）。

## unit 层（executed）

`tests/python/test_spec181_y_b_grant_seam.py`：进程内权威签发 →
发布 → Provider 侧 `verify_and_unwrap_grant` 解包同一内容密钥
（1 test，全绿）。grant 路径 58 项回归全绿。

## MiniNDN 层（调试中）

grant 签发、发布、Provider 精确名 fetch 链在 MiniNDN 多节点下的
逐步修复已完成（见上 5）;最终验证受 host 环境竞态阻碍（libndn-cxx
face 层 segfault、OOM——长时间运行后恶化）。下次会话按
`evidence/t005-y-n-matrix-current.md` 的环境清理步骤重跑
`unshare -Urnm python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py
--case Y-B`（env 见 /tmp/spec181-y-n-run/env-yb.sh）。

## Verdict

NOT PROVEN（MiniNDN 层）。接线与 unit 闭环完成;MiniNDN 首跑的环境
竞态阻碍了 executed 层。
