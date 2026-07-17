# Allocation topology and security offline gate

Date: 2026-07-13

Scope: T050-T058. T059 remains open because the restricted execution namespace
cannot obtain the root network-namespace authority required by MiniNDN.

## Verified behavior

- frozen single-node and two-node process maps;
- independently selected TCP and UDP candidates;
- exactly one job-scoped NFD per unique node;
- one controller, one user, and three providers with distinct read-only identities;
- distinct provider GPU UUIDs and node-local GPU slots;
- ordered readiness dependencies and command-digest binding;
- selected-transport admissibility independent of diagnostic transport failure;
- idempotent face/route configuration;
- shell-injection, duplicate identity/NFD/GPU, partial readiness, closed selected
  port, signal-policy, and zero-survivor rejection;
- normal supervisor completion and TERM preservation as exit code 143;
- six packaged permission/NAC-ABE/token/replay/selection regression entrypoints
  bound by digest to the frozen process-map contract.

## Commands and results

```text
python3 tools/ndnsf-di/run_spec110_offline_tests.py \
  --output results/spec110-itiger-qwen-live/minindn-packaged/offline-tests.junit.xml
  SPEC110_OFFLINE tests=70 failures=0 errors=0 skipped=0 duration=0.673005s

tests/container/itiger-qwen-live/integration/test_network_scripts.sh
  NETWORK_SCRIPT_PASS

tests/container/itiger-qwen-live/integration/test_packaged_security_contract.sh
  SECURITY_EXECUTION_NOT_ARMED
  PACKAGED_SECURITY_CONTRACT_PASS
```

JUnit SHA-256:
`9c56740f195bb15a383f77f5a9db74e35e433c07738c951abe72538a1d1dbdfd`.

The first line emitted by the security contract negative test is expected: it
proves execute mode cannot start unless explicitly armed. Contract validation
then passes without executing an unowned host NFD.

## T059 blocker

`results/spec110-itiger-qwen-live/minindn-packaged/prestart-blocker.json`
retains the exact `sudo -n true` failure. This is not candidate execution and
does not close T059. The next admissible action is the exact packaged MiniNDN
run in a real root-capable shell, with its original outcome retained.
