# Symbol Contract And Usage Template

## Coverage Rule

为受影响的每个类/结构体/枚举/别名、函数/方法/重载、成员/共享变量、
配置项、序列化字段建立唯一记录。新增、修改、移动、删除均覆盖；
原样复用记录精确来源、依赖契约和实际消费者，不复制整个上游库。
变量范围包括影响行为的局部量（deadline、digest、计数边界、暂存资源等）。
循环下标等无契约意义的局部实现细节可列为 LOCAL_DETAIL，并解释归类规则；
不要为了“每个变量”预写整份实现，也不要漏掉有安全/状态意义的 private 字段。

记录采用 stable ID；Spec 索引 → 符号契约 → 工作单元 → PO 一一可追踪。
同名类型、重载及不同 namespace 分开标识。类型出现在签名中就必须有来源：
existing（定位源码）、planned（完整定义）或 OPEN（明确阻断范围）。
“由编码时决定”“以后设计任务补齐”不算已经交付该符号的详细设计。

## Class Contract

| Field | Required content |
| --- | --- |
| SymbolID / Kind / Path / CD / Task | 精确名字、namespace、声明/实现/导出文件、增改删移或复用 |
| Purpose / WhyHere | 解决什么问题、为什么需要此对象、为什么放在此层；职责与非职责 |
| Before / After | 现有实现/absent；目标行为与保留部分；相对源码的实际 delta |
| Collaborators | 构造依赖、调用者、被调用者、依赖方向；是否已有可复用对象 |
| Construction / Destruction | 验证、初始化值、资源获得、注销/释放顺序；部分初始化失败 |
| Members | 每个字段的 Field Contract ID，不以“保存上下文”替代说明 |
| Methods | 每个方法/重载的 Function Contract ID，包括生命周期方法 |
| Invariants / Concurrency | 事实 owner、可变性、线程/锁/执行器、重入与复制/移动规则 |
| Usage / Migration | 可构造所有实参的最短调用例；旧入口前后对照；弃用/删除条件 |
| Documentation | 将写入代码的职责/边界注释，位置与语言 |
| Proof / Readiness | 独立 oracle、错误实现检出、OPEN 和受阻任务 |

## Function Contract

逐项列出：精确前后签名（可见性、参数名/类型/默认值、const/static/async、
返回与错误约定）、目的与改动理由、每个参数的字段契约、调用前置条件、
关键算法/分支顺序、校验与提交点、返回分支、外部副作用、取消/超时、
异常与部分失败、回调/资源寿命、调用方迁移及验证。

返回 future/handle 时区分“调用被接收”“结果可用”“资源清理完成”。
区分参数错误、等待超时、业务拒绝和底层运行失败；不以异常文本当稳定协议。
至少为每组有独立行为的 API 提供成功与失败/取消调用例。
示例必须使用所定义的方法和参数，不能出现未声明 helper；
若依赖尚未确定，示例显式 DESIGN_EXAMPLE / NOT_COMPILED，不伪造可运行性。

## Field Contract

| Field | Required content |
| --- | --- |
| FieldID / Owner / Name | 方法参数、成员、局部量、配置或 wire 字段的精确归属 |
| Type / Shape / Unit / Encoding | 完整类型、范围、长度、时间单位、字节/文字约定 |
| Meaning / Necessity | 代表什么，为何存在；能否可靠从其他权威值推导 |
| Source / Authority | 创建者；唯一事实或派生缓存；与相似字段的区别 |
| Initial / Default / Absent | 初值来源、缺省语义；不能以任意默认掩盖缺输入 |
| Readers / Writers | 谁读写，允许状态/转换；不能泛称“runtime” |
| Lifetime / Ownership | copy/borrow/move/shared，跨异步存活、重置、销毁和释放 |
| Validation / Invariant | 谁在何时校验；冲突/越界/重复/过期时具体行为 |
| Concurrency / Security | executor/锁/原子性，敏感性、脱敏/零化、跨权限边界 |
| Persistence / Compatibility | 是否入 journal/wire/config，版本、迁移及失效 |
| Comment / Proof | 要写入代码的语义注释；关联方法/PO 和反例 |

禁止无限制 context/dict 装入未知字段以规避字段清单。仅内部表示可自由选择的
字段须写出允许选项及保持的不变量；影响公开接口/寿命/格式的选择先关闭 OPEN。

## Documentation Contract

注释解释为什么、何时有效、谁拥有、有什么边界；不逐行翻译代码。
C++ 对新增/变更公开 API 提供英文 Doxygen 风格声明注释：
brief、每个参数、返回/失败、ownership/lifetime、thread safety。
Python 绑定提供英文 docstring，解释原生对象与兼容/异常映射。
私有复杂算法、状态提交点、密码/资源边界写必要英文行内注释；
普通实现语句无额外注释要求。Spec 中文叙述保留。
记录 annotation text 或精确内容义务及源码位置，不能只写“加注释”。

示例（模板，正式 Spec 要换成真实符号）：

~~~cpp
/**
 * Submit one request using a frozen policy snapshot.
 * @param options Validated per-request options, copied before returning.
 * @return A handle; submission is not proof of execution or cleanup.
 * @throws RequestError If options violate the public input contract.
 * Thread safety: posts work to the owning executor; never blocks Core I/O.
 */
RequestHandle submit(const RequestOptions& options);
~~~

## Review Gate

接手者应能只读契约回答：改什么、为何改、怎么改、谁调用、
每个输入/字段从哪来、失败后谁收拾、怎么使用、怎样判断正确。
检查源码 diff 候选集与符号清单的双向覆盖，不能凭填写率宣称设计正确。
每个 OPEN 有具体缺口/证据/owner/受阻单元；逐符号 PARTIAL 与范围 READY 分开。
一项未定义类型/成员/调用边界足以阻止受影响单元 READY。
