# Proposal Language and Logic Review

## Scope and Evidence Boundary

本轮按 NDN Slides Review 与 Academic Research Suite 的准则，审查当前 Proposal
中英文共享正文、四份摘要入口、共享 slides 及其讲稿。目标是明确执行主体、机制、
可推导结论和限制，不把文字修改当作创新、性能或安全资格验收。旧版未被入口加载的
章节及历史审计不回写为当前设计；本轮没有更改产品代码、实验数值或正式验收状态。

本记录对应的机器检查与交付物摘要见
[validation](research-revision-validation.json)。原始编译、静态检查、逐页截图、
PPTX 转换及导出证据保留在 `.codex-tmp/proposal-language-20260910/`。

## Claim Corrections

| 问题 | 修正后的表达及边界 |
|---|---|
| Authorization design 被写成加密主体 | User 使用 ABE 加密 discovery descriptor；服务授权结合 challenge response、信任规则下的签名验证、权限及请求状态检查。 |
| RQ1 像是由请求者发放权限 | Controller 管理服务权限；User/Provider 在调用时检查，User 的任务选择不是授予长期服务权限。 |
| Challenge 的含义不清 | Requester Challenge 在 Request 中受保护，由 ACK 回应；Provider Challenge 在 ACK 中受保护，由 Selection 回应。明确为 ABE-backed invocation path，不声称所有可配置路径均强制 challenge。 |
| Challenge 被等同完整授权证明 | 回应表示能取得受保护的值；还需验证签名、权限和请求状态。不是新权限发放或独占解密密钥的形式化证明，也不阻止获准者主动共享。 |
| 签名正确被等同身份可信 | 需要适用信任规则和信任锚下的证书链验证；裸签名有效性不足以授权执行或证明图像内容真实。 |
| Trust Schema 被当成主动验证者或加密机制 | Schema 定义允许的 Data-name/signing-key 关系；validator 应用规则。内容保密需要另配加密。 |
| 比较将原语与完整授权方案混为一谈 | 比较执行授权与发现内容加密两个维度；DNMP-inspired 对照需配套加密、密钥分发和请求状态，不声称这些补充均在 DNMP 原文实现。 |
| Request ID 被说成自行绑定/授权 | 经验证消息中的 ID 用于匹配请求；ID 本身不授予权限，也不表示前驱角色与依赖边。 |
| 规划策略被当成签名者，签名被当成唯一性 | User 应用规划策略并签名；Providers 验证计划后导出本地规则。唯一提交依赖可信 User 记录并拒绝冲突计划，不由签名单独保证。 |
| 候选快照似乎代表全网/未来资源 | 只冻结窗口内观察并接受的候选；选定 Provider 仍须检查届时资源。 |
| 拟议协作规则看起来已全部通过测试 | 明确 proposed acceptance contract；已有调用实验不证明完整协作或所有原生路径正确。 |
| “安全返回图像密钥”缺少机制 | 在给请求者的加密结果 Content 中提供允许分享的图像密钥，引用原生产者的签名 Data；加密 Content 不隐藏外层名称。 |
| 缓存被等同无效旧对象或免检对象 | 直接、缓存及 Repo 获取使用相同任务检查；新计划可以显式允许旧结果复用。 |
| 运行时状态和撤销表达省略条件 | Controller 发布签名状态，参与者获取验证后应用；保留传播和换钥成本，不宣称离线即时撤销或收回历史明文。 |
| 本地/native 测试被概括成不能跨进程 | 改为单进程测试不能证明跨进程/多设备正确；不按实现语言代替证据边界。 |
| 启动/collector 失败被统一当作测试工具错误 | 必须区分产品和测试设施原因；测量尚未启动的失败不能直接计为测得的协议失败。 |
| 实验标题暗示因果证明 | 改为 Fewer Execution Starts than the Parallel Baseline；保留配置、样本、完成率及不能推出字节/CPU/能耗收益的限定。 |

