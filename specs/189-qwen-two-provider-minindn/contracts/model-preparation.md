# Contract: Reusable Qwen Preparation

**Status**: TARGET / T003 pending. 当前 Repo adapter 已有组件实现；真实 requester 与
范围材料 consumer 尚未闭合，fallback 仍发布完整 graph/initializer。
以下是对现有公开入口的目标要求，不是当前行为声明。

## Required behavior

目标要求：现有 native prepare 入口验证 pinned canonical graph/config/initializer 后，
发布拓扑无关原子层/shared tensor。prepare 不输出最终两段 partition model，
不绑定 Provider；ACK 后规划、Selection 后组装。

复用 Repo manifest/payload owner，必要的内部 schema 扩展必须版本化：
layer→graph/tensor object 或 byte-range，digest/size/shared dependencies。
stageIndex=0/1 不能作为永久材料身份。读取单元有界，不能以整 initializer
下载模拟范围读取。

## Commit and lifetime

对象持久写入、摘要验证、manifest commit 且正常 Repo 客户端可读后才 READY。
实际 requester 持有/连接 Repo service/source owner；测试 adapter 不代表实际接线。
临时源 owner 可释放；handle 保留引用/lease。重复 prepare 命中完整 immutable identity，
同 handle 后续 request publication 增量为零。使用文件后端及预算内 cache。

## Failure behavior

digest/size/schema/range/config 错误拒绝 READY；只回滚本次未提交 staging，
不破坏已有有效 manifest。Repo 不可达/对象丢失/失效 handle 明确失败，
request 不隐式重发或退回 whole-model。旧 schema 明确拒绝或走受测兼容路径。

## Protection and compatibility

Repo 只替换材料存储/获取边界，不削弱已有 NAC-ABE/grant、签名或内容完整性校验。
manifest 与原子/shared 材料沿现有保护接口发布，Provider 只能在有效 Selection/grant
下读取其授权集合；byte-range 索引必须保留加密对象的校验/解密边界，不能按任意
密文字节切片绕过验证。schema 更新同时修改 producer/consumer 与错误格式反例。

## Validation

复用 Repo/PreparedModel C++ selectors，补 real-Qwen receipt、真实 requester 接线/
可达性、源释放和同进程两请求计数。离线导出只提供 canonical 输入，
不能以两个 stage 文件证明 native prepare 已完成。
