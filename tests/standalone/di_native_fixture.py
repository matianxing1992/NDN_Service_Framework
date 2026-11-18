"""Pure-Python metadata helpers for Spec185 native process fixtures.

The process drivers own only external lifecycle setup.  Keeping their small
descriptor encoder independent of ``pythonWrapper`` lets sanitizer-built C++
processes run without loading a sanitizer-instrumented extension into Python.
"""

import hashlib
import json
import os
from pathlib import Path


def fixture_path(relative):
    """Read only a declared fixture root; never search a checkout or the current directory."""
    relative = Path(relative)
    if relative.is_absolute() or '..' in relative.parts:
        raise ValueError('fixture path must be relative and contained')
    if 'NDNSF_TEST_FIXTURE_ROOT' in os.environ:
        value = os.environ['NDNSF_TEST_FIXTURE_ROOT']
        if not value:
            raise ValueError('NDNSF_TEST_FIXTURE_ROOT is empty')
        root = Path(value).resolve(strict=True)
    else:
        root = (Path(__file__).resolve().parents[1] / 'fixtures').resolve(strict=True)
    path = (root / relative).resolve(strict=True)
    path.relative_to(root)
    if not path.is_file():
        raise ValueError('test fixture is not a regular file: ' + str(path))
    return path


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
