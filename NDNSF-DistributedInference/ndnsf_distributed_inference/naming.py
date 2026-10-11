"""Producer naming wire codec. Business binding and authorization stay with their owners."""

from __future__ import annotations

import hashlib
import re
import struct
import json
from typing import NamedTuple

from ndn.encoding import Component, Name


class NamingDigestField(NamedTuple):
    tag: int
    kind: int
    value: bytes


class ParsedProducerDataName(NamedTuple):
    producer: list
    category: str
    suffix: list
    name_space: str = "NDNSF"


def _name(value):
    if isinstance(value, (bytes, bytearray, memoryview)):
        try:
            components, consumed = Name.decode(value)
        except (IndexError, struct.error) as exc:
            raise ValueError("invalid naming Name wire") from exc
        if consumed != len(value):
            raise ValueError("incomplete naming Name wire")
    elif isinstance(value, str):
        # python-ndn's URI parser treats dot-only values literally. ndn-cxx's
        # canonical URI adds three dots, including for an empty component.
        text = value[4:] if value.startswith("ndn:") else value
        components = []
        for part in text.split("/"):
            if not part:
                continue
            prefix, equal, tail = part.partition("=")
            component_value = tail if equal else part
            if component_value and all(c == "." for c in component_value):
                if len(component_value) < 3:
                    raise ValueError("invalid dot-only canonical component")
                typ = int(prefix) if equal else Component.TYPE_GENERIC
                components.append(Component.from_bytes(b"." * (len(component_value) - 3), typ))
            else:
                components.append(Component.from_str(part))
    else:
        components = Name.normalize(value)
    for component in components:
        Component.to_str(component)  # Validate the entire component TLV.
        if not 1 <= Component.get_type(component) <= 65535:
            raise ValueError("invalid Name component type")
    return [bytes(component) for component in components]


def _label(value):
    return isinstance(value, str) and re.fullmatch(r"[A-Z0-9-]+", value) is not None


def canonical_name_uri(value):
    parts = []
    for component in _name(value):
        typ = Component.get_type(component)
        raw = bytes(Component.get_value(component))
        escaped = "".join(chr(c) if c in b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"
                          else f"%{c:02X}" for c in raw)
        if not raw or all(c == ord(".") for c in raw):
            escaped += "..."
        parts.append(f"{typ}=" + escaped)
    return "/" + "/".join(parts)


def _category_allowed(name_space, category):
    if name_space == "NDNSF":
        return _label(category) or category in {"blob", "event"}
    return name_space == "NDNSF-DI" and category in {
        "model", "model-alias", "material", "manifest-page", "receipt", "assignment",
        "plan", "input", "invocation", "tensor", "collective", "result", "event", "conversation",
    }


def make_producer_data_prefix(producer, category, *, name_space="NDNSF"):
    producer = _name(producer)
    if not producer or not _category_allowed(name_space, category):
        raise ValueError("invalid Producer prefix or Data category")
    return producer + [bytes(Component.from_bytes(name_space.encode("ascii"))),
                       bytes(Component.from_bytes(category.encode("ascii")))]


def make_naming_label_component(full_label):
    if not isinstance(full_label, str) or not full_label:
        raise ValueError("invalid semantic label")
    if any(ord(c) < 0x20 or 0x7f <= ord(c) <= 0x9f for c in full_label):
        raise ValueError("semantic label contains control character")
    try:
        raw = full_label.encode("utf-8", errors="strict")
    except UnicodeError as exc:
        raise ValueError("invalid semantic label UTF8") from exc
    length = min(64, len(raw))
    while length < len(raw) and raw[length] & 0xc0 == 0x80:
        length -= 1
    return bytes(Component.from_bytes(b"name=" + raw[:length]))


def matches_naming_label_component(component, full_label):
    try:
        return bytes(component) == make_naming_label_component(full_label)
    except (ValueError, TypeError, UnicodeError):
        return False


