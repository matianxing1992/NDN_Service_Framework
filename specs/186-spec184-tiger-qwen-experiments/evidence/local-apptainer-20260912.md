# Spec186 local Apptainer 1.5.3 switch

**Captured:** 2026-09-12 (America/Chicago)
**Host:** local experiment host (`x86_64`, Ubuntu 20.04)
**Branch:** `SPEC184Experiments`

## Decision and boundary

The local experiment host now uses Apptainer 1.5.3 as its only installed
Apptainer runtime. The previous root-owned `/usr/local/bin/apptainer` 1.3.4
installation was replaced; no 1.3.4 fallback is kept in the local executable,
starter, offset-preload, CNI, or configuration paths. Tiger login-node
Apptainer is not used to build or execute SIF images. Tiger jobs execute on
allocated compute nodes, which were independently observed at
`1.5.3-1.el9`.

## Build and installation

| Item | Observation | Status |
| --- | --- | --- |
| Source | Apptainer v1.5.3, commit `6a76317a87c4eae10f52c7805965756a919291d7` in `/tmp/spec186-apptainer-v1.5.3` | PASS |
| Configure | `./mconfig -b builddir --prefix=/usr/local --without-suid` | PASS |
| Main binary | `/usr/local/bin/apptainer`, SHA-256 `96431b1ebb7ddd971b73a41b8975289e47e1f5d04e6b3f269c4f5d398f66f575` | PASS |
| Starter | `/usr/local/libexec/apptainer/bin/starter`, SHA-256 `243d93a217931c7b2143e6954d4ec71c348a357f5d0cf06632c9e5bd0ad2245d` | PASS |
| Offset preload | `/usr/local/libexec/apptainer/lib/offsetpreload.so`, SHA-256 `3afb7d0a07d69e4e3d65833fbf71e607aae1cfc77229861e666d5e17fc140667` | PASS |
| CNI | 16 compiled plugins installed under `/usr/local/libexec/apptainer/cni/` | PASS |
| Configuration | `/usr/local/etc/apptainer/apptainer.conf` generated for the `/usr/local` prefix | PASS |
| SUID | disabled; rootless user namespaces are enabled on this host | PASS |

The complete parallel Make target was bounded at 300 seconds. It generated
the 1.5.3 main binary, then stopped while generating bash completion. The
runtime targets were completed separately with `-j4`; this auxiliary timeout
does not invalidate the installed runtime and is retained in
`docs/failure-log.md`.

## Post-switch probes

```text
$ command -v apptainer
/usr/local/bin/apptainer
$ /usr/local/bin/apptainer --version
apptainer version 1.5.3
$ /usr/local/bin/apptainer version
1.5.3
$ /usr/local/bin/apptainer help
--config ... (default "/usr/local/etc/apptainer/apptainer.conf")
```

The active PATH therefore resolves every unqualified `apptainer` invocation
used by the local NDNSF-DI and Tiger preparation scripts to 1.5.3. This host
still has no GPU, Slurm, or MiniNDN runtime, so this switch is a local runtime
precondition and is not a Spec186 protocol qualification result.

## Next gate

Rebuild the exact Spec186 base SIF and read-only application bundle with this
1.5.3 runtime, then run the native ABI/import/entrypoint closure before any
Tiger submission. The old Spec183/174 SIF remains ineligible because its
source seal and candidate identity do not match Spec186.
