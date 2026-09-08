# Explicit layered input/runtime/dispatch rendering

2026-09-08: component/content scope only; no runtime qualification.

The existing input renderer accepts `--source-files <json>` (sourceLock,
sourceSeal, buildDefinition, baseSif) and `--layout layered-v1`. The existing
dispatch renderer accepts `--runtime-source-files <json>` (sif, nativeManifest,
libraryLock) and `--application <frozen app>`. Descriptor-relative paths resolve
against that descriptor. Legacy defaults remain distinct.

Layered rendering requires matching input layout and fresh runtime/dispatch
directories. It verifies all app files against the selected base digest,
records the app manifest in E, uses the profile's actual workload/oracle/security
references, and updates release/harness paths before computing effective
behavior. It cannot silently replace an existing layered receipt directory.

Three focused tests pass (`layered-plane-render-r3.xml` under the retained
yolo-layered-20260908 result root): explicit I sources, descriptor validation,
and full renderer→production profile validator with candidate-only fixtures.
The full path proves content VERIFIED / qualification NOT_EVALUATED and
rejects reuse of the output directory. No synthetic bytes are a model result.

An accidentally selected legacy real-input test failed FILE_DIGEST:baseSif
after rendering/reading the old disk SIF; this repeats the known buffered-read
problem, not an application regression. Initial fixture setup also lacked its
seed directory and was corrected. Both failures remain in layered-plane-render.xml.
The old input test is not being repeated; the verified RAM SDK remains the
current local execution input. Actual layered I/R/E publication, source/host-gate
semantics, MiniNDN and full GPU qualification remain pending.

## Actual layered candidate, using the verified RAM SIF

The default profile now points to `/dev/shm/spec183-sdk-d4031191/planes`, the
same d403 base and app-d9be0bfa manifest c086d64c… . Input/runtime metadata may
cross filesystems by bounded small-file copy; the large SIF remains hard-linked.
Four focused checks pass (`layered-ram-metadata-components.xml`).

`layered-production-check.log` is the actual public `submit.py check --stage
dispatch` result: content VERIFIED,29harness files,159application files,
qualification NOT_EVALUATED, status INCOMPLETE, intentional exit78.
Profile digest41670032e797de4431d9fe521a237877f7453392d2981de82728df74d9355320;
I775d15ee…, R5186b21e…, E357448bb… . Raw/render IDs are distinct from these
canonical stage IDs. `layered-plane-metadata.tar` and
`layered-profile-content-check.json` retain the metadata on disk without copying
the large SIF. RAM paths are local execution locators, not Tiger-ready transport.

The profile still declares Tiger's Apptainer1.3.4-1.el9 at/usr/bin/apptainer;
this host has1.5.3 at/opt/apptainer/1.5.3/bin/apptainer and no/usr/bin/apptainer.
Do not merely overwrite the expected version to pass the runtime gate. Explicit
local/cluster environment binding and the MiniNDN issuer/wrapper remain next.
