# Spec 168 v35 Replacement Candidate Audit

## Verdict

**PASS Gate D; authorize one linked replacement Gate E campaign.** The closed
v34 campaign is not retried. The replacement is necessary because job 182387
proved that v34's source bundle omitted a transitive policy-builder helper and
failed before any Request.

## Frozen replacement

- Campaign: `spec168-campaign-v3-695ce1a81c32f9843ec6`.
- Source: `sha256:f7c138b7dce961f6e22de18c1aa98fee04d78806f145de5cf354ecf45c829e9c`.
- Source bundle: `sha256:c5a0326c23be2c1e378dddf985215527f1c1dff2842885413178d324afd4482d`.
- Gate C job/result: 182388,
  `sha256:8dffb7620eb81fc5aa0e59288fd6dada702bf508721d501b9f9b47695f6e2dc5`.
- SIF/stage/route/analyzer/schedule/prompt/strategy identities remain exactly
  those accepted by the v34 audit.

## Repair boundary and new evidence

- Added only `prepare-qwen36.py` and `register-qwen36-repo.py` to the source
  bundle. No NDNSF-DI, Repository, model, routing, analyzer, inference, or
  schedule behavior changed.
- The bundle builder now scans every packaged Python/shell/sbatch file for
  direct `/source/...` references and
  `Path(__file__).with_name(...)` dependencies. Missing closure is a build
  error. The focused negative test proves the v34 omission is rejected.
- The complete source bundle imports in the existing exact parent Docker image
  under 2 GiB memory, and `build-generation-policy.py` executes successfully
  against a retained real Qwen policy fixture.
- Gate C job 182388 completed `0:0` in 1:51 with the 165-file closure, preserved
  native extensions, all three CUDA stages, top token 8065, zero CPU fallback,
  and clean overlay removal.
- Spec Kit structure/prerequisites pass. Sixty-nine focused contracts pass;
  source/campaign validation, no fixed 300-second settle, shell syntax, and
  diff whitespace gates pass.

## Authorized action

Submit campaign `spec168-campaign-v3-695ce1a81c32f9843ec6` once with request ID
`spec168-695ce1a81c32f9843ec6-single`. Preserve any result and never retry this
campaign identity automatically.

## Five-tool gate report

- Context Mode: stable/active checks passed earlier; repository is authority.
- CodeGraph: used before implementation and synchronized.
- Spec Kit: strict structure, prerequisites, campaign, and replacement audit
  pass.
- GSD: persistent deployment-fidelity goal remains active.
- ARS: not applicable; this is implementation admission, not a research claim.
