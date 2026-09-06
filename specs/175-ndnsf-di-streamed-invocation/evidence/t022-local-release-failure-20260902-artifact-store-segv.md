# T022 local-release failure — Artifact STORE segfault (2026-09-02)

## Scope

This is one fresh host/CPU M01 production-path attempt after the wrapped-key
cold-start repair. It is retained as negative evidence for the current source
subject and does not authorize an SIF build or Tiger submission.

## Observed boundary

- The real four-Provider MiniNDN topology started and tore down cleanly.
- The repository STATUS route completed ACK, Selection, Response, and response
  decryption.
- The subsequent `/NDNSF/DistributedRepo/Artifact/v2/STORE` request reached the
  repository Provider and received an ACK.
- The User-side publisher then emitted a duplicate STORE request; the Provider
  rejected it as `duplicate-request-and-token`.
- During `spec175_repo_bootstrap.py:publish` → `publish_file` → `transfer` →
  `commit_ack_tasks` → `ServiceUser.commit_plan`, the User process terminated
  with `Fatal Python error: Segmentation fault` before repository publication
  completed.
- The run returned `FOCUSED_M01_RC=1`; no M01 result or SIF qualification was
  accepted.

## Evidence

Run root:
`results/spec175/focused-20260902-cold-key-r1/`

Relevant records:

- `spec175-repo-publisher.log`: STORE ACKs followed by the Python segfault;
- `spec175-repo.log`: Provider `REQUEST_RECEIVED`, `REQUEST_OBSERVED`,
  `ACK_PUBLISHED`, then replay rejection for the duplicate STORE request;
- `spec175-repo-route-probe.json`: STATUS route probe passed.

## Classification

The exact ownership of the duplicate/retry and native lifetime failure is not
yet proven. This is an application/NDNSF repository-transfer blocker, not a
SIF, CUDA, ONNX, or Tiger scheduler result. The next focused repair must
identify why `commit_plan` reissues the same request and why that path can
segfault, then add a regression before a new T020 seal.
