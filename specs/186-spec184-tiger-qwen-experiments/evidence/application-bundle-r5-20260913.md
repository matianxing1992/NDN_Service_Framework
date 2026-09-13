# Spec186 application bundle r5 receipt

This receipt records the candidate refresh after the NAC-ABE ABI repair. The
previous r4 bundle remains immutable and is not reused for runtime evidence.

| Field | Value |
| --- | --- |
| source commit | `575b43cc93bbed29932303caf3d09974f1585af7` |
| provider repair commit | `4751148375dad9149c7c185c9381d5734c733e13` |
| build tree | `build-spec186-r5` |
| bundle | `.codex-tmp/spec186-app-bundle-r5` |
| files | 9 |
| bytes | 69,116,035 |
| bundle/tree SHA-256 | `687610de859155449c51ec2ba4bb7b57c77614cbf0a53f106bb65152f8c07129` |
| Tiger path | `/project/tma1/ndnsf-di/apps/spec186/spec186-app-bundle-r5` |
| Tiger tree SHA-256 | `687610de859155449c51ec2ba4bb7b57c77614cbf0a53f106bb65152f8c07129` |
| Tiger permissions | all nine files read-only after staging |

The ELF files in r5 are stripped copies of the verified current build outputs;
dynamic symbols and runtime dependencies are retained. The Python extension
hash is `05ed17c3def34416f9c54fee07a9408db1ac053e5f8334beb2f98a1b06d818db`.
The r5 manifest recomputation and remote tree recomputation match exactly.

## Runtime version boundary

The local experiment host resolves `apptainer` to `/usr/local/bin/apptainer`
and reports `apptainer version 1.5.3`; no local 1.3.4 executable remains. The
Tiger compute preflight independently reports `1.5.3-1.el9`. The login-node
1.3.4 package is an SSH/Slurm metadata boundary only and is not used to build
or execute SIF images.

## Qualification boundary

The exact source-sealed base SIF is still absent, so all eight regenerated
pre-dispatch checks fail closed before SSH/rsync/staging/Slurm side effects.
This r5 receipt proves application identity and staging only; it does not
promote T006.c or any MiniNDN/Tiger runtime task.
