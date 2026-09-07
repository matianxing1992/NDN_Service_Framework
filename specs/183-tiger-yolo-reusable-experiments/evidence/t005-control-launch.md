# T005 partial: Controller/Repo launch and protected-material audit

Date: 2026-09-07. No runtime qualification or remote submission.

## Implemented

`apps/yolo.py` now starts the maintained Controller and Repo entrypoints through
NodeRuntime. Both consume immutable `/config/case.json` and generate writable
role-local `/output/generated-policy`. Controller consumes the bounded JSON
publication batch at `/config/runtime-publication.json`; its existing decoder
and signing/readback code remain authoritative. Repo validates the configured
provider prefix against its actual `/repo` identity, stores data under its own
`/output/repo-store`, and receives measured free capacity from preflight.
The fixed bounded cache is 64 MiB with zero preallocation and one handler/ACK
thread, matching the small qualification workload; final effective profile
must record these constants. No default multi-gigabyte free-capacity claim.

These functions return process handles, not READY. The coordinator must still
perform real signed Controller/permission/catalogue checks and enforce startup
order. No sleeps or fabricated readiness receipts were introduced.

## Focused tests

Five added process-boundary tests check both real argv shapes, role-local
writable paths, no CUDA/model mount on Controller/Repo, and rejection before
launch of missing publication, duplicate JSON fields, wrong Repo identity and
invalid capacity. Test processes replace Apptainer/native execution; JSON and
keys are synthetic, not signed/candidate-qualified inputs.

Executed: `python3 -m pytest -q Experiments/TigerCluster/tests tests/python/test_spec183_v3_backend_selection.py --tb=short --junitxml=Experiments/TigerCluster/results/spec183-control-launch-r1/junit.xml`.
Exit 0: **301 passed in 18.16s** (292 Tiger component tests plus 9 isolated
planning-kernel tests). No normal native planner import or GPU execution proof.

## Newly identified required protected-material wiring

The maintained `user.py::_build_grant_seam` returns `plaintext-v1` if
`SPEC181_PROTECTION_EPOCH` is absent. The current Spec183 command component does
not yet pass this environment, so it cannot qualify as protected execution.
Do not enable the five-command release launcher until this is fixed and tested.

The existing protected branch also loads `SPEC181_PROVIDER_RECIPIENT_KEY_MAP`
as paths to Provider **private** keys and derives public keys in the User.
That old fixture arrangement conflicts with Spec183 role-private HOME mounts.
Required repair: add/use a verified public recipient-key map for the requester;
keep each corresponding private key with its own Provider. Do not mount a
shared private-key directory into the User to make the old path work.

`security/requester_grant_pipeline.py` explicitly defines the existing functional
slice's authority as an in-process trusted operator authority, with a distinct
logical identity/key from the requester; a standalone authority service is
deferred. Spec183 should reuse that declared owner, explicitly record its trusted
co-location and allowlisted model/epoch, and bind its private locator and public
registry to the run. Do not silently invent a network authority service or claim
independent-process authority isolation from this existing seam. This co-location
does not authorize sharing Provider private keys.

T005 must wire epoch, requester seed, authority registry/key and public recipient
map, plus native Provider recipient-key inputs, using the existing owners. T004
schema/effective environment and T002 closure must bind those inputs. Missing,
plaintext, stale/mismatched epoch or key materials must reject before launch.
T007 stays open; process argv/component tests cannot satisfy these requirements.
