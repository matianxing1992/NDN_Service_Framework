# Quickstart: Planned Spec 136 Execution

These commands are the intended future workflow. They have **not** been run
while defining Spec 136.

```bash
# 1. Source, security, ordering, and analyzer contracts
python3 -m unittest tests.python.test_spec136_svs_rsa_ordered_offload

# 2. Create and verify both isolated builds
python3 Experiments/build_svs_rsa_ordered_offload.py build
python3 Experiments/build_svs_rsa_ordered_offload.py verify

# 3. Run nonformal security/order/route/pacer admission checks
sudo -n -E taskset -c 0-3 \
  python3 Experiments/NDN_SVS_RSA_Ordered_Offload_Minindn.py preflight

# 4. Seal, review, and run the ten-cell manifest exactly once
python3 Experiments/NDN_SVS_RSA_Ordered_Offload_Minindn.py manifest
sudo -n -E taskset -c 0-3 \
  python3 Experiments/NDN_SVS_RSA_Ordered_Offload_Minindn.py run \
  --manifest build/spec136/campaign-manifest.json

# 5. Analyze all terminal receipts
python3 Experiments/analyze_svs_rsa_ordered_offload.py \
  results/spec136-rsa-sign-verify-offload/<campaign-id>
```

Before step 4, display the exact campaign ID, ten cells, subject hashes,
security policy hashes, and protected Spec 135 snapshot. A failed formal cell
must not be rerun.
