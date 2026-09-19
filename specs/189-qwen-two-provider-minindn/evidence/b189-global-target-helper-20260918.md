# B189 Single-Target Global Install Evidence — 2026-09-18

## Scope

本记录只验证宿主机的增量构建入口，不把它计为 Qwen、MiniNDN 或两 Provider
资格。目标是确认普通 NDNSF-DI 源码变化可以只编译并安装一个 Waf target，且不会
重新编译外部 ONNX Runtime、触发 Repo 的 editable Python 安装或引入临时依赖路径。

## Static gate

The read-only review-agent returned `STATIC_PASS` for the frozen r7 snapshot:

| Artifact | SHA-256 |
| --- | --- |
| `tracked.diff` | `299dd0ba7f847f0c55ac2e5d2a6465abcdec40ab632372ba27bc099eea833448` |
| `scripts/install-global-target.sh` | `1d30ac4582903e924d2540c355bc5832817816dd2f635e0730a61dee80c4047d` |
| `scripts/spec180_native_build.py` | `49ff4fe41026985cf875d5908223b0d6ea8d0dadda3d8f1ce952c0b0c62d045e` |
| `tests/python/test_spec180_native_build.py` | `5268ff320c9147da59f9f559835b51eb9c519ec309a3767940ff7c05f7c3fc1c` |

The review covered one-target argument handling, duplicate argument rejection, the
external ONNX boundary, global receipt/cache/RPATH/compile-flag preflight, and the
`NDNSF_SKIP_DEV_PIP_INSTALL=1` side-effect guard.

## Focused checks

```text
PYTHONPATH=. /usr/bin/python3 -m pytest -q tests/python/test_spec180_native_build.py
100 passed in 5.76s

bash -n scripts/install-global-target.sh
shellcheck scripts/install-global-target.sh
python3 -m py_compile scripts/spec180_native_build.py tests/python/test_spec180_native_build.py
```

The new negative cases reject split `-I`, `-isystem`, and `-L` tokens that point
outside the canonical global roots.

## Targeted build and install

Command:

```text
scripts/install-global-target.sh \
  --build-dir build-spec189-b189-3-global-r3 \
  --target ndnsf-distributed-inference --jobs 4
```

The helper first reported `Installed NDNSF global dependency closure is valid`,
then `SPEC180_NATIVE_IDENTITY_OK` for the configured tree. The selected target
build completed in `6.493s`; target installation completed in `5.406s`. The log
is retained at `.codex-tmp/spec189-global-target-helper-20260919/ndnsf-distributed-inference.log`.

The build and installed DI libraries match:

```text
sha256 243511c4479fe028a1392c0a52ba8893befae656ee79b14e1dfbbadcd54c0bc6
```

`ldd /usr/local/lib/libndnsf-distributed-inference.so` resolved
`libonnxruntime.so.1` to `/opt/onnxruntime/lib/libonnxruntime.so.1`, and the
NDN-CXX, NDN-SVS, NAC-ABE and OpenABE DSOs to `/usr/local/lib`. The library
RUNPATH is `/usr/local/lib:$ORIGIN`; no `not found` entry was present. The Waf
install log explicitly reported `Skipping dev pip install of pythonWrapper
(NDNSF_SKIP_DEV_PIP_INSTALL=1)`.

## Decision and limits

This is `TARGET_BUILD_INSTALL_PASS` for the host DI library helper. It proves the
incremental install boundary and loader identity only. It does not prove Python
binding freshness, native Qwen execution, Repo protected publication, ACK/Selection,
two-provider execution, resource drain, MiniNDN, Tiger, or `QWEN_TWO_PROVIDER_PASS`.
The external ONNX Runtime SDK remains a versioned global input; it is not rebuilt by
this helper or by ordinary DI source changes.
