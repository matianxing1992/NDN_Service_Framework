# Native external Runner fixture

This source is intentionally outside `NDNSF-DistributedInference/cpp`. It
includes only the public `NativeModelRunner.hpp` contract and compiles into an
object without ONNX, Qwen, APP, planner, NFD, or model dependencies.

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I. \
  -c tests/native-external-runner/runner.cpp \
  -o /tmp/spec111-native-external-runner.o
```
