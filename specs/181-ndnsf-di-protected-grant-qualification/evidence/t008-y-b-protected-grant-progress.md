# T008 — Y-B 保护纪元 grant 往返（接线与调试进度）

> **Current status: BLOCK / NOT PROVEN (revision 5)**。以下为 Python Provider 的历史接线与调试，不能关闭 T002 native 路径或 T008 资格。最新检查 `/tmp/spec181-y-n-run/yb39.log` 的六次尝试均为 `CASE_RUNTIME_PROCESS_START_FAILED:control`；包装脚本 `EXIT=0` 不是协议 PASS。

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

grant 发布与获取有多次接线修改，但当前日志未证明保护 Y-B 端到端
完成。旧记录的 OOM/face 竞态解释缺少本轮可核对的第一边界证据，
保留为历史假设，不作为重试依据。先完成 G0 定向修复与 T007 审计；
不得依据旧环境脚本直接启动完整 Y-B 资格。

## Verdict

BLOCK / NOT PROVEN。存在控制性设计/代码缺口与 control 启动失败；
按当前 plan.md 的 G0→G1→G2 顺序继续，不能将其统称为环境竞态。
