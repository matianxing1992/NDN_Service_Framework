#!/usr/bin/env python3
"""Canonical deterministic semantic validator for Spec 109."""
from __future__ import annotations
import copy,json,re,sys
from pathlib import Path
from typing import Any,Iterable,Mapping

HERE=Path(__file__).resolve().parent
if str(HERE) not in sys.path:sys.path.insert(0,str(HERE))
from spec109_candidate import comparison_fingerprint
from spec109_matrix import validate_matrix
from spec109_source import validate_source_snapshot

DIGEST=re.compile(r"^sha256:[0-9a-f]{64}$"); THRESHOLDS={"p50":20,"p95":100,"p99":1000}
class ValidationError(ValueError):pass
def _fail(code,detail=""):raise ValidationError(code+(":"+detail if detail else ""))
def _is_digest(value):return isinstance(value,str) and DIGEST.fullmatch(value) is not None

def validate_backend(value:Mapping[str,object],*,allocated_gpu_uuids:Iterable[str]|None=None)->dict[str,Any]:
    if value.get("requested")!="cuda":_fail("BACKEND_NOT_CUDA_REQUEST")
    if value.get("fallbackUsed") is True:_fail("BACKEND_FALLBACK_USED")
    if value.get("fullCuda") is not True:_fail("BACKEND_FULL_CUDA_FALSE")
    assignments=value.get("nodeAssignments")
    if not isinstance(assignments,list) or not assignments:_fail("BACKEND_NODE_PROFILE_INCOMPLETE")
    model_nodes=[x for x in assignments if isinstance(x,Mapping) and x.get("modelNode") is True]
    if not model_nodes or len(model_nodes)!=len([x for x in assignments if isinstance(x,Mapping)]):_fail("BACKEND_NODE_PROFILE_INCOMPLETE")
    for row in model_nodes:
        if row.get("provider")!="CUDAExecutionProvider":_fail("BACKEND_MODEL_NODE_NOT_CUDA",str(row.get("nodeName")))
    mappings=value.get("gpuMappings")
    if not isinstance(mappings,list) or not mappings:_fail("BACKEND_GPU_MAPPING_MISSING")
    if allocated_gpu_uuids is not None:
        allocated=set(allocated_gpu_uuids)
        for row in mappings:
            if not isinstance(row,Mapping) or row.get("uuid") not in allocated:_fail("BACKEND_GPU_UUID_NOT_ALLOCATED")
    return {"status":"PASS","profiledModelNodes":len(model_nodes),"gpuMappingCount":len(mappings)}

def validate_correctness(value:Mapping[str,object])->dict[str,Any]:
    output=value.get("outputTokenIds"); reference=value.get("referenceOutputTokenIds")
    if not isinstance(output,list) or not output or not isinstance(reference,list) or output!=reference or value.get("exactMatch") is not True:_fail("CORRECTNESS_TOKEN_MISMATCH")
    if not isinstance(value.get("inputTokenIds"),list) or not value["inputTokenIds"]:_fail("CORRECTNESS_INPUT_TOKENS_MISSING")
    checkpoints=value.get("checkpoints")
    if not isinstance(checkpoints,list) or not checkpoints:_fail("CORRECTNESS_CHECKPOINT_MISSING")
    for row in checkpoints:
        if not isinstance(row,Mapping) or row.get("pass") is not True:_fail("CORRECTNESS_CHECKPOINT_FAILED",str(row.get("name") if isinstance(row,Mapping) else "invalid"))
        for error,tolerance in (("maxAbsError","atol"),("maxRelError","rtol")):
            if not isinstance(row.get(error),(int,float)) or not isinstance(row.get(tolerance),(int,float)) or row[error]>row[tolerance]:_fail("CORRECTNESS_TOLERANCE_EXCEEDED",str(row.get("name")))
    return {"status":"PASS","outputTokenCount":len(output),"checkpointCount":len(checkpoints)}

def validate_percentile(count:int,value:Mapping[str,object],name:str):
    if name not in THRESHOLDS:_fail("PERCENTILE_NAME_INVALID")
    status=value.get("status"); number=value.get("value"); enough=count>=THRESHOLDS[name]
    if enough:
        if status!="AVAILABLE" or not isinstance(number,(int,float)):_fail("PERCENTILE_AVAILABILITY_INVALID",name)
        return number
    if status!="UNAVAILABLE_INSUFFICIENT_N" or number is not None:_fail("PERCENTILE_SAMPLE_COUNT",name)
    return None

