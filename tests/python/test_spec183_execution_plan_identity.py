"""Verbatim public property kernel; full native handle import remains T008."""
import ast
from pathlib import Path
import re
from types import SimpleNamespace as NS

import pytest


@pytest.mark.parametrize('metadata,expected', [({}, 'sha256:'+'1'*64),
    ({'plan_digest': 'sha256:'+'2'*64}, 'sha256:'+'2'*64),
    ({'plan_digest': 'invalid'}, None), ({'plan_digest': None}, None)])
def test_runtime_plan_identity_does_not_confuse_outer_carrier(metadata, expected):
    path = Path(__file__).resolve().parents[2]/'NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py'
    tree = ast.parse(path.read_text())
    cls = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'AutomaticInferenceHandle')
    method = next(n for n in cls.body if isinstance(n, ast.FunctionDef) and n.name == 'execution_plan_digest')
    method.decorator_list = []
    namespace = {'re': re}
    exec(compile(ast.fix_missing_locations(ast.Module(body=[method], type_ignores=[])), str(path), 'exec'), namespace)
    handle = NS(conversation_metadata=metadata, sealed_plan=NS(plan_digest='sha256:'+'1'*64))
    if expected is None:
        with pytest.raises(ValueError): namespace['execution_plan_digest'](handle)
    else:
        assert namespace['execution_plan_digest'](handle) == expected
