# Small C++ NDN example in the existing SIF

This diagnostic sends three Interests from a C++ consumer on rank 1 to a C++
producer on rank 0 through two NFDs. Data names, run-token contents and SHA-256
digest signatures must match. It checks transport/configuration, not producer
authentication, NDNSF permissions, YOLO, CUDA or image release qualification.

The same locally built binary is mounted read-only at `/probe/ndn-smoke` in
each SIF execution. It links to the image's `/opt/ndnsf-di/current/lib` libraries.
No cluster-side compiler or complete image rebuild is needed for this probe.

Build using the host's matching ndn-cxx headers and system toolchain:

```bash
bash Experiments/TigerCluster/tools/build-ndn-smoke.sh /absolute/new/build-dir
```

Copy the resulting binary and these maintained files into a fresh run directory:

```text
run.sbatch                  <- jobs/ndn-smoke/run.sbatch
manifest.json
bundle/ndn-smoke             <- build output
bundle/apps/ndn-smoke.cpp    <- apps/ndn-smoke.cpp
bundle/tools/ndn_smoke.py    <- tools/ndn_smoke.py
bundle/runtime/baseline.py  <- runtime/baseline.py
```

Manifest fields are explicit; use the allocated-host paths for cluster execution:

```json
{
  "scope": "NDN_TRANSPORT_DIAGNOSTIC",
  "local": false,
  "verifySif": true,
  "sif": "/project/ACCOUNT/path/runtime.sif",
  "sifSha256": "REPLACE_WITH_PINNED_IMAGE_SHA256",
  "apptainer": "/usr/bin/apptainer",
  "bundle": "/project/ACCOUNT/path/new-run/bundle",
  "binarySha256": "REPLACE_WITH_BUILT_BINARY_SHA256",
  "prefix": "/ndn-smoke/UNIQUE-RUN",
  "token": "UNIQUE-RUN",
  "port": 29680,
  "files": {
    "ndn-smoke": "REPLACE_WITH_BUILT_BINARY_SHA256",
    "apps/ndn-smoke.cpp": "REPLACE_WITH_SOURCE_SHA256",
    "tools/ndn_smoke.py": "REPLACE_WITH_SCRIPT_SHA256",
    "runtime/baseline.py": "REPLACE_WITH_HELPER_SHA256"
  }
}
```

The prefix/token must change for a new run. `files` records the exact staged
bundle; both ranks verify it and the image before NFD startup. Retain compiler,
source/binary hashes and the binary's dynamic dependency report from the build.
Run shell syntax checks and the Python entrypoint's `--help` on the staged copy.

For a local rehearsal, use local absolute paths, `local: true`, and execute:

```bash
python3 /absolute/new-run/bundle/tools/ndn_smoke.py local /absolute/new-run/manifest.json
```

This creates two NFDs with distinct ports, sockets and HOME directories. Setting
`verifySif: false` is permitted only for this local runtime diagnostic; it cannot
establish image integrity. Cluster execution always requires hash verification.

For the two-node CPU allocation, after confirming account/partition:

```bash
sbatch --parsable --partition=bigTiger --account=devs \
  --output=/project/ACCOUNT/path/new-run/slurm-%j.out \
  --error=/project/ACCOUNT/path/new-run/slurm-%j.err \
  /project/ACCOUNT/path/new-run/run.sbatch \
  /project/ACCOUNT/path/new-run/manifest.json
```

The job requests two nodes, one CPU and 1 GiB per node, five minutes, and no GPU.
Only Slurm compute nodes execute NFD and the binary. Each node gets a private
scratch directory, `/node/nfd.sock`, keychain and NFD config. NFD CS is disabled;
TCP listens on the chosen high port. `nfdc` creates a permanent peer face and a
cost-10 route; the producer's local registration remains preferred.

Read `result.json`, both `rank*/result.json`, producer/consumer logs, routes,
faces, ldd and Slurm exit state together. Three matching packets, clean logs,
zero exits, distinct allocated hosts, equal pinned image hashes, reaped children
and removed scratch establish this diagnostic's PASS. A contradictory FAIL line
invalidates a printed PASS. Outputs are exclusive; preserve a failed run and use
a new directory only after identifying and correcting its cause.

Observed run and limitations: [Spec183 evidence](../../../specs/183-tiger-yolo-reusable-experiments/evidence/cpp-ndn-smoke.md).
