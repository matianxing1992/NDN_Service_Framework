# T002 Input Publication and Binding Implementation Evidence

**Status**: implemented and focused-tested; production qualification remains open

## Source boundary

The maintained YOLO caller in
`examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` now publishes
encoded input bytes through
`app_sdk.client.APPClient.publish_application_input_reference()` before it
constructs `ApplicationInput.from_repo_ref()` or calls `request_task()`.
`--input-reference-file` is rejected by the ACK-driven path; it remains an
offline-oracle input only.

The canonical APP owner delegates publication to the native
`ServiceUser::publishEncryptedLargeData` path. The native result now carries
the source-bound plaintext size, content digest, publication-manifest digest,
authorization scope, protection epoch, and encryption bit. The Python binding
exposes the same fields. `bind_published_large_data_reference()` verifies all
of them against the exact source bytes and fails closed when an old binding
returns only the legacy name/object ID.

The APP owner records only non-secret publication metadata in its durable
journal with event type `INPUT_REFERENCE_PUBLISHED`. `request_task()` requires
the canonical reference digest to match that journal before it prints the
request-sent marker and delegates to the existing coordinator.

## Checks run

```text
./waf build --targets=ndn-service-framework -j2                         PASS
(cd pythonWrapper && python3 setup.py build_ext --inplace --force)       PASS
PYTHONPATH=pythonWrapper python3 -c 'import ndnsf._ndnsf; ...'          PASS
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper python3 -m pytest \
  -q tests/python/test_spec180_candidate.py \
     tests/python/test_spec180_contract_gate.py \
     tests/python/test_spec180_generic_request_api.py \
     tests/python/test_spec180_role_assembly.py \
     tests/python/test_spec180_yolo_ack_planning.py \
     tests/python/test_spec180_yolo_adapter.py \
     tests/python/test_spec180_yolo_application.py \
     tests/python/test_spec180_yolo_security.py \
     tests/python/test_spec180_catalog_resolver.py \
     tests/python/test_spec180_qwen_reference.py                    65 passed
```

An earlier verification refresh ran the then-current Spec180 collection as
`tests/python/test_spec180_*.py`: **71 passed, 19 warnings**. That count is
historical evidence for the implementation slice, not the current collection
total; later inventory and local-gate tests increased the current collection
to 91 tests.

The import check was performed with the repository host Python 3.8 extension
and only proves that the updated binding exposes the metadata fields. It is not
evidence for the final Python 3.10 SIF or for a live NDN publication.

## Remaining boundary

No live encrypted repository fetch, ACK closure, Selection, Provider
execution, terminal Response, MiniNDN case, SIF replay, or Tiger job has been
run for this evidence. T002/T009/T011 therefore remain unchecked, and this
file cannot be used as qualification evidence.
