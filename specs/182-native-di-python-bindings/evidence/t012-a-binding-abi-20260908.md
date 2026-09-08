# T012-A Native Binding ABI Verification

## Status

PARTIAL。候选 native library 与 Python extension 已在同一依赖前缀下重新构建，
extension 可以导入，四组 Spec182 Python focused suites 共 21 cases 通过。该结果
关闭了本轮 ABI/source-closure 观察项，但没有关闭真实 C++/Python 请求 parity、完整
runtime construction、跨进程交付或 T016 qualification，因此 T012-A 继续保持
`PARTIAL`。

## Build boundary

本轮使用 system-first toolchain 和候选依赖闭包：

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin
CXX=/usr/bin/g++ CC=/usr/bin/gcc
NDNSF_LIBRARY_DIR=.codex-tmp/spec182-r4-b2/build
NDNSF_NAC_ABE_PREFIX=/home/tianxing/NDN/nac-abe-integration-182/install
NDNSF_NDN_SVS_SOURCE_TREE=/home/tianxing/NDN/ndn-svs
NDNSF_NDN_SVS_BUILD_TREE=/home/tianxing/NDN/ndn-svs/build
python3 setup.py build_ext --inplace --force
```

`libndnsf-distributed-inference.so` was first relinked in the configured Waf tree so
`NativePlanning::descriptorDigest()` was present before the extension build. The extension
RPATH resolves `libnac-abe.so` to the explicit candidate prefix and the Core/DI libraries to
the same candidate build; `libndn-svs.so` resolves to the recorded SVS build tree.

## Focused result

```text
PYTHONPATH=pythonWrapper python3 -m pytest -q \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_closure.py \
  tests/python/test_ndnsf_python_service_response_binding.py
result: 21 passed in 0.36s
PYTHONPATH=pythonWrapper python3 -c 'import ndnsf'
result: PASS
```

Durable command output, `ldd`, import output and SHA-256 records are in
`.codex-tmp/spec182-t012-a-binding-abi-20260908/` (local ignored evidence; regenerate
after any source commit that changes the extension or its native dependencies).

## Miss taxonomy and remaining work

- **Static findings**: the binding source already had explicit candidate Core/DI checks and
  optional NAC-ABE/SVS prefix inputs; no new static finding was accepted from this run.
- **Compile/link misses**: the first extension build selected `/usr/local/lib/libnac-abe.so`
  and failed import with an undefined `Consumer::clearCache(Name,string)` symbol. The DI
  shared library was also stale relative to `NativePlanning::descriptorDigest()`. Both
  boundaries were repaired by selecting the explicit NAC-ABE prefix and relinking the
  configured candidate Waf tree before rebuilding the extension.
- **Runtime/test misses**: none in the 21 focused cases after repair; this is a binding
  closure result, not proof that the default requester has a configured native preparation
  and offer-admission pipeline.
- **Remaining**: expose or configure the complete native request runtime for maintained
  callers, prove C++/Python behavior parity, and run the final ABI/qualification gates in
  T016. The existing no-prefix setup compatibility tests remain intentionally supported;
  making the NAC-ABE prefix mandatory would be a separate interface change.

## Coverage matrix

| Lane | Status | Evidence |
| --- | --- | --- |
| production entry/callers | covered | `pythonWrapper/setup.py` extension entry and `pythonWrapper/src/ndnsf/di_bindings.cpp` exports; import command above |
| implementation and wire | covered | `pythonWrapper/src/ndnsf/di_bindings.cpp`, `_ndnsf.cpp`, and candidate `libndnsf-distributed-inference.so`; `ldd`/symbol checks in durable evidence |
| test/harness/oracle | covered for binding closure; gap for full request parity | four focused Python suites, 21 cases; complete requester runtime remains T012-B/T013 scope |
| build/source closure | covered for this local candidate | explicit Waf candidate build, explicit NAC-ABE/SVS paths, extension RPATH and SHA-256 records |
| migration/evidence | gap | maintained caller migration and final qualification remain T012-B/T013/T016; durable local run directory is recorded above |

