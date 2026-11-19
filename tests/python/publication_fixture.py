"""Mock SDK result metadata; real packet behavior uses the C++ oracle."""
import hashlib
import json
from types import SimpleNamespace

from ndn.encoding import Component
from ndnsf_distributed_inference.naming import (
    canonical_name_uri, make_naming_digest_component, make_semantic_content_prefix)


def publication_result(payload, service, *, producer="/encrypted", label="payload",
                       content_name=None):
    payload = bytes(payload)
    content_digest = hashlib.sha256(payload).hexdigest()
    representation_digest = hashlib.sha256(("mock:" + service).encode()).hexdigest()
    metadata = dict(content_name or {"kind": "blob", "label": label})
    base = make_semantic_content_prefix(producer, metadata, content_digest) + [
        bytes(Component.from_str("PROTECTED")),
        make_naming_digest_component(representation_digest)]
    data_prefix = canonical_name_uri(base + [bytes(Component.from_str("DATA"))])
    descriptor = {"contentName": metadata, "contentDigest": content_digest, "size": len(payload),
                  "scope": "/SERVICE" + service, "dataPrefix": data_prefix}
    manifest_digest = hashlib.sha256(json.dumps(
        descriptor, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    manifest_name = canonical_name_uri(base + [bytes(Component.from_str("MANIFEST")),
        make_naming_digest_component(manifest_digest)])
    return SimpleNamespace(
        success=True, error="", encrypted=True, manifest_name=manifest_name,
        data_prefix=data_prefix, encrypted_data_name=data_prefix,
        content_digest="sha256:" + content_digest,
        manifest_digest="sha256:" + manifest_digest,
        object_id=label, plaintext_size=len(payload),
        authorization_scope="/SERVICE" + service, protection_epoch="mock-epoch")
