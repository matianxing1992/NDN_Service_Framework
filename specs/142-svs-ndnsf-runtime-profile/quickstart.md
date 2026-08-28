# Quickstart: Spec 142

This is a staged, once-only campaign. Commands below are implementation
contracts; do not start formal cells until the pre-implementation audit and
build/profile gates pass.

## 1. Verify the build and runtime identity

```bash
python3 Experiments/build_svs_ndnsf_profile_worker.py build
python3 Experiments/build_svs_ndnsf_profile_worker.py verify
taskset -c 0-3 python3 \
  Experiments/NDN_SVS_NDNSF_Profile_Worker_Minindn.py \
  --stage preflight
```

Required output:

- one content-addressed manifest;
- V3 on both peers;
- effective piggyback limit 800 bytes on both peers;
- identical mode-independent profiles;
- exact binary and runtime library hashes;
- two 800 pps pacer-only checks within +/-2%.

## 2. Run the 400 pps qualification pair

```bash
sudo -n -E taskset -c 0-3 python3 \
  Experiments/NDN_SVS_NDNSF_Profile_Worker_Minindn.py \
  --stage qualification \
  --campaign results/spec142-svs-ndnsf-runtime-profile/<campaign-id>
```

Do not manually rerun a failed or invalid cell. Inspect the two terminal
receipts and the generated qualification verdict.

## 3. Run 600/800 only after qualification passes

```bash
sudo -n -E taskset -c 0-3 python3 \
  Experiments/NDN_SVS_NDNSF_Profile_Worker_Minindn.py \
  --stage formal \
  --campaign results/spec142-svs-ndnsf-runtime-profile/<campaign-id>
```

The runner must refuse this command unless the same campaign contains a passed
400 pps qualification verdict and the manifest hash is unchanged.

## 4. Analyze and freeze

```bash
python3 Experiments/analyze_svs_ndnsf_profile_worker.py \
  results/spec142-svs-ndnsf-runtime-profile/<campaign-id> \
  --json-output specs/142-svs-ndnsf-runtime-profile/evidence/final-analysis.json \
  --report-output specs/142-svs-ndnsf-runtime-profile/evidence/final-report.md
```

The analyzer emits a validity table before any performance table. Only
rate-matched pairs with two `PROFILE_VALID` receipts are compared.
