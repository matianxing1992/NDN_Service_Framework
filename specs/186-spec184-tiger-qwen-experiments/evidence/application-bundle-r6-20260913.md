# Spec186 application bundle r6 receipt

This receipt supersedes r5 for new candidate work. r5 remains on the local
experiment host as historical evidence, but its remote staging was removed
after the source repair invalidated its identity. The r6 bundle contains the
native collaboration-grant repair built from the current checkpoint.

| Field | Value |
| --- | --- |
| source commit | `6d143d3f0f7a7c627af2c1ef6810d79c0738b52d` |
| source seal file SHA-256 | `f4676a0f937c903caebc8374893171890d0d6d0639be42a3ced1b101d7512a00` |
| build output | `build/` (packaging label `build-spec186-r6`) |
| bundle | `.codex-tmp/spec186-app-bundle-r6` |
| files | 9 |
| bytes | 69,126,686 |
| bundle/tree SHA-256 | `04c2dd64b4f070cbd909a87f75a0372a0e3d4dae45c7e712641369cf76531d73` |
| Tiger path | `/project/tma1/ndnsf-di/apps/spec186/spec186-app-bundle-r6` |
| Tiger tree SHA-256 | `04c2dd64b4f070cbd909a87f75a0372a0e3d4dae45c7e712641369cf76531d73` |
| Tiger permissions | all nine files read-only after staging |

The native outputs were rebuilt with the repository `-j4` ceiling and then
stripped only of debug sections. The provider, requester, authority, ONNX
assembly worker, native extension and DI library hashes are recorded in
`bundle-manifest.json`; its independent tree recomputation matches both local
and Tiger copies. The native identity manifest reports
`SPEC180_NATIVE_IDENTITY_OK` before packaging.

## Runtime version boundary

The local experiment host resolves `apptainer` to `/usr/local/bin/apptainer`
and reports `apptainer version 1.5.3`; no local 1.3.4 executable remains. The
Tiger compute preflight independently reports `1.5.3-1.el9`. The login node is
used only for SSH, Slurm and file metadata; it does not build or execute SIF
images.

## Qualification boundary

The exact source-sealed base SIF is still absent, so this r6 identity has not
been promoted to a SIF, MiniNDN YOLO, Qwen3 or Tiger qualification result. The
remote application staging is complete; the next gate is a matching 1.5.3
base SIF and composition receipt.
