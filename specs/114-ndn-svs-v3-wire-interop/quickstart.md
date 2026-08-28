# Quickstart: Validate NDN-SVS V3 Wire Compatibility

This is the expected runnable validation flow after implementation. It does not
authorize remote mutation or claim current code already passes.

## 1. Safety and identity preflight

```bash
cd /home/tianxing/NDN/ndn-svs
test "$(git branch --show-current)" = Experimental
git status --short
git merge-base --is-ancestor c34c04d766836bba1567a70bae846dfbd9d25b66 HEAD

cd /home/tianxing/NDN/ndn-service-framework
python3 Experiments/spec114_candidate_manifest.py create \
  --ndn-svs /home/tianxing/NDN/ndn-svs \
  --ndnts-lock /home/tianxing/NDN/ndn-svs/tests/interop/ndnts/package-lock.json \
  --campaign-attempt formal-01 \
  --output-root results/spec114-svs-v3
```

Expected: a new immutable candidate manifest; no existing candidate/cell is
overwritten.

## 2. Build and fixed-vector tests

```bash
cd /home/tianxing/NDN/ndn-svs
./waf configure --with-tests
./waf -j"$(nproc)"

build/unit-tests \
  --run_test=TestV3Wire,TestVersionVector,TestCore,TestSecurityOptions \
  --log_level=test_suite
```

Expected:

- V3 packet name and embedded Data bytes match fixed fixtures;
- V2 vectors remain unchanged;
- sequence zero, future bootstrap, wrong name/signature, malformed content, and
  serial/parallel negative cases reject atomically;
- the focused test command reports no error.

## 3. Full NDN-SVS suite

```bash
cd /home/tianxing/NDN/ndn-svs
build/unit-tests --log_level=test_suite
```

Expected: 100% of the rebuilt suite passes from the current committed sources.

## 4. Install independent peer dependencies

```bash
cd /home/tianxing/NDN/ndn-svs/tests/interop/ndnts
npm ci --ignore-scripts
npm ls --all
```

Expected: lockfile-exact installation. Do not commit `node_modules/` and do not
float package versions.

## 5. Standalone bidirectional interop smoke

```bash
cd /home/tianxing/NDN/ndn-svs
tests/interop/run-svs3-interop.sh \
  --cpp build/tests/interop/svs3-peer \
  --ndnts tests/interop/ndnts/svs3-peer.mjs \
  --sync-prefix /ndn/spec114/smoke \
  --publish-count 5
```

Expected: C++→NDNts and NDNts→C++ both report five updates, concurrent publish
converges, V2 regression passes, and mixed V2/V3 produces no cross-state.
This host-NFD run is a smoke only and must be cleaned up afterward.

## 6. Formal MiniNDN matrix

```bash
cd /home/tianxing/NDN/ndn-service-framework
sudo -n python3 Experiments/NDN_SVS_V3_Interop_Minindn.py \
  --candidate-manifest results/spec114-svs-v3/<candidate>/candidate-manifest.json \
  --matrix formal \
  --publish-count 20 \
  --convergence-timeout-s 60
```

Expected: three 0% and three 5% unique cells, each executed once. The summary
must report 120/120 unique remote sequence numbers covered for 0% (callback
ranges may batch them), convergence for every 5% run, no duplicate sequence
coverage, no restart, zero Sync Ack Data, identical final vectors, and matching
source/config identities.

Do not rerun or tune a failed cell under the same candidate. Fixes require a new
candidate and complete six-cell matrix.

## 7. Reinstall and rebuild affected NDNSF consumers

```bash
cd /home/tianxing/NDN/ndn-svs
sudo -n ./waf install
sudo -n ldconfig

cd /home/tianxing/NDN/ndn-service-framework
./waf -j"$(nproc)"

python3 -m pytest -q \
  tests/python/test_spec112_segmented_response.py \
  tests/python/test_spec112_targeted_timeout.py \
  tests/python/test_spec112_candidate_manifest.py \
  tests/python/test_ndnsf_di_tk_widgets.py
```

Run the focused C++ Targeted and provider-liveness tests named in Spec 113's
integration evidence, then verify installed header/library hashes still match
the candidate. The focused configuration assertions must also prove that an
unset `NDNSF_SVS_MAX_SUPPRESSION_MS` leaves V3 at 200 ms, explicit `1` remains
an observable override, explicit V2 selects only the V2 profile, and an invalid
protocol value fails startup.

## 8. Audit and convergence

```bash
cd /home/tianxing/NDN/ndn-service-framework
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
```

Expected: zero structural error, zero unchecked task, full traceability, final
code-aware audit PASS, and convergence appends no remaining work.
