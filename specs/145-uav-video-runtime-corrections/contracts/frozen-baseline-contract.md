# Frozen Baseline Contract

Recorded 2026-07-24 before Spec 145 source implementation.

The digest is:

```text
rg --files <directory> | sort | xargs sha256sum | sha256sum
```

| Frozen directory | Directory-root SHA-256 |
|---|---|
| `specs/125-adaptive-sample-atomic-prefetch` | `08fbc07481e4d2565e99dad9e8ea2259c811e1f1abe973fa55bfdfcf06f409c9` |
| `specs/126-loss-reorder-resilience` | `766969e6e8dfffff86732ff84ab7567c15bc0cbe0541bf2b6d8648d7ed98c493` |
| `results/spec125-adaptive-sample-atomic-20260719-confirm06` | `80dbfbd02f5f7aeaaeb5a6c7556c82c0429d5dc43109c9a72b613655b28c6c0d` |
| `results/spec126-loss-reorder-20260720-confirmation07` | `aab1ff1c340f8acb1858fbf8ee3c44a274c0e32581a144e0786c50a297e8412a` |

Rules:

- Do not execute any historical runner.
- Do not copy fresh output into a historical result.
- Do not reinterpret a frozen failure.
- Recompute all four digests at T001 and T005.
- Any mismatch is a blocking integrity failure requiring investigation before
  implementation or reference promotion.

