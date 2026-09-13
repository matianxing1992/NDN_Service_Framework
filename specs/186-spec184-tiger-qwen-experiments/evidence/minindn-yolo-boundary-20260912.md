# Spec186 YOLO MiniNDN execution boundary

**Date:** 2026-09-12
**Candidate:** profile sources are present, but no accepted runtime candidate digest was promoted.
**Status:** `WAITING_EXTERNAL_INPUT`

The three fresh entrypoint attempts were:

```text
python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-A
python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-B
python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-N
```

Each exited `78` before NFD, Controller, Provider or User startup with:

```text
SPEC180_CASE_RESULT status=WAITING_EXTERNAL_INPUT error=ENVIRONMENT_MISSING:
NDNSF_DI_STATE_ROOT,NDNSF_DI_ENVELOPE_KEY_FILE,SPEC180_YOLO_CANONICAL_PACKAGE,
NDNSF_DI_CATALOGUE_REGISTRY,NDNSF_DI_CATALOG_DATA_NAME,NDNSF_DI_CATALOG_SIGNER,
NDNSF_DI_OFFER_TRUST_ROOT,NDNSF_DI_OFFER_PUBLIC_KEY_MAP,
NDNSF_DI_OFFER_PRIVATE_KEY_MAP,NDNSF_DI_TOPOLOGY,NDNSF_DI_CONFIG
```

The repository entrypoint reports the actual environment names in its
structured receipt; the shortened display above groups the catalogue/offer
fields. No ACK, Selection, dependency Data, terminal response, numerical
oracle or cleanup event exists, so none of Y-A/Y-B/Y-N is a `LOCAL_CPU_PASS` or
`EXPECTED_REJECTION_PASS`. The missing package, registry, key maps, topology
and config must be supplied under a new run root and bound to a fresh
candidate digest before retry.
