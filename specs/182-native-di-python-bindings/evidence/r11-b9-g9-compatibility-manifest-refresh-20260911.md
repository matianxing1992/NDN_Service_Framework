# R11-B9-G9 Compatibility Manifest Provenance Refresh

在 `729555fa` 的 C++ client-close registry checkpoint 后重新生成
`contracts/compatibility-manifest.json`，避免 caller inventory 继续指向旧 source
identity。该批没有改变 API 映射、运行时行为或 legacy disposition。

```text
python3 specs/182-native-di-python-bindings/checklists/build_api_migration_manifest.py
{"path": "specs/182-native-di-python-bindings/contracts/compatibility-manifest.json", "entries": 344, "dynamicAppSdk": 67}
```

验证结果：`sourceCommit=729555fadd8d61cc89dbb1e820b10aca3a6c786c`，与当前 code
checkpoint 一致；manifest 仍是 routing/inventory evidence，不代替 semantic parity、
maintained caller migration、legacy zero-use、no-Python 或 T015--T017 qualification。
