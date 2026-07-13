#!/usr/bin/env python3
"""Quota-authoritative storage admission and protected cleanup planning."""
from __future__ import annotations
from pathlib import PurePosixPath
from typing import Any,Iterable,Mapping
import re
class StorageError(ValueError): pass
def _fail(code,detail=""): raise StorageError(code+(":"+detail if detail else ""))
def _number(value,field):
    if isinstance(value,bool) or not isinstance(value,int) or value<0:_fail("STORAGE_NUMBER_INVALID",field)
    return value
def evaluate_storage(value:Mapping[str,object])->dict[str,Any]:
    for field in ("targetPath","quotaSource","quotaVerified","limitBytes","usedBytes","sharedFreeBytes","projected","reserveBytes","protectedPaths"):
        if field not in value:_fail("STORAGE_FIELD_MISSING",field)
    target=str(value["targetPath"])
    if not target.startswith("/project/"):_fail("STORAGE_TARGET_NOT_PROJECT",target)
    projected=value["projected"]
    if not isinstance(projected,Mapping) or set(projected)!={"source","export","cache","evidence"}:_fail("STORAGE_PROJECTION_INVALID")
    peak=sum(_number(projected[x],x) for x in projected)
    limit=_number(value["limitBytes"],"limitBytes"); used=_number(value["usedBytes"],"usedBytes"); reserve=_number(value["reserveBytes"],"reserveBytes")
    available=max(0,limit-used); required=peak+reserve
    if value["quotaVerified"] is not True:return {"status":"BLOCKED","reasonCode":"QUOTA_NOT_VERIFIED","projectedPeakBytes":peak,"quotaAvailableBytes":available,"requiredBytes":required}
    status="PASS" if available>=required else "BLOCKED"
    return {"status":status,"reasonCode":"" if status=="PASS" else "QUOTA_RESERVE_INSUFFICIENT","projectedPeakBytes":peak,"quotaAvailableBytes":available,"requiredBytes":required,"sharedFreeBytes":_number(value["sharedFreeBytes"],"sharedFreeBytes"),"quotaSource":str(value["quotaSource"])}
def _safe(path):
    p=PurePosixPath(str(path));
    if p.is_absolute() or any(x in ("",".","..") for x in p.parts):_fail("CLEANUP_PATH_INVALID",str(path))
    return p.as_posix()
def plan_cleanup(candidates:Iterable[str],*,protected:Iterable[str],referenced:Iterable[str]=())->dict[str,list[str]]:
    protect={_safe(x) for x in [*protected,*referenced]}; delete=[]; kept=[]
    for item in sorted({_safe(x) for x in candidates}):
        if item in protect or any(item.startswith(x.rstrip("/")+"/") for x in protect):kept.append(item)
        else:delete.append(item)
    return {"delete":delete,"protected":kept}
def parse_discovery_output(text:str)->dict[str,Any]:
    """Parse only tagged, repository-owned discovery records; retain raw lines."""
    records=[]
    for line in text.splitlines():
        if not line.startswith("NDNSF_DISCOVERY|"):
            continue
        parts=line.split("|")
        if len(parts)<3:_fail("DISCOVERY_RECORD_INVALID",line)
        records.append((parts[1],parts[2:]))
    result={"schemaVersion":"1.0","user":None,"host":None,"quota":None,"filesystems":[],"gres":[],"apptainer":None,"egress":"UNKNOWN","rawRecords":["|".join([key,*values]) for key,values in records]}
    for key,values in records:
        if key in {"USER","HOST","APPTAINER","EGRESS"}:
            if len(values)!=1:_fail("DISCOVERY_RECORD_INVALID",key)
            field={"USER":"user","HOST":"host","APPTAINER":"apptainer","EGRESS":"egress"}[key];result[field]=values[0]
        elif key=="QUOTA":
            if len(values)!=4:_fail("DISCOVERY_RECORD_INVALID",key)
            source,used,limit,verified=values
            result["quota"]={"source":source,"usedBytes":int(used),"limitBytes":int(limit),"verified":verified=="true"}
        elif key=="DF":
            if len(values)!=4:_fail("DISCOVERY_RECORD_INVALID",key)
            path,total,used,available=values;result["filesystems"].append({"path":path,"totalBytes":int(total),"usedBytes":int(used),"availableBytes":int(available)})
        elif key=="GRES":
            if len(values)!=4:_fail("DISCOVERY_RECORD_INVALID",key)
            partition,nodes,gres,state=values;result["gres"].append({"partition":partition,"nodes":nodes,"gres":gres,"state":state})
        else:_fail("DISCOVERY_KEY_UNKNOWN",key)
    if not result["user"] or not result["host"] or not result["filesystems"] or not result["gres"] or not result["apptainer"]:_fail("DISCOVERY_INCOMPLETE")
    if result["egress"] not in {"PASS","FAIL","UNKNOWN"}:_fail("DISCOVERY_EGRESS_INVALID")
    return result
def large_model_peak(file_manifest:Iterable[Mapping[str,object]],*,export_multiplier_milli:int,cache_multiplier_milli:int,evidence_bytes:int)->dict[str,int]:
    """Calculate peak from sealed source files, never parameter-count estimates."""
    files=list(file_manifest)
    if not files:_fail("LARGE_MODEL_MANIFEST_EMPTY")
    seen=set();source=0
    for row in files:
        path=str(row.get("path",""));size=_number(row.get("bytes"),"bytes")
        digest=str(row.get("sha256",""))
        if not path or path in seen or not re.fullmatch(r"sha256:[0-9a-f]{64}",digest):_fail("LARGE_MODEL_MANIFEST_INVALID",path)
        seen.add(path);source+=size
    export=source*_number(export_multiplier_milli,"exportMultiplierMilli")//1000
    cache=source*_number(cache_multiplier_milli,"cacheMultiplierMilli")//1000
    evidence=_number(evidence_bytes,"evidenceBytes")
    return {"source":source,"export":export,"cache":cache,"evidence":evidence,"peakBytes":source+export+cache+evidence}
__all__=["StorageError","evaluate_storage","large_model_peak","parse_discovery_output","plan_cleanup"]
