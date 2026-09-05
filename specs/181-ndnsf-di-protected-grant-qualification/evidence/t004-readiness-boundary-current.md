# T004 — 运行时就绪边界修复

> **Current scope correction (revision 5, 2026-09-05)**: T004 当前 partial：以下旧 Python 测试使用 fake native。新 GDB/NDN 日志与真实 native 探针已定位并修复 pre-thread readiness 误判，80 ms 等待从约 15 us 提前失败修正为约 81 ms，随后正常 READY/catalogue。见 [native live repair](t002-native-live-repair-20260905.md)。完整取消、超时及无热转 integration 尚未闭合，旧 PASS 不代表当前任务完成。
> 当前裁决与下一步以 [audit.md](../audit.md) 为准，以下保留为原始范围记录。

**Layer**: implemented（native waitUntilReady + Python seam 15000 ms）;
executed（7 项 unit 测试全绿，2026-09-05）;无 measured 声明。

Date: 2026-09-05. Source HEAD: commit `18744ca1`
（T004 readiness boundary）。

## 声称

Python `start()`/`start_background()` 的就绪等待为 15000 ms（对 Core
10 s 探针 deadline 留余量，FR-011）;就绪超时或 native 失败时
fail-closed 到 `stop()`;取消/停止路径不得热转事件循环。

## 代码实现（implemented）

- `pythonWrapper/src/ndnsf/_ndnsf.cpp`：`NativeServiceProvider` 新增
  `waitUntilReady(timeoutMs)`——事件线程首个成功切片后置位
  `m_started` 并通知 CV;线程错误或退出也通知（等待立即失败关闭）。
  pybind 绑定 `wait_until_ready(timeout_ms=15000)`。
- `pythonWrapper/ndnsf/service.py`：`ServiceProvider.start()` 与
  `start_background()` 在 `self._native.start()` 后等待 15000 ms;
  异常或超时调用 `stop()` 并抛 `RuntimeError("ServiceProvider
  readiness timeout")`。`ServiceController` 的 15000 ms 等待已存在
  （未改动）。
- `ndn-service-framework/ServiceController.cpp`：探针循环已有完整
  取消机制（`cancelStart()` 标志、每 25 ms 切片检查、`io.stopped()`
  立即退出、work guard 防热转）——审计确认无需修改（:290-357）。

## 测试执行（executed）

`tests/python/test_spec181_controller_readiness.py`（7 tests）:

```
python3 -m pytest tests/python/test_spec181_controller_readiness.py -q
7 passed
```

覆盖：start 等待 15000 ms 且就绪返回;超时 stop+raise;native 失败
stop+re-raise;start_background 等待并返回线程;background 超时 stop;
ServiceController seam 15000 ms;controller 超时 stop+raise。

## Verdict

PASS（T004 范围）。就绪边界在 Python seam 与 native 两侧就位。
