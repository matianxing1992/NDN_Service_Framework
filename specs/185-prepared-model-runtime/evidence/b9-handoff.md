# B9 Design API and Scoped Handoff Evidence

## Status and scope

`PASS` — T014 文档单元已完成静态与批末组合审查；生产代码和 native qualification 沿用
前序批次的已验证证据。
本批次不修改生产代码、不运行 native qualification；它交付当前/目标 Design、API 参考、
源码身份、三类图、PDF 和 Spec184 边界。

## Inputs and current identity

- Active feature: `specs/185-prepared-model-runtime`。
- Governing contract: `Design/MANAGEMENT.md`、Spec185 C-04/C-05/C-06/C-07/C-08 和
  Spec184 outstanding registry。
- Current API generation: `build-api-reference.py --changed-only` followed by
  `build-behavior-coverage.py` and `refresh-snapshot.py`.
- Current snapshot: `Design/source-baseline.json`, baseline commit
  `3c7c11c75d151ac87577e7a2ea2f863bfe551061`, 514 files; worktree patch is retained at
  `Design/evidence/source-baseline-worktree.patch`.
- Current API inventory: 327 files, 17,589 declaration entries; Python binding map: 1,114
  operations; behavior coverage: 6,168 functions.

## Design/code boundary

当前设计明确 `Runtime::open → User::prepare → PreparedModel → NativeInferenceClient` 是
已实现并由 B7 C++ 过程矩阵验证的原生入口；`api/_async.py` 只转换 native completion、
EventReader、GIL 和 asyncio。没有 preparation/request contract 的兼容构造继续返回
`NATIVE_REQUEST_PIPELINE_NOT_READY`，不把兼容分支误报为生产 prepared 路径。

目标设计仍独立保留 TG-01--TG-05 的 `PLANNED` 内容。Spec184 的 Qwen3.6-27B、继承
negative/retirement、I05、Python retirement、SIF/Tiger 资格和 B8 subinterpreter/wheel
压力均保留原 `PARTIAL`/未观测状态。

## Static and document checks

| Lane | Result | Evidence |
| --- | --- | --- |
| `production entry/callers` | covered | 当前 API reference、PreparedModel/NativeInferenceClient 源码摘要、B7/B8 evidence 链接；B9 不改生产入口 |
| `implementation and wire` | covered / N/A for document-only changes | 当前/目标分离、C-05/C-06 exposure、兼容 not-ready 边界和 `api-contracts.json` AC-14 更新；无 wire 修改 |
| `test/harness/oracle` | covered | B7 C++ native oracle 与 B8 wrapper checks 被分别引用；B9 仅运行文档验证，不以文档 PASS 替代产品测试 |
| `build/source closure` | covered | `test_design_state.py`、`verify-api-reference.py`、`verify-source-baseline.py`、`build.py` 和 `verify.py` |
| `migration/evidence` | covered | `spec-design-changes.md`、Spec185 tasks、B7/B8 evidence、Spec184 registry 和 source patch identity |

Initial static checks before review:

```text
test_design_state.py             PASS (5 tests)
verify-api-reference.py          PASS (327 files, 17589 entries, 1114 bindings)
verify-source-baseline.py       PASS (514 files, patch SHA256=3f32d391229fbff0fb729cf396e64ce30439075bcf8cba17e5b8915ad7e902ae)
```

## PDF build and layout receipt

`python3 Design/build.py` completed successfully in
`.codex-tmp/design-pdf-20260915T130303244487Z/`; `python3 Design/verify.py` returned `PASS`.

| PDF | Pages | TOC sections | SHA-256 |
| --- | ---: | ---: | --- |
| `Design/current-design.pdf` | 91 | 59 | `5d41a247e798352e5c05f5794c17f04cf88a11f216d9afb3e35c49881eba1c57` |
| `Design/target-design.pdf` | 98 | 65 | `c86b76eee4b9fd55a1e46314a8bfbee58b70e69ba37d4fa61328a040c1b727bd` |

Both PDFs embed fonts, contain no verify warnings, and pass `pdfinfo`/`pdftotext` checks.
The current and target technical bodies intentionally differ; the target is not regenerated
from the current snapshot.

## Review and closure

The immutable review snapshot `.codex-tmp/spec185-t014-review-v05-20260915` was reviewed with the
official `/home/tianxing/.codex/skills/review-agent/SKILL.md` and returned `STATIC_PASS` with no
P0–P3 findings. Its base is `3c7c11c75d151ac87577e7a2ea2f863bfe551061`, with 25 paths,
diff SHA-256 `719f9353356460ddd54029046080cddfdbde72958931031f935eb68b87af5772`, and paths
SHA-256 `9b2f39deac0d18aaae940fe2995f523c2b5da798ed4ceb8366d6fc6333ae32d8`. The five lanes
were covered: production entry/callers; implementation/wire; test/harness/oracle;
build/source closure; and migration/evidence. The review confirmed current/target separation,
the explicit compatibility `NATIVE_REQUEST_PIPELINE_NOT_READY` boundary, and synchronized
T007/T008/T014 task states. No compile-link, runtime, or sanitizer was required for this
document-only batch; native and Python evidence remains referenced separately.

The B9 final composition review first returned `B9_COMPOSITION_PASS` on immutable snapshot
`.codex-tmp/spec185-b9-composition-v01-20260915`. After recording the final closure metadata,
a second immutable snapshot `.codex-tmp/spec185-b9-composition-v02-20260915` was reviewed and
also returned `B9_COMPOSITION_PASS` with no P0–P3 findings. The final snapshot base is
`3c7c11c75d151ac87577e7a2ea2f863bfe551061`, with 25 paths, changes SHA-256
`363909f8828ee2023903c9ad1b8e7f78a0e73fbc9c6717756d448bf1a323bab7`, and paths SHA-256
`9b2f39deac0d18aaae940fe2995f523c2b5da798ed4ceb8366d6fc6333ae32d8`. It confirmed the final
`T014 | PASS` table row, `[x] T014` card, document links, current/target separation,
source/API/PDF identities, five lanes and unobserved boundaries. B9 is closed; no compile-link,
runtime, or sanitizer was required for this document-only batch.

## Unobserved boundaries

This evidence does not claim native runtime behavior, Python subinterpreter safety, wheel
packaging, wrapper sanitizer coverage, Spec184 external model breadth, or SIF/Tiger qualification.
Those boundaries remain owned by their active Spec/task evidence.
