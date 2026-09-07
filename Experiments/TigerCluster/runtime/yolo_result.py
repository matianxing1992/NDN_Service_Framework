"""Shared YOLO evidence checks. Publication readiness is NOT inference PASS.

Request/role/edge/numerical/cleanup collection is still pending in T006.
"""
from collections.abc import Mapping
import re


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