def validate_comparison(baseline:Mapping[str,object],candidate:Mapping[str,object])->dict[str,str]:
    first=comparison_fingerprint(baseline); second=comparison_fingerprint(candidate)
    if first!=second:_fail("COMPARISON_UNMATCHED")
    return {"status":"PASS","comparisonFingerprint":first}
def validate_overhead_roles(baseline_role:str,candidate_role:str)->dict[str,str]:
    if baseline_role!="matched-staged-baseline" or candidate_role!="candidate":_fail("COMPARISON_ORACLE_TIMING_FORBIDDEN")
    return {"status":"PASS"}

def _scan_secret_fields(value:object,path:str="$"):
    if isinstance(value,Mapping):
        for key,item in value.items():
            low=str(key).lower()
            allowed={"tokenizerdigest","inputtokenids","outputtokenids","referenceoutputtokenids","tokenspersecond","promptsetdigest"}
            if low not in allowed and ("password" in low or "privatekey" in low or "providertoken" in low or "usertoken" in low or low in {"secret","mfa","credential"}):_fail("EVIDENCE_SECRET_FIELD",path+"."+str(key))
            _scan_secret_fields(item,path+"."+str(key))
    elif isinstance(value,list):
        for index,item in enumerate(value):_scan_secret_fields(item,f"{path}[{index}]")

def validate_evidence(value:Mapping[str,object])->dict[str,Any]:
    _scan_secret_fields(value)
    required={"runId","cellId","backend","correctness","terminal","promotion","authority","checksums"}
    if required-set(value):_fail("EVIDENCE_FIELD_MISSING",",".join(sorted(required-set(value))))
    terminal=value["terminal"]; promotion=value["promotion"]; authority=value["authority"]
    if not isinstance(terminal,Mapping) or not isinstance(promotion,Mapping) or not isinstance(authority,Mapping):_fail("EVIDENCE_STRUCTURE_INVALID")
    if terminal.get("status")=="PASS" and terminal.get("originalExitCode")!=0:_fail("EVIDENCE_EXIT_CONTRADICTION")
    if terminal.get("status")=="PASS" and promotion.get("complete") is not True:_fail("EVIDENCE_PROMOTION_INCOMPLETE")
    if authority.get("physicalProduction")!="DEFERRED" or authority.get("physicalProductionOwner")!="Spec 106":_fail("EVIDENCE_FALSE_AUTHORITY")
    checksums=value["checksums"]
    if not isinstance(checksums,Mapping) or not checksums or any(not _is_digest(x) for x in checksums.values()):_fail("EVIDENCE_CHECKSUM_INVALID")
    backend=validate_backend(value["backend"])
    correctness=validate_correctness(value["correctness"])
    metrics=value.get("metrics",{})
    if isinstance(metrics,Mapping):
        for field in ("ttftMs","interTokenMs","tokensPerSecond","requestThroughput"):
            dist=metrics.get(field)
            if isinstance(dist,Mapping) and isinstance(dist.get("count"),int):
                for name in THRESHOLDS:validate_percentile(dist["count"],dist.get(name,{}),name)
    return {"status":"PASS","runId":value["runId"],"cellId":value["cellId"],"backend":backend,"correctness":correctness}

def schema_validator(schema_name:str):
    from jsonschema import Draft202012Validator,RefResolver
    root=Path(__file__).resolve().parents[2]/"specs/109-ndnsf-di-itiger-qwen-scaling/contracts"
    path=root/schema_name; schema=json.loads(path.read_text())
    store={}
    for item in root.glob("*.json"):
        loaded=json.loads(item.read_text()); store[item.as_uri()]=loaded
        if "$id" in loaded:store[loaded["$id"]]=loaded
    return Draft202012Validator(schema,resolver=RefResolver(path.as_uri(),schema,store=store))
def validate_document(schema_name:str,value:Mapping[str,object],*,semantic:bool=False)->dict[str,Any]:
    errors=sorted(schema_validator(schema_name).iter_errors(value),key=lambda x:list(x.path))
    if errors:_fail("SCHEMA_INVALID",errors[0].message)
    report={"schema":"PASS"}
    if semantic:
        if schema_name=="source-snapshot.schema.json":validate_source_snapshot(value)
        elif schema_name=="scale-matrix.schema.json":validate_matrix(value)
        elif schema_name=="qwen-experiment-evidence.schema.json":validate_evidence(value)
    report["semantic"]="PASS" if semantic else "NOT_REQUESTED"; report["status"]="PASS"
    return report
__all__=["ValidationError","schema_validator","validate_backend","validate_comparison","validate_correctness","validate_document","validate_evidence","validate_overhead_roles","validate_percentile"]
