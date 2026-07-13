#!/usr/bin/env python3
"""Spec 109 candidate and matched-comparison fingerprints."""
from __future__ import annotations
import hashlib,json,re
from typing import Any,Mapping
DIGEST=re.compile(r"^sha256:[0-9a-f]{64}$"); SIZES={"0.5B","1.5B","3B","7B","14B","32B","72B"}
class CandidateError(ValueError):pass
def _fail(code,detail=""):raise CandidateError(code+(":"+detail if detail else ""))
def digest_object(value):return "sha256:"+hashlib.sha256(json.dumps(value,sort_keys=True,separators=(",",":")).encode()).hexdigest()
def build_candidate(value:Mapping[str,object],size:str)->dict[str,Any]:
    if size not in SIZES or size not in value.get("sizes",[]):_fail("CANDIDATE_SIZE_INVALID")
    keys=("sourceSnapshotDigest","predecessorGateDigest","deploymentProfileDigest","workloadDigest")
    for key in keys:
        if not isinstance(value.get(key),str) or DIGEST.fullmatch(str(value[key])) is None:_fail("CANDIDATE_DIGEST_INVALID",key)
    binding={key:value[key] for key in keys}; binding["sizeClass"]=size; binding["candidateBindings"]=value.get("candidateBindings")
    digest=digest_object(binding)
    return {"schemaVersion":"1.0","candidateId":"spec109-"+size.lower().replace(".","_")+"-"+digest[7:23],"candidateDigest":digest,"binding":binding}
COMPARISON_FIELDS=("artifactDigest","runtimeDigest","sessionDigest","workloadDigest","cacheState","loggingProfile","stageTopology","gpuMapping","warmup","timeoutMs","windowSeconds")
def comparison_fingerprint(value:Mapping[str,object])->str:
    missing=[x for x in COMPARISON_FIELDS if x not in value]
    if missing:_fail("COMPARISON_FIELD_MISSING",",".join(missing))
    return digest_object({x:value[x] for x in COMPARISON_FIELDS})
__all__=["CandidateError","build_candidate","comparison_fingerprint","digest_object"]
