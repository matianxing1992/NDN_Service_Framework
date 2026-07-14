# Spec 110 experiment-ready image contract convergence

**State**: implementation in progress; no cloud build or Slurm submission was
authorized by this convergence task.

## Superseded pre-start candidate

The Phase 18 candidate bound to source
`01f730d122a4408737443d75a65dc0d5ff99af5b` and Foundation digest
`sha256:71711601004a6f032fda032037691409324d2adb867efcd94be48b2d879227aa`
is `PRE_START_SUPERSEDED`. No candidate source tag, release tag, GitHub workflow
dispatch, GHCR runtime publication, or iTiger job was created for T181-T183.
The Foundation remains valid historical build evidence; it is not a valid
Foundation input for source changed after this audit because `Dockerfile.gpu`
checks its embedded source revision fail closed.

## Controlling audit findings

1. The official `Dockerfile.gpu` did not install the three Qwen experiment
   entrypoints referenced by the Slurm route.
2. An unused Qwen overlay Dockerfile installed a conflicting second Python
   lock, so it could not safely repair the official image.
3. `ndnsf-qwen.sbatch.in` bypassed `run-container.sh` and exposed the entire
   project root writable inside the allocation.
4. The canonical runner did not provide an explicit read-only artifacts mount.

These findings blocked a new image build. The repair makes the official GPU
Dockerfile the sole final recipe, keeps `gpu.lock` as the sole dependency
contract, installs all experiment entrypoints in `/opt/ndnsf/bin`, and routes
Qwen execution through explicit least-privilege mounts.

## RED evidence

`python3 tests/container/itiger-qwen-live/unit/test_github_sealed_workflow.py`
ran 18 tests and failed exactly two new contract tests before implementation:

- official GPU image packages Qwen entrypoints without an overlay lock;
- Qwen Slurm uses the canonical least-privilege runner.

After implementation the same focused suite passed 18/18. The release pipeline
integration test also reached `RELEASE_PIPELINE_PASS` and observed
`/artifacts:ro` with no writable whole-project bind. Full offline, secret,
sealed-source, and image-build evidence is appended only after those gates run.

## Post-implementation gate

- Spec 110 offline suite: 103/103 PASS, zero failures/errors/skips;
- container integration scripts: 5/5 PASS, including release materialization,
  runtime compatibility, rootless fallback, network scripts, and packaged
  security contract;
- official build-context secret scan: 107 files, 373,011 bytes, zero findings;
- GPU build preflight: PASS;
- workflow YAML parse: PASS;
- changed container/Slurm shell syntax: PASS;
- CodeGraph synchronization: already up to date;
- strict Spec Kit structural audit: PASS, 37/37 requirements traced and 190
  tasks parsed;
- `git diff --check`: PASS.

An intentionally broader full-repository secret scan was also run and failed
with 42 findings in historical experiment keys, hydrated `RELEASE/` payloads,
token implementation syntax, and scanner negative fixtures. This is not hidden
or counted as a build-context pass. Those paths are outside the official
workflow scan root and are excluded by the OCI `.dockerignore`; the immutable
seal/build-context scan remains the controlling publication gate. Similarly, a
naive `bash -n` over every tracked `*.sh` found the existing
`Experiments/start_ucla.sh`, whose content is MiniNDN Python despite its suffix.
The four changed container/Slurm scripts and templates passed `bash -n`.
