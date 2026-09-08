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
