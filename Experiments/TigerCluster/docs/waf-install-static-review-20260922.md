# Waf install payload static review

> 以下第一轮记录保留为历史。第二轮进一步取消了外部库复制、外置 APP 强制导出和
> 手动测试附件安装；以本文末尾的 Second audit 为当前状态。

## Scope and design binding

用户授权：仅修改 SIF 脚本并静态审查；不执行 configure、编译、Waf install、
Apptainer build、MiniNDN 或 Tiger 作业。适用 B1–B3、S1–S3：外部依赖归 base
自己的构建系统管理，NDNSF 原生安装清单由根 Waf 管理，容器内构建绑定。
本次不改变协议、公开 API 或 Design 目标；运行资格不因脚本检查而升级。

## Changes

- `development-runtime.def.in` 改为 `waf install --destdir`；完整安装树进入最终
  `/opt/ndnsf-di/current`，不再从 build 手工安装生产库、程序、pkg-config 或遍历全部私有头文件。
- Waf 的 selected-target install 显式 post 公共头文件安装任务，不扩大编译 target 集合。
- 禁用 Repo 安装后的开发用途 editable-pip hook；Python 绑定继续在容器内、针对 staged 库安装。
- 加入 Waf 安装清单的摘要/符号链接核对和 builder→final 清单摘要绑定。
- 包含 Waf 安装的 assembly worker，补全 libexec 相对 RPATH 与运行环境路径。
- 开启既有 Spec187 测试目标所需的 `--with-tests`，其容器 RPATH 不再指向已删除的 build 树。
- `di-native-fault-provider` 和 `spec187-yolo-minindn` 是已有非安装测试附件，
  保留两条明确的 build-tree 拷贝例外。旧 APP 兼容导出名单未作为当前完整 SIF 的安装权威。

## Static review checkpoint

已阅读 `review-agent/SKILL.md`，由当前执行者独立切换只读审查阶段；未启动子代理。
调用链：`prepare-development-handoff.py` → 模板渲染 → `validate_definition()` →
builder/final 模板。核对 root Waf、`examples/wscript`、`tests/wscript`、Repo pip hook、
Python setup 与 `Provider::resolveWorkerLocation()`。新旧文件名单与模板语法检查属于
脚本证据，不能证明容器 ABI、原生编译链接或推理成功。

## Check attempts

1. 初始模板离线检查：21 passed。没有调用真实 Waf 或 Apptainer。
2. 加入清单负例与旧 Spec170 gate 的组合检查：28 passed / 8 failed。
   - 新清单负例的 5 个失败首次边界是宿主 Python 3.8 不提供 `Path.is_relative_to()`；
     模板目标 Python 3.10 支持该方法，但静态 fixture 需兼容宿主解释器。
   - 旧 `test_spec170_exact_sif_gate.py` 3 个失败为其旧 definition fixture 缺少已有的
     `PYTHON_NAC_STAGE_PREFIX` 标记；失败发生于本次新增 Waf 检查之前。
   - pytest 临时输入保留于 `/tmp/pytest-of-tianxing/pytest-279/`；未发生原生构建或容器尝试。

Changed gate：新增安装目录逃逸、摘要改变、缺文件、符号链接改变的离线负例；
移除新函数对宿主 Python 3.9+ pathlib API 的依赖后再检查。

3. 修正后的模板定向检查：`python3 -m pytest -q
   Experiments/TigerCluster/tests/test_development_runtime_template.py`：27 passed。
   包含两个 `%post` 的 `sh -n`、内嵌 Python AST、头文件缺失/过期/多余、
   安装清单损坏/路径逃逸、跳过 Waf install、遗漏安装树转移及匿名 header task 负例。
4. 将 HEAD (`63ac2556`) 的原版 boundary validator 加载到独立 Python 进程，
   原样运行旧 Spec170 9 项测试：同样 3 failed / 6 passed，确认上述三个失败为
   已有 fixture 问题；未削弱现行校验来使旧 fixture 通过。
5. `git diff --check`：通过。本轮没有 configure、编译、链接、真实 Waf install、
   Apptainer、容器导入或 MiniNDN 行为证据。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Result |
