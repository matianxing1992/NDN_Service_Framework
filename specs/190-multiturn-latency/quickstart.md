# Quickstart: Validation Route

## Current State

仅计划；不得照抄planned selector当作已存在的可执行命令。按依赖完成T001–T006及T008–T011，再执行T007。
基线：[research](research.md)。固定Qwen3-0.6B、两Provider、r260三轮输入/采样、1024 token预算。

## Document Checks

```bash
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py specs/190-multiturn-latency --strict
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints
```

## Native and Installed Gates

T001注册后使用planned `spec190-latency-tests` 的PhaseTiming/AckWindow/LiveTurns/TerminalDrain/ResidentSession。
Waf受影响增量构建，系统编译器与已安装依赖，默认-j4；Core/API变化重编真实消费者。
T003–005生命周期selector repeated+asan-ubsan；三组正常/边界/失败用例按plan参数矩阵，不全树重构。
安装使用`scripts/install-global-target.sh`，核对实际库/binary路径hash及`ldd`，不能MiniNDN加载build树。
T006注册并安装 `spec190-multiturn-oracle` 后再运行；`--help`不算行为通过。

## Matched Runs

由维护launcher生成当前profile和turns数组，一次启动C++driver；不新增第二套实验脚本。
使用其实施后验证过的CLI参数冻结完整argv；此处不编造尚未实现的运行命令。
control：ACK60000ms、单轮进程、resident disabled；treatment：ACK1000ms、同handle、resident enabled。
同一candidate同时支持两配置，至少3组交替顺序；另报cold/warm，首次候选预检失败零启动。
成功必须同时具备C++oracle、正确token/KV、子进程退出、cleanup；记录失败和重试，不覆盖raw。
证据在`evidence/b190-07.md`，原始日志新建`.codex-tmp/spec190-<run-id>/`，模型/密钥不进Git。

## Repo and Transport Gates

使用[材料复用契约](contracts/material-reuse.md)的真实Repo-enabled profile；旧compatibility仅作诊断。
每node固定持久根置于launcher reset/cleanup之外，一个native owner持锁；不得按run复制大Repo。
先冷入库一次，再至少3次进程重启/新run验证恢复；重复production `user.prepare(model)`应无split/export/package/STORE，
receipt和材料闭包与冷准备一致。另做缺失层真实fetch及错误身份、半提交、撤销授权反例。
性能control/treatment均相同Repo warm状态；冷启动与重启恢复单列，不混入纯ACK收益。
分别报告layer、assembled、resident命中；layer hit不能当作免assembly/ORT load。
按stage tensor实际shape/dtype计算预算，分别记录payload/wire/重传/控制字节；KV和无关层不得跨stage搬运。
T011先闭合CD-09安全接口再编码；原生安全反例未通过，不用明文兼容缓存替代真实Repo验收。

## Metrics

每轮submit→first-token、prefill、decode间隔、last-token→checkpoint→Provider-terminal、轮间gap；
ACK到达/验证/冻结及规划耗时；session实际load数、KV restore、tokenIds/EOS、RSS/swap、disk delta。
峰值内存不从缓存条目数推导；TTFT不由Provider计算时间代替；总耗时降低不等于decode提速。
