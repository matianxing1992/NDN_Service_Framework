# Spec183 Local Base SIF Checkpoint

2026-09-08: **BASE_LIBRARIES_ONLY PASS; complete base+application qualification PENDING**.

| Item | Verified observation |
| --- | --- |
| Base/SDK image | `Experiments/TigerCluster/.cache/layered-base-20260908/base-runtime.sif` |
| Size | 3,900,682,240 bytes |
| Buffered and direct-I/O SHA256 | `d4031191aed0e9aaa105032ffa4f5fa289b38044da29d06e261db701430f1dc6` in both independent reads |
| Build owner | `adapters/slurm-apptainer/templates/library-runtime.def.in` and `scripts/build-base-libraries.sh` |
| Local executable | `/opt/apptainer/1.5.3/bin/apptainer` |
| Native artifacts | NAC, NDN-SVS, NDNSD, NDNSF, `ndnsf._ndnsf`, `py_repoclient._py_repoclient` compiled in the container |
| Build and pre-pack check | Both print `PASS / BASE_LIBRARIES_ONLY` |
| Independent finished-SIF check | Same verifier executed through Apptainer against the finished image, exit 0 |
| Application boundary | DI package and replay absent; Provider/fault-Provider/Controller application binaries absent; stable NDN/OpenABE tools remain |
| Cleanup | Exited build's `/tmp/build-temp-703876096`, `/tmp/bundle-temp-638063888`, and owned tmpfs source removed; approximately 9.9 GiB disk available afterward |

Raw evidence is under `Experiments/TigerCluster/results/yolo-layered-20260908/`:
`base-build-3.log`, `preflight/base-final-verify.log`, `preflight/base-runtime.json`,
`preflight/base-final-buffered-sha256.txt`, `preflight/base-final-direct-sha256.txt`.
The previous Python development-package and extraction failures are retained in
the first two logs and documented in `input-read-integrity.md` and the failure log.

No external application has yet been compiled in this new SDK. No MiniNDN,
exact-composition local YOLO or Tiger NDNSF-DI qualification is claimed by this
base receipt. T011 remains unchecked. Next: build the already sealed app source
with this exact base, inspect its native/Python closure and real entrypoints,
then complete the production wiring and the registered runtime gates.
