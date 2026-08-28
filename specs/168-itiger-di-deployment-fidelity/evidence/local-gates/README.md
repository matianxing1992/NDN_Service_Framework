# T004 Local Deployment-Fidelity Gate

**Implementation status**: canonical Gate B is **PASS** under source identity
`sha256:213a425256e599c7b76b58b6b99dce8845f961e198a018e727d8561376bb9851`.
Exact-SIF Gate C remains pending, so T004 and remote admission remain open.

The retained v29 run used a content-addressed 75 MiB, seeded random-weight
three-stage Qwen3 architecture under Docker `--memory=6g --memory-swap=7g` on
the 8 GiB host. It exercised the real Qwen transformer loader and forward path,
three independent Repo and Provider processes, deferred ACK-driven planning,
two committed data dependencies, and one four-token generation invocation.
The User emitted `LLM_PIPELINE_GENERATION_CAMPAIGN_PASS`; the formal analyzer
accepted 20 lifecycle events, four tokens, and 78,254,966 unique Repo bytes in
125,787.885 ms. The kernel journal contains no cgroup OOM event for the run
interval. See `v29-canonical-gate-b.md`. The v28 result remains the measured
predecessor that exposed two analyzer false negatives.

## Why the predecessor was not rerun

The required prepared run
`results/spec165-local-gates/20260731T074249Z-1edd6ad0` remains present and its
6,014,071,792-byte Qwen stage bundle is still referenced from the shared
content-addressed store with zero duplicate payload bytes. Its prior Gate B/C
evidence is valuable, but its contract is not Spec 168 deployment fidelity:

- Provider stages read model artifacts through a shared-filesystem symlink;
  there is no independent NDNSF-DistributedRepo model-delivery process;
- the old launcher waits a fixed `provider_wait_s=10.0` before the User;
- the command enables `--test-only-allow-ephemeral-app-state`;
- eight invocations used 64 distributed token requests rather than one durable
  collaboration per invocation;
- it emits neither `ndnsf-di.lifecycle-event.v1` nor the Spec 168 runtime
  admission manifest;
- Gate C was Docker with `NDNSF_ALLOW_CPU_FALLBACK=1`, not the exact SIF.

These facts make the result a known preflight BLOCK. Re-executing six gigabytes
of unchanged model work could not change that contract and would violate the
no-duplicate-work rule.

## New gate

`spec168_local_gate.py` is a single-writer, no-auto-retry analyzer/launcher.
The two shell entrypoints run either real MiniNDN or the exact digest-checked
Apptainer SIF and require the runtime itself to emit evidence. Admission rejects:

- missing independent Controller/Repository/User/three-Provider PIDs;
- host NFD, simulated components, test identities, or security bypasses;
- non-Repo delivery, shared-filesystem Provider injection, or zero real bytes;
- fixed settle waits or non-event-driven readiness;
- mocked/non-Qwen adapters;
- more than one wire Request or any per-token Request;
- incomplete/multi-terminal/unauthenticated lifecycle evidence;
- undeclared/mismatched execution devices or a mismatched SIF digest. The
  bounded 8 GiB Gate B profile explicitly admits CPU execution as logic
  evidence; setting `NDNSF_SPEC168_REQUIRE_CUDA=1` makes non-CUDA evidence fail
  closed for Gate C/Tiger CUDA admission.

## Focused verification

```text
PYTHONPATH=.:NDNSF-DistributedInference \
  python3 tests/python/test_spec168_real_minindn_gate.py
Ran 9 tests — OK

bash -n run-real-minindn-gate.sh run-exact-container-gate.sh
python3 -m py_compile spec168_local_gate.py test_spec168_real_minindn_gate.py
```

The retained `gate-b-preflight-block.json` and `gate-c-preflight-block.json`
remain historical negative evidence for the superseded Spec 165 path. The v28
run is the first bounded Spec 168 product-path success; it also exposed two
post-run analyzer false negatives, now covered by unit tests: full-generation
transformer markers were classified as single-pass markers, and deferred model
preparation was incorrectly required to emit an eager-startup artifact marker.
The v29 rerun closed both analyzer gaps and proved that intentional
`CPU_LOGIC` execution is not counted as CPU fallback.
