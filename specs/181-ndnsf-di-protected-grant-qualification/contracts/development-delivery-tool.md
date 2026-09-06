# Local Development Delivery Tool

**Status**: planned implementation | **Owner**: T009, source closure before T008.
**Authority**: FR-008/009, SC-005, handoff-contract.md.

## Scope And Identity

`scripts/spec180_candidate.py` 沿用原路径与 canonical_bytes/sha256_digest。
原 CandidateRecord 十平面 API 保留为历史兼容入口，本地 seal/verify
不实例化它、不要求 SIF/profile/远端回执；不据兼容测试声明远端验收。
新增本地入口只生成 `spec181-development-delivery-v1`。

candidateId/candidateDigest 来自已经运行的 T008 inventory，用于关联
该次运行；deliveryDigest 在验证证据齐全后计算，不将其反向作为本次
运行的 candidateDigest，避免证据自包含的循环摘要。源码必须是 T008
的准确 clean commit。封印写到源码检出之外；之后提交文档时仍明确
保留已验证的 sourceRevision，不声称新文档 commit 已重新运行。

## Functions And File Changes

| Symbol / file | Change / purpose |
| --- | --- |
| scripts/spec180_candidate.py::seal_development_delivery(recipe, *, root, environment) -> dict | 验证实际文件、源码与完整资格证据，构造规范本地交付记录并计算 deliveryDigest；库函数不写文件 |
| scripts/spec180_candidate.py::verify_development_delivery(record, *, root, environment) -> None | 先核对原 digest/引用实际字节，再复用相同构造/验证逻辑核对环境、源码及资格语义；不重新签署变更记录 |
| _delivery_reference(root, value) -> dict | path string 或已有引用转为 path/sha256/bytes；验证已有引用摘要与大小，分块读取实际文件，检查读取期间身份稳定 |
| _delivery_json(root, reference) -> dict | 读取实际 JSON 并核对所读取字节摘要；非 object 拒绝 |
| _delivery_context(root, revision, validation, environment) -> dict | 复用 inventory/supervisor owner，核对审计、完整条目、源码/输入/配置、实际日志和子进程结果，返回绑定上下文 |
| _build_development_delivery(recipe, *, root, environment) -> dict | 统一规范化引用、检查必填分组/交接状态；内部唯一 record 构造者，不维护额外状态 |
| main(argv=None) -> int | seal/verify CLI；显式 root/recipe 或 record/environment-json；seal 在同目录临时文件写入/flush/fsync 后以硬链接独占发布 output，finally 清理临时文件；失败不留部分 PASS 文件，verify 不写文件 |
| tests/python/test_spec180_candidate.py | 保留历史 API 回归，新增真实临时 Git checkout、生产 gate 生成的受控子进程证据、seal/verify 及错误输入检查 |

root 是实际源码检出根目录，environment 是 T008 同一显式字符串映射；
环境内容不直接进入交付记录，复用 local_launch_configuration 的摘要
和 local_input_identity 的文件身份。recipe/record 是 operator 声明的
路径集合；不执行其中命令，不复制文件内容/密钥，不修改构建或源码。
公钥/模型有效性由既有生产预检负责，封印工具核对相同字节与证据。

## Recipe And Record

recipe 使用以下唯一具名字段；缺分组、空列表、非文件、不可读 JSON
或未知字段都拒绝，不根据本机目录猜测输入：

- sourceRevision：40-hex commit。
- inputs：contracts、registries、publicKeys、models、oracle、runners、
  parityVectors、caseEvidence；每组为非空文件路径列表。caseEvidence
  收录实际案例结果/终端/清理及失败历史；具体覆盖范围仍经 T009 审查。
- localEnvironment：nativeArtifacts 非空文件路径列表，绑定 native
  manifest、实际二进制和复现所需依赖；effectiveConfiguration 与
  inputIdentity 由工具从实际 T008 身份检查生成。
- validation：audit、inventory、qualification 三份 JSON 路径。
- reproduction：可复现命令、获取方式、版本/输出规则的维护文档路径。
- limitations：CPU/YOLO 范围、未采用草稿、GPU 准备缺口和延期的文档路径。
- experimentTransfer：contract 路径、owner=experiment-machine，
  T010=TRANSFERRED、T011=TRANSFERRED。

记录保留这些分组，把路径规范为 `{path, sha256, bytes}`；额外生成
schema、candidateId、candidateDigest、handoffStatus 和 deliveryDigest。
validation 同时绑定所有条目的原始 stdout/stderr 文件引用。外部工件
可在源根之外，必须在 reproduction 给出获取/留存方式；引用临时文件
的摘要本身不满足“可交付”，T009 另核对持久副本与获取说明。

## Verification Rules

1. 复用 `_validate_source_checkout` 拒绝脏源/错误 commit，不通过
   git status 简写替代完整源码检查。复用 `_validate_complete_inventory`、
   `_validate_entry_command`、`_entry_digest_matches` 校验原完整清单。
2. audit 是新鲜代码审查之后保存的 `spec181-convergence-verdict-v1`
   JSON，verdict=PASS，并绑定 sourceRevision、effectiveConfigDigest、
   inputDigest 和非空 evidence 文件引用。它是 T007 人工/agent 代码审查
   的机器可读凭据，不是自动从旧 Markdown 搜到 PASS。没有新审查不得生成。
   reviewedAtUnix 必须是有效正时间且不晚于第一项资格 child 的 startedAtUnix，
   保留先审查、后完整运行的顺序；不得事后补填旧时间。
3. qualification 必须为生产 RESULT_SCHEMA/PASS，绑定相同 candidate、
   source、inventory、config、input；三类身份 PASS，零 failed/unexecuted
   entries，cleanup/redaction PASS。结果的 inventory 副本字节摘要一致。
4. 完整 entry ID 和顺序必须与 inventory 一致；每项 command/digest、
   kind/case、cwd、timeout 和真实 child environment 相符。exitCode=0、
   signal=None、timedOut=False、cleanup/redaction/status PASS；pid 必须
   是正整数。核对 stdout/stderr 实际摘要，MiniNDN 的注册 oracle marker
   必须存在于其实际日志。语义仍由各案例生产 owner 决定，工具不重定义它。
5. 从当前 root/environment 重新计算 configuration/input 身份，与
   inventory、qualification、audit 一致；seal 前后复核源码/输入/引用，
   变动不得静默更新旧证据。verify 不仅核对 deliveryDigest，还核对以上
   实际状态与关系，不能靠重新计算外层摘要掩盖坏终态。
6. handoffStatus 只能是 READY_FOR_EXPERIMENT_MACHINE；不出现 SIF/GPU/
   Tiger PASS、远端已接收或 LOCAL_DEVELOPMENT_PASS。最后状态属于 T012。

## Proof And Failure Boundary

正常用例使用真实临时 Git repo 和生产 run_local_gate 的轻量受控命令，
只证明封印工具/监督器关系，不证明真实 MiniNDN。至少覆盖：缺本地字段、
无需 SIF、脏文件/新 commit、模型或公钥变化、配置变化、跨版本结果、
不完整 entry、非零退出/信号/超时、cleanup/redaction 失败、日志变化、
仅重算外层摘要、缺/错误审计，以及 CLI 输出不覆盖已有记录。

实现仅在命名的定向测试上执行；T008/T009/T012 保持未完成。真实资格
结束后再用本工具封印实际记录，随后核对交付持久性与全部 FR/SC。
