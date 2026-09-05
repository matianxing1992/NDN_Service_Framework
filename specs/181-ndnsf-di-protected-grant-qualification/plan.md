# Implementation Plan: NDNSF-DI Protected-Grant and Qualification Closure

**Branch**: `Experimental` | **Date**: 2026-09-05 | **Spec**: [spec.md](spec.md)

## Summary

继承 Spec 180 的实现资产（ACK 驱动规划、V3 选择投影、规范 grant 编码
与真实权威、runner 契约、Tiger 提交工具链），按三层测试标准收口剩余
工作：grant 网络服务端与 Provider 双侧解包 → Y-N 全矩阵语义重跑 →
收敛审计与本地资格 → 不可变候选 → SIF → 一次 Tiger Y-B。

## Gate order（只有一个活动门）

```text
R0 诚实化修正（R001--R004：合成拒绝/状态冒充/明文冒充全部失败关闭或
   UNAVAILABLE；先于一切实现任务，作为 G0 的前置门）
 -> G0 grant 正路径（进程内权威 + Python/native 解包 + Y-N-E 真实变异）
 -> G1 负面矩阵语义重跑（MiniNDN Y-N-C/P/R/I/E/L + 集成用例设计标准）
 -> G2 收敛审计 PASS + 本地资格认证（MiniNDN Y-A/Y-B/Y-N 小模型 CPU）
 -> G3 S1 候选封印（提交哈希） + S4 新 SIF + exact-SIF replay
 -> G4 S5 一次 Tiger Y-B + 终局记录
```

只有第一个未关门是活动门。集成/单元测试可以关闭其所属任务，但不得
提前打开后续门。任何行为影响面变更使下游证据失效并回到最早失效门。
（Spec 180 的教训：rev 121/122 在 S2 未完成时做 SIF——本 spec 用任务
依赖结构禁止，而不是靠叙述约束。）

## Constitution Check

- **Pre-design gate**：原则 I（通用动态运行时）——grant 机制是通用
  NDNSF-DI 基础设施，无 YOLO 特判；原则 II（安全在数据路径）——grant
  复用权威签名 + 收件人加密，不新增调试后门；原则 VII（昂贵执行的
  不可变晋升）——候选身份含提交哈希与全部平面。
- **Post-design gate**：原则 VIII（收敛审计在正式验证前）——G2 是硬门；
  审计原则 4 的四层证据分离在每个证据文件头部声明层。

## Architecture Decisions

1. **权威宿主 = 请求方进程（功能切片）**。策略权威在 user 进程中
   进程内签发（`AuthorityBackedGrantProvider` 已是进程内 closure），
   grant Data 经既有 `ServiceUser.publish_signed_app_data` 路径发布，
  不新增网络角色与前缀。身份/公钥摘要来自 Spec 180 注册表的
   `artifactPolicyAuthority` 条目。独立权威服务是生产形态的延期项
  （spec.md Out of Scope）。
2. **Provider 解包先 Python 后 native，向量共享**。Python 路径
   （`provider.py` 装配入口）先接上 `verify_and_unwrap_grant`；native
   `ProtectedRuntime` 用同一跨语言向量（相同 grant 字节 → 相同内容
   密钥）验证等价。
3. **集成测试 = 真实生产链**。集成用例必须经过：真实权威进程签发 →
   真实请求方（requester 身份签名）→ 真实 Provider 获取与解包（进程
   边界用真实 NFD/face，不得用 mock 替换被测链）。单元测试只覆盖纯
   函数与编码层。
4. **测试标准先于实现**。每个任务的验收清单明确列出三层用例名称；
   集成用例的"生产入口 + 注册拒绝原因 + 边界位置"三要素缺一不可。
5. **内容密钥必须被真实消费（FR-013，设计自审结论）**。解包出的
   内容密钥必须解密真实密文（装配产物 AEAD 加密暂存），禁止"验证
   授权但密钥闲置"的授权剧场——这是本 spec 对 Spec 180 机制真实
   起作用教训的直接落实。
5. **小模型 CPU 是资格对象**。MiniNDN 用例全部使用 canonical YOLO26n
   的 CPU 后端；GPU 只出现在 S5 Tiger 阶段，且记录设备证据。

## Ownership matrix（继承 180，不改变归属）

| 平面 | Owner |
|---|---|
| grant 编码/权威/解包 | `core/protected_artifacts.py`、`security/*`、`app_sdk/placement.py`、native `ProtectedRuntime` |
| 权威服务端 | controller 子进程（`examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py` 扩展或等价维护入口） |
| MiniNDN 执行 | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`（继承 180 的 barriered runner） |
| 候选/SIF/Tiger | Spec 180 的 `scripts/spec180_*` 工具链（路径沿用，所有权移交本 spec 的 T009--T012） |

## Project Structure

```text
specs/181-ndnsf-di-protected-grant-qualification/
├── spec.md
├── plan.md
├── tasks.md
├── traceability.md
└── evidence/                  # 本 spec 的资格证据（新目录）
```

## Migration and Rollback

- Spec 180 全树冻结；本 spec 继承其契约与证据路径（引用式，不复制）。
- 每个行为变更提交独立 commit；任何任务发现设计缺口时回到本 plan
  修订（revision 编号递增），禁止就地静默改契约。
- 若 grant 网络服务端在 controller 内实现受阻，回退路径是独立权威
  进程（同契约、同注册表），不弱化安全语义。
- **撤销子系统（延期，原则 11）**：账本与网络撤销服务由所有者的另一
  分支开发。本分支 `revocationSequence` 固定为 1、无撤销服务；集成
  条件与删除标准见 spec.md Out of Scope 的延期项。
