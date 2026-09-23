"""Pure-Python metadata helpers for Spec185 native process fixtures.

The process drivers own only external lifecycle setup.  Keeping their small
descriptor encoder independent of ``pythonWrapper`` lets sanitizer-built C++
processes run without loading a sanitizer-instrumented extension into Python.
"""

import hashlib
import json


def canonical_bytes(value):
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False,
    ).encode("utf-8")


def digest(value):
    return "sha256:" + hashlib.sha256(canonical_bytes(value)).hexdigest()


def adapter_descriptor(
    name, version, state_digest, abi, model_formats, tasks, backends,
    precisions, input_schema_digest, options_schema_digest,
    result_schema_digest, graph_schema_digest, split_schema_digest,
    state_schema_digest, graph_inspectable, splittable,
    deterministic_analysis=True,
):
    return {
        "name": name,
        "version": version,
        "state_digest": state_digest,
        "abi": abi,
        "model_formats": list(model_formats),
        "tasks": list(tasks),
        "backends": list(backends),
        "precisions": list(precisions),
        "input_schema_digest": input_schema_digest,
        "options_schema_digest": options_schema_digest,
        "result_schema_digest": result_schema_digest,
        "graph_schema_digest": graph_schema_digest,
        "split_schema_digest": split_schema_digest,
        "state_schema_digest": state_schema_digest,
        "graph_inspectable": bool(graph_inspectable),
        "splittable": bool(splittable),
        "deterministic_analysis": bool(deterministic_analysis),
    }


def model_descriptor(
    model_name, content_digest, semantics_digest, graph_digest,
    model_format, precision, adapter, source_revision="",
):
    return {
        "model_name": model_name,
        "content_digest": content_digest,
        "semantics_digest": semantics_digest,
        "graph_digest": graph_digest,
        "model_format": model_format,
        "precision": precision,
        "adapter": adapter,
        "source_revision": source_revision,
    }
