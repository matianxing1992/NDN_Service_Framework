# T036 route and terminal evidence — 2026-09-01

Status: focused implementation boundary **CLOSED**. This record does not
claim a complete MiniNDN matrix, SIF replay, or Tiger execution.

The current source tests cover the non-mutating live repository route probe,
same-identity ACK classification, M13 triplet handling, fresh case results,
registered terminal oracles, child reaping, idempotent shutdown, and
pre-network rejection. The focused command was:

```text
python3 -m pytest -q tests/python/test_spec175_real_minindn_gate.py \
  -k 'nfd_snapshot or nfdc_mutation or repo_fetches_start or repo_publisher_probes or repo_route_probe or expected_terminal_stream or normal_stream_owner or native_user_stop or pre_network_expected_rejection'
16 passed, 49 deselected
```

The full current real-MiniNDN gate file passed as part of the 94-test local
preflight/route/lifecycle/privacy run. A fresh T020 source seal and later G0--G4
execution remain required.
