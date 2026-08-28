# Spec 168 v51 Gate B - Targeted Selection transport gap

## Verdict

`BLOCK` — `EXEC_LOCAL_GATE_HARD_DEADLINE` after `900004.779 ms`. The gate
recorded `automaticRetry=false`; the only actual v51 runtime attempt was not
retried or overwritten. An earlier launcher preflight rejection created no
container and is not counted as a runtime attempt.

## Frozen identity

- Candidate: `20260804T080022Z-v51-targeted-selection`
- Source identity: `sha256:6860a00ccf0603937c692483d42109152f5d6a7e1d520ea268a797a7b8d57030`
- Source bundle: `sha256:71ff94b0eb99aa83dd74af110b56cb1102714b5c3c68f85466440af7d5d679d1`
- Gate command: `sha256:c9fddcedc565f082e13cfd322aaf349b7e3e03bfb904a0a3484d0c5ff4cb2a09`
- Gate manifest: `sha256:140cf6b1584edd34c3f5a40ab0210af178662fc9fd822d19bb180c95126e8ffa`
- Gate checkpoint: `sha256:026af16215482e5dbcbd6a76db7188f934be830ed89cf0d92f9df2a268a107ea`
- Launcher log: `sha256:b4ce3991bac4310241140b4b2fa661a33fe17cccc3c7528585dde23278d2988f`

## Narrowest lifecycle boundary

The Controller, three Repo Providers and three inference Providers completed
normal startup. The inference Request collected three successful ACKs, closed
the ACK window, committed its automatic plan, and opened one deferred
DistributedRepo `Artifact/v2/STORE` collaboration. All three Repo Providers
returned successful ACKs. The User attached the selected Repo Provider's
`2196`-byte provider-specific assignment, but no Repo Provider accepted the
corresponding Selection. No artifact publication, model fetch, model load,
generated token, or final Response occurred.

Unlike v50, v51 enabled `NDNSF_SELECTION_TARGETED_PREFETCH=1`. During the
running attempt, `/proc/<pid>/environ` confirmed the switch in the User, all
three Repo Provider processes, and all three inference Provider processes. The
User loaded `/opt/ndnsf-app/lib/libndn-service-framework.so.0.1.0`; that binary
contains the `SELECTION_DIRECT_PUT` and
`SELECTION_TARGETED_PREFETCH_{ISSUED,DATA,TIMEOUT}` paths. An attached debugger
showed the Face event-loop thread idle in `epoll_wait`, so the failure was not a
Python caller blocking queued Face work.

Therefore v51 rules out missing harness activation. The remaining boundary is
the exact-name Selection transport itself: either the one-shot unsolicited
Selection Data is not retained for the Provider's later Interest, or the User
and Provider construct different exact names. This is still a pre-inference
transport/lifecycle failure, not a Qwen, CUDA, Repo-throughput, or TigerCluster
failure.

Retained runtime log digests:

- User: `sha256:2968af40e140640aaa29afa8676d2ee26a6bc365af1c5293f70e6939c39f79a0`
- Repo 0: `sha256:e8229e827df27614f5793542be66d17aa308918e982c626ac3cf9c3cfff6c4e7`
- Repo 1: `sha256:6b064eee143249ce62df3b81f53c6526b6679dab332079d8d4916447b68cec14`
- Repo 2: `sha256:17158f7377a9d91fdc95ba3d671c74834e72dba9c8aefc17f5bf44dea9aacf05`
- Stage 0: `sha256:4daa07207cdbf1aeabc18186ac3c19675535aad09985b64e276cb8bfbb60d479`
- Stage 1: `sha256:e8f48df2ded3895fb920e1012cc5a790569487c562656bc0527b8f98df82d703`
- Stage 2: `sha256:ce26d3f4d40515ea1e4ab8c78f2409020bfc730c48aa7648712b83829c026ffb`

## Next diagnostic boundary

v52 is trace-only and non-admitted. It reuses the v49 native image, model
assets, routes, and Repo payload, while enabling Selection transport TRACE
markers through a new immutable source identity. It must establish the exact
User-published and Provider-requested names and the
`ISSUED/DATA/TIMEOUT/NACK` outcome before any transport repair is accepted.
It cannot satisfy Gate B and cannot be submitted to TigerCluster.
