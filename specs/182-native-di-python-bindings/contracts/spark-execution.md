# Spark Execution Contract

历史兼容入口；所有执行者统一使用 [Execution Unit Contract](execution-units.md)。
当前进度见 [Execution Progress](../tasks.md#execution-progress)。
T001-C（2026-09-07）已冻结实际 build identity、L0 命令与每卡 planned suite/case
selector 到 [case-manifest](../../../tests/fixtures/spec182/case-manifest.json)；
实现卡按 manifest 的 selector/file/executeOwner 领取与运行。

## Execution Cards

见 [execution cards](execution-units.md#execution-cards)。

## Verification Commands

见 [verification commands](execution-units.md#verification-commands)。