def _semantic_content_context(fields):
    allowed = {"kind", "label", "model_name", "model_identity", "artifact_kind",
               "coordinate", "profile_identity", "input_kind"}
    if not isinstance(fields, dict) or set(fields) - allowed:
        raise ValueError("invalid semantic content fields")
    values = {key: fields.get(key, "") for key in allowed}
    if any(not isinstance(value, str) for value in values.values()):
        raise ValueError("semantic content fields require strings")
    kind, label = values["kind"], values["label"]
    make_naming_label_component(label)
    leaves = {"model-file": "file", "weights": "weights", "initializer": "initializer",
              "tokenizer": "tokenizer", "artifact-file": "file"}
    atom = lambda value: re.fullmatch(r"[A-Za-z0-9=.\-]+", value) is not None
    name_space, category, parent = "NDNSF-DI", "model", {}
    if kind == "assignment":
        make_naming_label_component(values["model_name"])
        make_naming_digest_component(values["model_identity"])
        if any(values[key] for key in ("artifact_kind", "coordinate", "profile_identity", "input_kind")):
            raise ValueError("unexpected assignment metadata")
        category = semantic_category = "assignment"
        parent = {"modelIdentity": values["model_identity"], "modelName": values["model_name"]}
    elif kind in leaves:
        make_naming_label_component(values["model_name"])
        make_naming_digest_component(values["model_identity"])
        if values["input_kind"]:
            raise ValueError("model has input kind")
        if kind == "artifact-file":
            if not atom(values["artifact_kind"]) or not atom(values["coordinate"]):
                raise ValueError("invalid artifact kind/coordinate")
            make_naming_digest_component(values["profile_identity"])
        elif any(values[key] for key in ("artifact_kind", "coordinate", "profile_identity")):
            raise ValueError("unexpected artifact identity")
        parent = {"coordinate": values["coordinate"], "kind": values["artifact_kind"],
                  "modelIdentity": values["model_identity"], "profile": values["profile_identity"]}
        semantic_category = "model/" + ("artifact/file" if kind == "artifact-file" else leaves[kind])
    else:
        if kind not in {"blob", "material", "input"}:
            raise ValueError("unknown semantic content kind")
        if any(values[key] for key in ("model_name", "model_identity", "artifact_kind",
                                      "coordinate", "profile_identity")):
            raise ValueError("unexpected model identity")
        name_space, category = ("NDNSF" if kind == "blob" else "NDNSF-DI"), kind
        semantic_category = kind
        if kind == "input":
            if not atom(values["input_kind"]):
                raise ValueError("invalid input kind")
            parent = {"kind": values["input_kind"]}
        elif values["input_kind"]:
            raise ValueError("unexpected input kind")
    identity = compute_naming_digest("ndnsf-naming/semantic-object", [
        NamingDigestField(1, 2, name_space.encode("ascii")),
        NamingDigestField(2, 2, semantic_category.encode("ascii")),
        NamingDigestField(3, 2, label.encode("utf-8")),
        NamingDigestField(4, 5, json.dumps(parent, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")),
    ])
    return values, name_space, category, identity, leaves


def validate_semantic_content_fields(fields):
    """Copy validated metadata without inventing a Producer or publishing Data."""
    values, _, _, _, _ = _semantic_content_context(fields)
    return {key: value for key, value in values.items() if value}


def make_semantic_content_prefix(producer, fields, plaintext_digest):
    """C02 immutable content prefix; Core remains the actual packet publisher."""
    values, name_space, category, identity, leaves = _semantic_content_context(fields)
    kind, label = values["kind"], values["label"]
    name = make_producer_data_prefix(producer, category, name_space=name_space)
    raw = lambda text: bytes(Component.from_bytes(text.encode("utf-8")))
    if kind == "assignment":
        name += [raw("model"), make_naming_label_component(values["model_name"]),
                 raw("ROLE"), make_naming_label_component(label),
                 make_naming_digest_component(plaintext_digest)]
        check_naming_name_size(name)
        return name
    if kind in leaves:
        name += [make_naming_label_component(values["model_name"]),
                 make_naming_digest_component(values["model_identity"])]
        if kind == "artifact-file":
            name += [raw("artifact"), raw(values["artifact_kind"]), raw(values["coordinate"]),
                     make_naming_digest_component(values["profile_identity"])]
        name += [raw(leaves[kind])]
    elif kind == "input":
        name += [raw(values["input_kind"])]
    name += [make_naming_label_component(label), make_naming_digest_component(identity),
             make_naming_digest_component(plaintext_digest)]
    check_naming_name_size(name)
    return name


def infer_semantic_data_producer(data_prefix):
    """Resolve only supported C02 DATA grammars; no marker scan or legacy fallback."""
    name = _name(data_prefix)
    raw = lambda text: bytes(Component.from_bytes(text.encode("ascii")))
    digest = lambda c: Component.get_type(c) == 8 and len(Component.get_value(c)) == 32

    def readable(c):
        if Component.get_type(c) != 8:
            return False
        value = bytes(Component.get_value(c))
        try:
            return (6 <= len(value) <= 69 and value.startswith(b"name=")
                    and matches_naming_label_component(c, value[5:].decode("utf-8", errors="strict")))
        except UnicodeError:
            return False

    def atom(c):
        return (Component.get_type(c) == 8 and
                re.fullmatch(rb"[A-Za-z0-9=.\-]+", bytes(Component.get_value(c))) is not None)

    def semantic(s):
        if len(s) == 5 and (s[:2] == [raw("NDNSF"), raw("blob")] or
                            s[:2] == [raw("NDNSF-DI"), raw("material")]):
            return readable(s[2]) and digest(s[3]) and digest(s[4])
        if len(s) == 6 and s[:2] == [raw("NDNSF-DI"), raw("input")]:
            return atom(s[2]) and readable(s[3]) and digest(s[4]) and digest(s[5])
        if len(s) == 7 and s[:2] == [raw("NDNSF-DI"), raw("assignment")]:
            return (s[2] == raw("model") and readable(s[3]) and s[4] == raw("ROLE")
                    and readable(s[5]) and digest(s[6]))
        if (len(s) not in (8, 12) or s[:2] != [raw("NDNSF-DI"), raw("model")]
                or not readable(s[2]) or not digest(s[3])):
            return False
        if len(s) == 8:
            return (s[4] in [raw(k) for k in ("file", "weights", "initializer", "tokenizer")]
                    and readable(s[5]) and digest(s[6]) and digest(s[7]))
        return (s[4] == raw("artifact") and atom(s[5]) and atom(s[6]) and digest(s[7])
                and s[8] == raw("file") and readable(s[9]) and digest(s[10]) and digest(s[11]))

    if len(name) < 3 or name[-3] != raw("PROTECTED") or not digest(name[-2]) or name[-1] != raw("DATA"):
        return None
    candidates = [name[:-length - 3] for length in (5, 6, 7, 8, 12)
                  if len(name) > length + 3 and semantic(name[-length - 3:-3])]
    return candidates[0] if len(candidates) == 1 else None


def make_naming_digest_component(sha256_hex):
    if not isinstance(sha256_hex, str) or re.fullmatch(r"[0-9a-f]{64}", sha256_hex) is None:
        raise ValueError("naming digest requires full lowerhex SHA-256")
    return bytes(Component.from_bytes(bytes.fromhex(sha256_hex)))


def make_naming_decimal_component(value):
    if type(value) is not int or not 0 <= value <= (1 << 64) - 1:
        raise ValueError("invalid naming UInt64")
    return bytes(Component.from_str(str(value)))


def compute_naming_digest(domain, ordered_fields):
    try:
        encoded_domain = domain.encode("ascii")
    except (AttributeError, UnicodeError) as exc:
        raise ValueError("invalid naming digest domain") from exc
    fields = list(ordered_fields)
    if not encoded_domain or b"\0" in encoded_domain or len(encoded_domain) > 65535 or len(fields) > 65535:
        raise ValueError("invalid naming digest domain/count")
    digest = hashlib.sha256()
    digest.update(struct.pack(">H", len(encoded_domain)) + encoded_domain + struct.pack(">HH", 2, len(fields)))
    previous = -1
    for tag, kind, value in fields:
        if type(tag) is not int or not previous < tag <= 65535 or tag < 0:
            raise ValueError("naming digest tags must be strictly increasing")
        if type(kind) is not int or not 1 <= kind <= 5:
            raise ValueError("unknown naming digest field kind")
        if not isinstance(value, (bytes, bytearray, memoryview)):
            raise ValueError("naming field requires raw bytes")
        value = bytes(value)
        if kind == 1:
            parsed = _name(value)
            if bytes(Name.encode(parsed)) != value:
                raise ValueError("noncanonical naming Name wire")
        elif kind == 3 and len(value) != 8:
            raise ValueError("naming UInt64 must contain eight bytes")
        elif kind in (2, 5):
            try:
                value.decode("utf-8", errors="strict")
            except UnicodeError as exc:
                raise ValueError("invalid naming UTF8 field") from exc
        digest.update(struct.pack(">HBQ", tag, kind, len(value)))
        digest.update(value)
        previous = tag
    return digest.hexdigest()


def make_reversible_name_component(name):
    return bytes(Component.from_bytes(canonical_name_uri(name).encode("ascii")))


def parse_reversible_name_component(component):
    try:
        if Component.get_type(component) != Component.TYPE_GENERIC:
            return None
        Component.to_str(component)
        uri = bytes(Component.get_value(component)).decode("ascii", errors="strict")
        if not uri.startswith("/"):
            return None
        name = _name(uri)
        return name if canonical_name_uri(name) == uri else None
    except (ValueError, IndexError, UnicodeError, struct.error):
        return None


def parse_producer_data_name(name, expected_producer):
    try:
        name, producer = _name(name), _name(expected_producer)
        boundary = len(producer)
        if not producer or name[:boundary] != producer or len(name) <= boundary + 2:
            return None
        if Component.get_type(name[boundary]) != Component.TYPE_GENERIC:
            return None
        name_space = bytes(Component.get_value(name[boundary])).decode("ascii", errors="strict")
        category_component = name[boundary + 1]
        if Component.get_type(category_component) != Component.TYPE_GENERIC:
            return None
        category = bytes(Component.get_value(category_component)).decode("ascii", errors="strict")
        if not _category_allowed(name_space, category):
            return None
        return ParsedProducerDataName(producer, category, name[boundary + 2:], name_space)
    except (ValueError, IndexError, UnicodeError, struct.error):
        return None


def check_naming_name_size(name, max_name_wire_bytes=1024):
    if type(max_name_wire_bytes) is not int or max_name_wire_bytes <= 0:
        raise ValueError("invalid naming wire budget")
    size = len(Name.encode(_name(name)))
    if size > max_name_wire_bytes:
        raise ValueError(f"NAME_TOO_LONG: wire={size} limit={max_name_wire_bytes}")


def make_producer_rpc_name(reply_producer, operation, call_identity_digest, query_nonce):
    if not _label(operation) or not isinstance(query_nonce, str) or not query_nonce:
        raise ValueError("invalid RPC operation/nonce")
    return make_producer_data_prefix(reply_producer, "RPC") + [
        bytes(Component.from_str(operation)), make_naming_digest_component(call_identity_digest),
        bytes(Component.from_bytes(query_nonce.encode("utf-8"))),
    ]


def make_invocation_event_name(producer, request_id, service, attempt, binding_digest, cursor):
    """C05 Name codec using the native owner's complete binding commitment."""
    request_id, service = _name(request_id), _name(service)
    if not request_id or not service or type(attempt) is not int or attempt not in (1, 2):
        raise ValueError("invalid event invocation identity")
    if type(cursor) is not int or not 1 <= cursor <= (1 << 64) - 1:
        raise ValueError("invalid event sequence")
    components = make_producer_data_prefix(producer, "event") + [
        bytes(Component.from_str("REQUEST")),
        make_naming_label_component(canonical_name_uri(request_id)),
        bytes(Component.from_str("ATTEMPT")), make_naming_decimal_component(attempt),
        bytes(Component.from_str("SERVICE")),
        make_naming_label_component(canonical_name_uri(service)),
        make_naming_digest_component(binding_digest),
        bytes(Component.from_number(cursor, 58)),
    ]
    check_naming_name_size(components)
    return canonical_name_uri(components)
