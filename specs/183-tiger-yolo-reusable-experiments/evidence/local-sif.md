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

The external application has now compiled in this exact SDK (150 Waf tasks,
26m40.982s, maximum -j2; `app-build-2.log` ends BUILT). No MiniNDN,
exact-composition local YOLO or Tiger NDNSF-DI qualification is claimed by this
base receipt. T011 remains unchecked. Next: inspect the complete native/Python
runtime path and produce the registered source/composition-bound gate receipts,
then complete the production wiring and the registered runtime gates.

## Application build completed; composition qualification pending

The first application attempt rejected `APP_BASE_DIGEST` before compilation,
despite the earlier agreeing reads of this new image. The buffered-read problem
also affects newly built SIFs; do not call the host permanently repaired.
Direct-copy the new SIF to `/dev/shm/spec183-sdk-d4031191/base-runtime.sif` and
check it against the same d4031191 digest. The second attempt passed the digest,
base source/ABI checks and configure, and compiled the external targets with
`-j2`. Logs: `app-build-1.log` (rejection) and `app-build-2.log` (successful build).
Keep the owned RAM source until the build and exact-composition checks finish.

The frozen app is `.cache/layered-base-20260908/app-81e330ea`: 157 files,
manifest SHA256 `e8a6266e0fe6cc3036c848df8f9e9e37aa9023276739182adea9534bfc4b4f11`.
All three ELF dependency listings resolve in the exact base, and the real YOLO
User `--help` exits 0 through `/app:ro`; logs are `preflight/app-runtime-preflight.log`
and `preflight/app-user-help.log`. The combined native probe exits 2 because
Provider rejects `--help`; Controller also interprets it as normal startup and
exits 1 without NFD. These are not successful service/inference tests.
The real MiniNDN runner `--help` and imports of installed `ndnsf`, installed
`py_repoclient`, and external `ndnsf_distributed_inference` also exit0 in the
same composition (`preflight/app-minindn-help.log`); this verifies entrypoint
loading, not execution of any MiniNDN scenario.
`preflight/app-content-final.log` revalidates all bytes against the base identity.
Publication now normalizes binaries to 0555 and other files to 0444; the already
built candidate received the same mode-only normalization without recompilation.

Layered profile, readonly mount and launch component checks: 33 passed (45
deselected); profile/plane closure checks: 38 passed; final transport inventory
checks: 11 passed (`layered-transport-components-r3.xml`). Application payloads
are enumerated transitively and tampering rejects transport. None qualifies
MiniNDN or the four-Provider NDNSF-DI GPU experiment.

## Same-source repackaging and actual MiniNDN import boundary

The runner lazily imports two helpers absent from the first app package:
`NDNSF_DI_Yolo2x2_Minindn.py` and `NDNSF_NewAPI_Minindn_Perf.py`. They were already
in the sealed source. The builder now includes them and supports verified
same-source `--reuse-application` without recompiling. Actual output:
`.cache/layered-base-20260908/app-81e330ea-r2`,159files, manifest SHA256
`65264ff70e725e2061b56957ca3cbf60e0ad4a2384f1ae0336d891883447500b`.
All three binary manifest rows equal the prior build. `app-repackage-r2.log`
reports `compiled:false`. Six tests reject changes to source, revision, base,
binary bytes or flags and verify preserved compiler provenance.

Full helper import failed inside the base and the old host `minindn-venv`
because Mininet is absent. The intended host-orchestrated path succeeds with
system Python3.8 (`preflight/app-host-minindn-import.log`); the container supplies
the application ABI, not the host namespace orchestrator. This confirms the
environment choice without another image build. The generic driver's legacy
SIF command paths and Tiger wrapper still need explicit layered wiring before
a real scenario; this is not a MiniNDN case PASS.

`runtime/application.py` now verifies the producer's application manifest before
publication. Four component checks pass: candidate-only scope, wrong base,
changed binary, and declared foundational-library shadowing. An initial test
collection import-path error is retained separately; the final/strict XMLs
contain four passing checks. These content fixtures contain no real ELF or
runtime proof. Profile/dispatch integration of the application layer is pending.