| --- | --- | --- | --- | --- |
| production entry/callers | covered | `prepare-development-handoff.py::render`、`validate_definition` | 精确源码查询渲染与校验调用 | 新模板可经既有入口消费；冻结旧 bundle 不自动改变 |
| implementation and wire | covered | builder/final、Waf install receipt、`Provider::resolveWorkerLocation` | 完整本次 diff；路径/摘要/worker 环境核对 | 无协议/API 改动；文件路径不构成运行通过 |
| test/harness/oracle | covered | `test_development_runtime_template.py` | 27 个离线检查；旧 validator 基线复核 | 定向通过；旧 fixture 的三个失败独立保留 |
| build/source closure | covered | root/examples/tests Waf、Repo install hook、Waf `post_group/get_tasks_group` | 源码手推 selected-target install 与匿名安装任务 | 不扩大原生编译列表；未执行编译链接 |
| migration/evidence | covered | 兼容 APP 导出、旧 Spec170 fixtures、本记录 | 保留测试附件例外；HEAD 前后失败比较 | 不宣称旧外部 APP 交付完成迁移或新版 SIF 已合格 |

## Batch Retrospective

- `static`：复审修正公开/私有头文件边界、遗漏 worker、测试配置和 RPATH；
  只读复审本次差异后，没有发现尚未处理的本次引入缺陷。
- `compile/link`：NOT_RUN，遵照用户要求。
- `runtime/test`：只运行 Python/静态 shell fixture；宿主 pathlib API 问题已修正。
  旧 Spec170 fixture 三个失败已用 HEAD validator 复现。
- `unobserved`：容器内的编译器、Waf install 实际输出、动态 ABI、绑定导入、
  base 身份和完整推理不能由本轮静态检查证明。

## Remaining validation

已完成最终静态复审；本轮禁止构建，因此实际 Waf install、容器动态库闭包、绑定导入、
worker 启动和最终 SIF 仍为 UNVERIFIED。下一步只在另行授权构建后验证这些边界。

## Handoff and checkpoint

本次修改保留在工作树，未混入先前 staged 的 DI slides，也未提交并行修改。
checkpoint 前只执行既有 `pre-commit` 守卫（不执行构建）：返回 1，原因是索引中
已有 development-assistant references；完整输出 `/tmp/ndnsf-sif-hook-9bVVPl.log`。
没有绕过守卫、改动 hook 或自动 push。没有新 commit。

本次文件：runtime 模板、boundary 校验器、对应静态测试、root `wscript` 的安装任务
posting 段、`tests/wscript` 的 Spec187 容器 RPATH，以及本审查/失败索引/Spec checkpoint/
NO_DESIGN_CHANGE 记录。共享文件内已有 configure preflight、模型测试与实验记录等
并行改动保持不动。后续交付必须同时包含根 Waf 的新安装 posting 段；只复制模板不完整。

最终复查：27 passed；root/tests Waf 与校验器 Python AST 均通过，模板 shell/Python
语法由上述定向 suite 检查。尚不能声明镜像可构建或原生运行合格。

## Second audit — installed-v1

第二轮用户要求继续审查非必要包装操作，仍不构建。已定位并修改：base 外部库/头文件
重复复制、base SDK 源码/build 路径覆盖、旧外置 APP 强制导出、Waf 之外的测试附件安装、
Python extension 不正确的相对 RPATH，以及对 base 外部库/Python 包的静默删除。
新增 `installed-v1` 完整 SIF 布局，旧 APP 模板仍走原兼容校验；新布局不是外置 APP 来源。
base 若含旧 NDNSF 则明确报错并要求修复/reseal base，而不是在应用层悄悄修补依赖。

首轮新静态 fixture：39 passed / 1 failed / 5 deselected。唯一失败是 transfer mutation
误匹配 builder 的同名清理参数，未真正修改 `%files from builder`，不是产品校验漏检。
Changed gate：mutation 使用完整 section 标题和转移行，确保改变真实边界后再复查。
未执行 configure、编译、真实安装、SIF 构建或集群操作。

### Final result

**当前结果是脚本静态收敛，不是 SIF 构建合格。** 当前模板由约 708 行缩减为约
537 行，取消了非必要的 base 文件复制和外置 APP 生成；没有以删除 ABI 检查换取简化。

