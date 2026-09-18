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

### Protected Repo integration binding

2026-09-18 源码复核：`RepoSourceProvider::publish` 的 plain Repo object names
不满足生产 assembler 的 `fetchEncryptedLargeData` 契约；不得仅注入该 publisher
并把返回 receipt 当作网络可用。现有组件测试保留，但不能证明加密产物持久发布。

T003 生产路径复用 `NativeCanonicalArtifactPublisher::Transport` 与 ServiceUser 的
分段签名/serving；在 Core 存储边界增加 `EncryptedLargeDataRangeStore`（TARGET，已有未验草稿），
由 Repo adapter 复用 `RepoCore::putRange/commitRanges/abortRanges/getRange`。
Core 负责确定 name、AES-GCM/AAD、wrapped key 和 Data 签名，store 只接收已经加密的
envelope，按同一 name/digest/size 持久提交并返回有界范围读取 owner。
不让 Repo adapter 生成密钥、重写 name 或自行宣称受保护 receipt；不走明文降级。
commit 返回前核对 manifest 与范围可读；失败只回滚本事务 staging，warm/shared
对象不能删除，提交状态不明时先核对，不能直接 READY。

`ServiceUser::replyFromLargeDataFile` 的现有分段响应应读取该 owner，保留同名
segment/final-block/签名；RepoCore 的普通 getRange 本身不是 NDN producer。
requester Runtime 明确持有 ServiceUser/Face/Repo owner，不以 Repo 文件存在代替网络可达。

publication serving lease 必须随 `PreparedModelPackage`/活动请求的最后一个 owner
释放；现有 file publication 默认 5 分钟 TTL 不能使仍存活的 prepared handle 失效。
lease 覆盖 serving 元数据与 wrapped-key 引用，不只是磁盘文件；close/cancel 仍须按
已有授权失效语义停止新请求。新增 C++ 反例覆盖越过原 TTL、源 owner 释放、正常读取、
取消/失败 rollback 和最后 lease 释放。不得仅延长 TTL 或改环境变量掩盖所有权缺口。

先在 B189-1a 独立验证共享 protected publication 接缝，再于 B189-1b 将原子层/shared
tensors 沿同一路径发布。前者可关闭子批但 T003 仍 PARTIAL；source lookup/ingest
不作为 T003 完整替代方案。

成功 receipt 的 publisher cache 不能永久强持 serving lease；由既有 preparation
cache 的预算/淘汰决定空闲保留，活动 package/request 保住读取能力。最后 handle
释放不一定立即删除预算内缓存，但 eviction/close 后必须可回收；增加真实 publisher
参与的 C++ 反例，不只检查手动 token reset。持久提交保证本次存活 owner 下可读，
不承诺进程重启后恢复密钥/serving；跨重启恢复不属于本 Spec，不能为此增建服务。

### Bounded commit and identity ownership

B189-1a 草稿静态审查发现同步 commit 在 Core I/O 路径执行大文件 hash/写盘，且
没有取消参数；必须在既有受控 worker 生命周期内执行重型存储/加密工作，
只把必要 Core 状态安装/响应操作投递到 I/O。传递现有 cancellation/deadline，
窗口间检查并有 join/drain；不能只在大文件处理前后检查，也不能靠调大 timeout。
慢盘测试验证 I/O heartbeat、取消返回和 worker drain；不宣称能立即中断任意 OS 磁盘调用。

lease 必须绑定 committed object identity，而非仅 name/size。若允许删除后同名重建，
旧 lease 不得读取/删除替代对象；复用 Repo 的 generation/事务所有权或等价原子
identity-checked read/remove，不能只在 adapter 中先比较摘要再无条件 remove。
失败回滚仅处理本次拥有的 staging/committed object，不能删除其他发布者的数据。
小型 C++ 反例覆盖同名替代、部分提交失败、取消、旧 lease 释放和预算淘汰。
上述属于共享存储正确性，不新增独立任务或新 Repo 服务。

Repo 只替换材料存储/获取边界，不削弱已有 NAC-ABE/grant、签名或内容完整性校验。
manifest 与原子/shared 材料沿现有保护接口发布，Provider 只能在有效 Selection/grant
下读取其授权集合；byte-range 索引必须保留加密对象的校验/解密边界，不能按任意
密文字节切片绕过验证。schema 更新同时修改 producer/consumer 与错误格式反例。

## Validation

复用 Repo/PreparedModel C++ selectors，补 real-Qwen receipt、真实 requester 接线/
可达性、源释放和同进程两请求计数。离线导出只提供 canonical 输入，
不能以两个 stage 文件证明 native prepare 已完成。
