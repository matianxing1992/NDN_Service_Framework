"""Shared YOLO evidence checks. Publication readiness is NOT inference PASS.

Numerical reanalysis is a component only. Full lifecycle/role/edge/cleanup
collection is still pending in T006.
"""
from collections.abc import Mapping
import re


def reanalyze_numerical_response(root, reference, *, case, request_id, attempt_id,
                               plan_digest, result_digest, candidate_id, candidate_digest):
    """Recompute fixed-input numerical comparison from retained User bytes.

    Caller authenticates/pins the independent reference and lifecycle inputs.
    This is one request's numerical component, NOT the complete T006 verdict.
    """
    import hashlib
    from pathlib import Path
    from runtime.yolo_profile import _read_plane
    from runtime.yolo_bundle import _bytes
    from ndnsf_distributed_inference.adapters.yolo.tensor_bundle import decode_tensor_bundle
    from ndnsf_distributed_inference.adapters.yolo.reference import compare_reference
    root = Path(root)
    record_path, payload_path = root / 'yolo-numerical.json', root / 'yolo-response.bin'
    if any(p.is_symlink() for p in (record_path, payload_path, root, *root.parents)):
        raise EvidenceError('NUMERICAL_SYMLINK')
    record = _read_plane(record_path)
    expected = dict(schemaVersion='spec180-yolo-numerical-v1', case=case, requestId=request_id,
        attemptId=attempt_id, planDigest=plan_digest, candidateId=candidate_id, candidateDigest=candidate_digest,
        manifestDigest=reference.manifest_digest, oracleDigest=reference.oracle_digest,
        fixtureDigest=reference.fixture_digest,
        inputTensorDigest='sha256:' + hashlib.sha256(reference.input_tensor.tobytes()).hexdigest(),
        responseDigest=result_digest, responsePath='yolo-response.bin')
    if any(record.get(k) != v for k, v in expected.items()):
        raise EvidenceError('NUMERICAL_LINEAGE_OR_REFERENCE')
    payload = _bytes(payload_path)
    if (not 0 < len(payload) <= 1024 * 1024 or type(record.get('responseBytes')) is not int
            or record['responseBytes'] != len(payload)
            or 'sha256:' + hashlib.sha256(payload).hexdigest() != result_digest):
        raise EvidenceError('NUMERICAL_RESPONSE_BYTES')
    tensors = decode_tensor_bundle(payload)
    if set(tensors) != {'predictions'}:
        raise EvidenceError('NUMERICAL_RESPONSE_TENSORS')
    comparison = compare_reference(reference, tensors['predictions'])
    if (comparison['matched'] is not True or record.get('matched') is not True
            or any(record.get(k) != v for k, v in comparison.items())):
        raise EvidenceError('NUMERICAL_REANALYSIS_FAILED')
    return dict(comparison, responseDigest=result_digest, qualification='NUMERICAL_COMPONENT_ONLY')


class EvidenceError(ValueError):
    pass


def validate_runtime_publication_receipt(expected, receipt):
    """Match Controller readback evidence, retaining exact artifact cardinality."""
    if (not isinstance(expected, Mapping) or not isinstance(receipt, Mapping)
            or receipt.get('schema') != 'spec180-runtime-publication-receipt-v1'):
        raise EvidenceError('CASE_RUNTIME_PUBLICATION_RECEIPT_SCHEMA_INVALID')
    for field in ('catalogueDataName', 'catalogueSigner', 'cataloguePayloadDigest'):
        if (not isinstance(expected.get(field), str) or not expected[field]
                or receipt.get(field) != expected[field]):
            raise EvidenceError('CASE_RUNTIME_PUBLICATION_RECEIPT_MISMATCH')
    def rows(document):
        values = document.get('artifacts')
        if not isinstance(values, list) or not values or len(values) > 256:
            raise EvidenceError('CASE_RUNTIME_PUBLICATION_ARTIFACT_RECEIPT_MISMATCH')
        result = {}
        for row in values:
            if (not isinstance(row, Mapping) or not isinstance(row.get('dataName'), str)
                    or not row['dataName'].startswith('/')
                    or not isinstance(row.get('payloadDigest'), str)
                    or not re.fullmatch(r'sha256:[0-9a-f]{64}', row['payloadDigest'])
                    or row['dataName'] in result):
                raise EvidenceError('CASE_RUNTIME_PUBLICATION_ARTIFACT_RECEIPT_MISMATCH')
            result[row['dataName']] = row['payloadDigest']
        return result
    if rows(expected) != rows(receipt):
        raise EvidenceError('CASE_RUNTIME_PUBLICATION_ARTIFACT_RECEIPT_MISMATCH')
    return receipt
