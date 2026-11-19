# Bidirectional NDN-SVS PubSub benchmark

This external driver is compiled twice, once against each immutable NDN-SVS
worktree. The pinned baseline calls `publish()` synchronously. The pinned
treatment calls `publishAsync()` and enables four-worker receive processing.
Both peers publish and receive against their subject's NDN-SVS library.

Use a disposable NDN-SVS Git clone containing both commits pinned by the
builder. Use a compatible system SDK with Boost 1.71, ndn-cxx, OpenSSL and
pkg-config, with its compiler and linker on `PATH`. From the NDNSF root,
select owned absolute paths and run:

```sh
python3 Experiments/build_svs_pubsub_commit_bench.py \
  --svs-source "$svs_clone" --output "$subject_root"
python3 Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py plan \
  --build-authority "$subject_root/subjects.json" --campaign-id paired \
  --output "$campaign_root"
python3 Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py seal "$campaign_root"
```

Run `run-block` against the sealed campaign with `--subject`
`sync-publish-no-internal-parallelism` before `async-publish-parallel-sync`.
The network step requires MiniNDN, privileges for its private namespaces,
NFD, `nfdc`, `ndndump`, `infoconv`, `tc` and four available CPUs. For a short
check use `smoke --subject SUBJECT --build-authority MANIFEST --output NEW_DIR`.

Run `Experiments/analyze_svs_pubsub_commit_latency.py CAMPAIGN` to write the
campaign summary. Each direction links a sender's API return to the matching
remote delivery and reports missing, invalid, duplicate and reordered data.
The historical one-way campaign format remains readable by the analyzer.

The synchronous stage-profiler uses the same SDK. Rebuild it in a disposable
clone with the pinned upstream commit using
`python3 Experiments/build_svs_sync_stage_profile.py rebuild-frozen --svs-source SVS_CLONE --output NEW_DIR`.
This restores the archived instrumentation patch, verifies the resulting Git
tree and runs the driver self-tests. Give `NEW_DIR/subject-manifest-io.json` to
the stage runner's `overhead-preflight` and `plan` commands with
`--subject-manifest`. Set `NDNSF_SVS_STAGE_PROFILE_MANIFEST` to that file to
select the native profiler cases in `tests/python/test_spec133_svs_sync_stage_profile.py`.

For serial offload, run `Experiments/build_svs_serial_production_offload.py`
with `--base` set to the pinned commit from the builder, an explicit
`--svs-source`, `--boost-root` and a new `--output` directory. Use its
`source-manifest.json` with `NDN_SVS_Serial_Production_Offload_Minindn.py
--preflight --subject-manifest FILE --campaign DIR`. The later worker-necessity
and fixed-worker campaigns require their exact archived binary and predecessor
evidence. Select them with `--subject-manifest` and `--archive-root` in those
runners' first phase. To verify the archived binary via Python tests, set
`NDNSF_SVS_WORKER_SUBJECT_MANIFEST` and `NDNSF_SVS_WORKER_ARCHIVE_ROOT`.
