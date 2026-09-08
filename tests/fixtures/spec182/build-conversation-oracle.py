#!/usr/bin/env python3
"""Freeze legacy checkpoint wire evidence for native C++ tests (offline only)."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/conversation.py"


def build():
    spec = importlib.util.spec_from_file_location("spec182_conversation_reference", SOURCE)
    reference = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = reference
    spec.loader.exec_module(reference)
    # Public fixture material only; never load production keys or journals.
    key = bytes(range(32))
    digest = lambda value: "sha256:" + hashlib.sha256(value.encode()).hexdigest()
    cases = []
    parent = None
    for epoch, tokens in ((1, (10, 11)), (2, (10, 11, 12, 13))):
        checkpoint = reference.ConversationCheckpointV1(
            conversation_id="0123456789abcdef0123456789abcdef",
            parent_context_epoch=epoch - 1, context_epoch=epoch,
            service_name="/service/会话", requester_identity="/requester/A",
            security_domain_digest=digest("security"), model_contract_digest=digest("model"),
            plan_role_map_digest=digest("roles"),
            logical_prefix_digest=reference._prefix_digest(tokens),
            prefix_token_count=len(tokens),
            role_receipt_digests={"/role/B": digest("receipt-B-" + str(epoch)),
                                 "/role/A": digest("receipt-A-" + str(epoch))},
            issued_at_ms=2_000_000_000_000 + epoch,
            expires_at_ms=2_000_000_100_000 + epoch,
        ).sign(key)
        wire = checkpoint.to_bytes()
        parsed = reference.ConversationCheckpointV1.from_bytes(wire)
        assert parsed.to_bytes() == wire and parsed.verify(key)
        assert not parsed.verify(bytes(reversed(key)))
        # Digest validation must detect authenticated field changes.
        changed = json.loads(wire)
        changed["prefixTokenCount"] += 1
        try:
            reference.ConversationCheckpointV1.from_bytes(reference._canonical_payload(changed))
        except (ValueError, reference.ConversationCheckpointInvalid):
            pass
        else:
            raise AssertionError("reference accepted altered checkpoint")
        continuation = reference.ConversationContinuation(
            conversation_id=checkpoint.conversation_id,
            mode=(reference.ConversationInputMode.APPEND_DELTA if parent
                  else reference.ConversationInputMode.FULL_CONTEXT),
            parent_checkpoint=parent,
            expected_parent_context_epoch=epoch - 1 if parent else None,
            turn_input_digest=digest("input-" + str(epoch)),
        )
        cases.append({"name": "first" if epoch == 1 else "append",
                      "canonicalTokenIds": list(tokens),
                      "continuation": continuation.to_dict(),
                      "checkpointWire": wire.decode(),
                      "checkpointDigest": checkpoint.checkpoint_digest,
                      "signature": checkpoint.signature})
        parent = wire
    return {"schema": "spec182-conversation-oracle-v1",
            "source": str(SOURCE.relative_to(ROOT)),
            "sourceSha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
            "testOnlyAuthenticationKeyHex": key.hex(), "cases": cases}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    encoded = json.dumps(build(), ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    if args.check:
        if args.output.read_text() != encoded:
            raise SystemExit("conversation oracle differs from reference")
    else:
        args.output.write_text(encoded)
    print("PASS: 2 legacy checkpoint/continuation vectors; round-trip, wrong-key and tamper checks")


if __name__ == "__main__":
    main()