| 原操作 | 当前处理 | 边界 |
| --- | --- | --- |
| 手工复制 Waf build 文件 | 根 Waf 安装整树，再整体传递 | 生产程序、公开头文件、SONAME、pkg-config、worker 归同一 owner |
| 两个测试程序手动 install | 默认关闭的 `--install-experiment-fixtures` | 当前 DI development 模板显式启用，不默认编译全部测试 |
| base 的 SVS/NAC/Relic 等复制进应用 lib | 直接使用 base SDK | 元数据前缀核对、SDK 摘要和 ldd/hash 校验保留 |
| SVS 源码/build pair 强制覆盖安装 | 取消覆盖，使用 base 的 pkg-config | 两个 Python binding 同样不再注入该 pair |
| 两份 APP/current 原生树与外置 APP 拆包 | 当前只产完整 installed-v1 SIF | 旧 exporter 明确拒绝此布局；冻结旧模板不改写 |
| 删除 base 外部库、torch 等来修镜像 | 不改写 base 外部依赖 | 旧 NDNSF 残留明确报错；必要时单独更新 base |
| venv 扩展使用错误 `$ORIGIN/../../lib` | 嵌入声明的最终 NDNSF lib 前缀 | 编译链接仍指向 stage，实际 ABI 未运行验证 |
| 登录节点版本决定构建版本 | 固定 1.5.3、保留 binary hash 记录 | legacy OCI 入口不再自动 SSH 选择版本 |
| 仅构建也要求完整实验 gate | 文档将 `--build-only` 作为普通候选构建示例 | `BUILT_UNQUALIFIED` 与正式 release 验收分开 |

复审补充：base 的旧 NDN-CXX loader alias 可指向相同的 canonical base 文件，
因此不把整个旧 `/opt/ndnsf-di/current` 目录的存在误判为含 NDNSF；只拒绝已知
NDNSF-owned 文件/目录。依赖比较规范化此类 base alias，不允许任意宿主库混入。
`validate-local-sif-build-record.py` 对新布局要求 `cleanDependencyBaseRequired=true`，
不伪称已删除旧文件；旧布局仍执行原 stale replacement 要求。

最终离线检查命令：

```text
python3 -m pytest -q --tb=short \
  Experiments/TigerCluster/tests/test_development_runtime_template.py \
  tests/python/test_spec170_sif_build_record.py \
  Experiments/TigerCluster/tests/test_sif_app.py \
  -k 'not run_container and not native_link_contract'
```

结果：**61 passed, 1 skipped, 5 deselected**。skip 为需要容器 APP/lib 来源的旧 ELF 检查，
deselected 为 run-container 路径；没有将它们算作通过。包含 shell `-n`、Python AST、
definition 负例、Waf fixture 注册、清单损坏/路径逃逸、build-record 两种布局、旧 APP
入口拒绝新布局等。此前旧 Spec170 definition fixture 的三个基线失败不因此自动修复。

本轮读后修正与复审覆盖：根/examples/tests Waf；maintained build-local、base builder、
base-runtime/dependency-sdk 的相应 ownership 路径；development 模板及 renderer；
boundary/build-record/legacy APP consumers。没有声称逐行验证所有历史 Slurm job 或
所有外部项目的构建脚本。CodeGraph 定位 validator，非索引模板以精确文本与实际 caller 核对。
Context Mode project health 通过；active task 状态以仓库为准，不使用统计摘要推定。

### Explicit limits and next step

- 不承诺“所有模块程序已经都在此 profile 中”：该模板仍是明确的 DI development profile。
- DI 的历史 pure-Python tree 仍作为 compatibility package 复制到安装目录；不能称为
  Python owner-wheel/profile 迁移已完成。直接把它换成 aggregate pip 会漏掉尚未完整
  纳入 owner packages 的旧模块，因此本轮不假装这只是换一条命令即可安全完成。
- base 的初次 legacy→SDK 转换仍有从既有已验证镜像导入 OpenABE/Relic 材料的代码。
  这是 base 构建输入边界，不应在每次 NDNSF 更新时重复执行；没有改写已封存镜像。
- 仍为干净容器内编译；没有跨镜像增量对象缓存，没有测量时间。下一步在授权构建后，
  对固定 base 的一个新候选检查安装清单、ABI、绑定、worker 与 smoke，再讨论增量缓存。
- “与本机一致”是同源码、同安装规则、可核对的行为；不同 Python/glibc/GPU 平台不能
  保证二进制相同，宿主编译产物不得用于绕过容器 ABI 边界。

第二轮改动保留在工作树，无新 commit：既有 hook 阻止 checkpoint，未绕过守卫，
也未把已 staged 的 slides 或其他并行 Waf/模型改动一起提交。