## Source and Literature Checks

以下为本轮只读核对，不等同重新运行产品测试：

- [ServiceUser.cpp](../../../ndn-service-framework/ServiceUser.cpp)：ACK 路径约 8056–8099 行检查权限与活动请求中的 UserToken；不只检查裸签名或请求 ID。
- [ServiceProvider.cpp](../../../ndn-service-framework/ServiceProvider.cpp)：Selection 路径约 14286–14315 行接受匹配的 ProviderToken 或相应 proof hash。因此正文用 challenge response，不限定为总是回传原始值；`m_useTokens` 是配置边界。
- [ServiceController.cpp](../../../ndn-service-framework/ServiceController.cpp)：约 1029–1044 行构造 OR policy 并发行对应身份的策略密钥；权限聚合不等于固定大小/免费更新。
- [Schematizing Trust in Named Data Networking](https://named-data.net/wp-content/uploads/2015/11/schematizing_trust_ndn.pdf)：规则限制 Data 与 signing-key 名称关系，由验证逻辑递归检查到信任锚。不能把 Schema 自身写成加密操作。
- [DNMP](https://named-data.net/wp-content/uploads/2019/10/kathleen.pdf)：角色/命令约束签名与验证作为具体先例。本文新增的加密及 NDNSF 状态流程明确属于对照设计，不冒称原文能力。
- [NDN Data Packet](https://docs.named-data.net/NDN-packet-spec/current/data.html)：Content、名称、签名的作用分开；FreshnessPeriod 是缓存新鲜度，不等同执行权限或取消/重放状态。

## Slide-by-Slide Ledger

以下页号为修订后的 39 页版本。FIX 表示文字/逻辑修正；RETAIN 表示检查后保留，
均不代表相应研究任务已经验收。参考文献页不受普通内容页 100 词限制。

| Page | Topic | Disposition |
|---|---|---|
| 1 | Title | RETAIN：Proposal 身份及时间一致。 |
| 2 | Several Views, One Result | RETAIN：案例包含采集、计算、返回。 |
| 3 | Three Requirements | FIX：图像签名不能证明任务角色分配。 |
| 4 | NDN Building Blocks | FIX：分开信任验证、服务权限、内容加密和缓存边界。 |
| 5 | Research Questions | FIX：检查服务权限；应用是验证负载，不是独立贡献数量。 |
| 6 | Related Work | RETAIN：保留最近工作逐特征比较尚待完成。 |
| 7 | Contribution/Evidence | RETAIN：设计、实测与正式验收分开。 |
| 8 | Participants | RETAIN：组织授权与本次 Provider 选择分开。 |
| 9 | Four Message Types | FIX：Selection 分配任务并携带 challenge response；正 ACK 不启动任务。 |
| 10 | Descriptor/Input | RETAIN：按参与判断与执行需要划分字段。 |
| 11 | Confidential Discovery | FIX：User 是加密主体；保留非获准读者和外层可见性限制。 |
| 12 | Dissemination/Access | FIX：能读的是密钥持有者；合并权限不等于合并明文加密范围。 |
| 13 | Permission Checks | ADD：两方向 challenge 与其余检查、证明边界。 |
| 14 | Authorization/Encryption | FIX：完整对照；规则与 validator 分开；DNMP 补充机制标明。 |
| 15 | Alternative Designs | FIX：不宣称 ABE 普遍优于完整替代方案。 |
| 16 | Lifecycle | FIX：状态先获取验证后应用；请求者保留密钥访问；撤销成本保留。 |
| 17 | Threat Model | RETAIN：可信规划、恶意数据、主动密钥共享等边界。 |
| 18 | Plan Generation | FIX：User 规划并签名；候选快照不保证未来资源。 |
| 19 | Request ID/Plan | FIX：ID 不是签名或权限；缩短表格措辞并取消首列强制两端对齐。 |
| 20 | Plan-to-Data | FIX：明确验证主体、对象格式及 proposed contract。 |
| 21 | Failure State | FIX：旧执行隔离与显式结果复用。 |
| 22 | UAV Flow | FIX：签名 Data 包含加密图像 Content；加密结果交付原图引用及允许的密钥。 |
| 23 | UAV Evidence | FIX：缓存命中不免除任务检查；识别精度不作无证据假定。 |
| 24 | DI Flow | RETAIN：命名依赖与计算划分分开，Repo 与机会缓存分开。 |
| 25 | DI State | FIX：单进程检查的证据边界；KV 与驻留权重分开。 |
| 26 | Baselines | RETAIN：完整配置公平比较，不以简单配置替代框架能力。 |
| 27 | Mobility Completion | RETAIN：原始数值及无整体优越性结论。 |
| 28 | Mobility Tail | RETAIN：定义子集及置信区间边界。 |
| 29 | Execution Starts | FIX：标题报告观察，不能推出无测量的资源/能耗优势。 |
| 30 | Negative DI Result | RETAIN：保留较慢结果及不能外推新架构。 |
| 31 | Experiments | RETAIN：待完成比较、反例、指标，不计为已完成。 |
| 32 | Timeline | RETAIN：明确 2026/2027；沿用上轮已引用日期，本轮未重新核验学校日期。 |
| 33 | Defense Claims | FIX：ABE-encrypted discovery 与 ABE-backed authorization 分别命名。 |
| 34 | Scope Discussion | RETAIN：范围及删减决策明确。 |
| 35 | Discovery Backup | RETAIN：实验配置不等于框架固有限制。 |
| 36 | Loss Backup | RETAIN：单次历史值非新 matched experiment。 |
| 37 | NDN References | RETAIN：第一组参考文献。 |
| 38 | Invocation References | RETAIN：第二组参考文献。 |
| 39 | Tool/Evidence References | RETAIN：证据记录及学位规划来源。 |

## Validation and Workspace Boundary

六份正文／slides 入口及两份讲稿使用隔离构建目录编译；最终 LaTeX 日志无
Overfull、缺字或未定义引用。英文两入口各 29 页，中文两入口各 21 页，slides
两入口各 39 页，讲稿两入口各 8 页；同语言／同用途入口提取文本一致。
逐页检查正文和 slides 联系表，放大检查摘要、权限检查及比较表；LibreOffice
重导出的 39 页 PPTX 也逐页检查，没有发现缺字、文本越界或重叠。

普通 PDF 内容页至多 100 词，参考文献页单列。静态语言脚本的 15 个提示经人工
审阅：大部分是已限定的 claim/matched/gate 等词，参考页密度提示不作为内容页
超限；没有把关键词扫描当作正确性证明。PPTX 转换检查所有源文本 span 恰好
分配一次、背景不含可提取文字；精确计数见 validation。讲稿百分号解析两个回归
测试通过。原实验页的数字逐项比对未改变。没有运行 Microsoft PowerPoint 或
Google Slides 客户端，因此不声称完成这两个客户端的实测验收。

工作区已有并行改动未纳入本轮文档 checkpoint：`docs/failure-log.md`、Spec182 的
`compatibility-manifest.json`、`native-dependency-design.md`、
`native-generation-design.md`，以及三个 native grant/stream/unary process Python
driver。`tasks.md` 只提交本轮 LANGUAGE REVIEW 旁路记录，其余并行进度保留在工作区。
未跟踪缓存、构建树、原始日志及其他未跟踪文件不纳入提交。

## Next Research Step

逐特征相关工作比较、完整授权替代方案的成本实测、多 UAV/DI 协作的端到端反例与
恢复证据仍未由本轮完成。文档措辞更准确不意味着正式答辩已就绪。下一步应按
RQ1/RQ2/RQ3 的证据矩阵补齐结果，再决定哪些条件性论断可以提升为实验结论。
