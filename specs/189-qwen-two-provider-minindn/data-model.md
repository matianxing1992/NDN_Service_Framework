# Data Model: Qwen Two-Provider Candidate

## Existing public entities

复用 PreparedModel、Runtime/prepare/request options、Repo manifest/receipt 和 native
placement 类型。本节是目标约束，不声明已实现的新 DTO 或 PreparedQwenModel API。

## Prepared materials

模型 identity 绑定 revision/config/tokenizer、canonical graph 和 initializer。
Repo manifest 描述原子 layer→graph nodes/tensor object（或受验证 byte-range）、
digest/size/schema/shared dependencies；材料数量与 Provider 数量无关。
embedding/final/tied weights 内容去重。handle 持有 reference/lease，
不为未来 request 常驻整份模型。

## Placement

ACK 后计划将材料映射到 Provider；本实验 [0,14)/[14,28)。
Selection/grant 绑定 manifest、材料集合、role/range、Provider、attempt/epoch、
plan digest 与 dependency endpoints。授权 epoch 不属于模型永久默认值。

## Candidate and run

candidate 是 source content、ABI/binaries、model/materials、profile/topology/oracle；
run 是 run-id、request ids、运行期 key/path、时间/资源/exit records。
同 candidate 可重复；同 PreparedModel 可有多个独立请求。

## State transitions

UNPREPARED → PREPARING → REPO_COMMITTED → READY_REFERENCE。
每 request 独立 REQUESTED → ACKED → PLANNED → SELECTED。
授权后每 Provider 可并行准备 runner/等待输入，二者 ready 才 execute；
前段输出先于后段消费，末段返回 terminal，全体 owner drain 后结束。
异常/cancel/resource stop 进入失败清理，不因分类完整成为 PASS。
READY handle/持久缓存可在预算内供后续复用；drain 不等于删除持久模型。
