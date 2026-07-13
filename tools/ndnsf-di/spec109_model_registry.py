#!/usr/bin/env python3
"""Immutable Qwen model registry validation and sealing."""
from __future__ import annotations
import copy,hashlib,json,re
from pathlib import Path,PurePosixPath
from typing import Any,Mapping

SIZES={"0.5B","1.5B","3B","7B","14B","32B","72B"}; STATES={"PLANNED","STAGING","VERIFIED","SEALED","BLOCKED"}
DIGEST=re.compile(r"^sha256:[0-9a-f]{64}$"); REVISION=re.compile(r"^[0-9a-f]{40}$")
class ModelRegistryError(ValueError): pass
def _fail(code,detail=""): raise ModelRegistryError(code+(":"+detail if detail else ""))
def _digest(value): return "sha256:"+hashlib.sha256(json.dumps(value,sort_keys=True,separators=(",",":")).encode()).hexdigest()
def _is_digest(value): return isinstance(value,str) and DIGEST.fullmatch(value) is not None

def validate_registry_entry(value:Mapping[str,object])->dict[str,Any]:
    required={"modelId","family","sizeClass","repository","revision","tokenizerDigest","licenseClass","licenseDigest","files","sourceBytes","state","projectPath"}
    optional={"registryDigest"}
    if set(value)-required-optional or required-set(value): _fail("MODEL_REGISTRY_FIELDS_INVALID")
    size=value["sizeClass"]
    if size not in SIZES or value["repository"]!=f"Qwen/Qwen2.5-{size}-Instruct": _fail("MODEL_IDENTITY_INVALID")
    if value["family"]!="Qwen2.5-Instruct": _fail("MODEL_FAMILY_INVALID")
    if not isinstance(value["revision"],str) or REVISION.fullmatch(value["revision"]) is None: _fail("MODEL_REVISION_MUTABLE")
    for field in ("tokenizerDigest","licenseDigest"):
        if not _is_digest(value[field]): _fail("MODEL_DIGEST_INVALID",field)
    if value["licenseClass"] not in {"apache-2.0","qwen-research","qwen"}: _fail("MODEL_LICENSE_INVALID")
    if value["state"] not in STATES: _fail("MODEL_STATE_INVALID")
    project=str(value["projectPath"])
    if not project.startswith("/project/") or "/../" in project or project.startswith("/home/"): _fail("MODEL_PROJECT_PATH_INVALID")
    files=value["files"]
    if not isinstance(files,list) or not files: _fail("MODEL_FILES_INVALID")
    seen=set(); total=0; normalized=[]
    for row in files:
        if not isinstance(row,Mapping) or set(row)!={"path","bytes","digest","lfsPointer"}: _fail("MODEL_FILE_FIELDS_INVALID")
        path=row["path"]; pure=PurePosixPath(str(path))
        if pure.is_absolute() or any(x in ("",".","..") for x in pure.parts): _fail("MODEL_FILE_PATH_INVALID")
        if str(path) in seen:
            _fail("MODEL_FILE_DUPLICATE")
        seen.add(str(path))
        if row["lfsPointer"] is not False: _fail("MODEL_LFS_POINTER_UNRESOLVED")
        if isinstance(row["bytes"],bool) or not isinstance(row["bytes"],int) or row["bytes"]<0: _fail("MODEL_FILE_SIZE_INVALID")
        if not _is_digest(row["digest"]): _fail("MODEL_FILE_DIGEST_INVALID")
        total+=row["bytes"]; normalized.append(dict(row))
    if value["sourceBytes"]!=total: _fail("MODEL_SOURCE_SIZE_MISMATCH")
    result=copy.deepcopy(dict(value)); result["files"]=normalized
    if "registryDigest" in result:
        digest=result.pop("registryDigest")
        if digest!=_digest(result): _fail("MODEL_REGISTRY_DIGEST_MISMATCH")
        result["registryDigest"]=digest
    return result

def seal_registry_entry(value:Mapping[str,object])->dict[str,Any]:
    result=validate_registry_entry(value)
    if result["state"] not in {"VERIFIED","SEALED"}: _fail("MODEL_NOT_VERIFIED")
    result.pop("registryDigest",None); result["state"]="SEALED"; result["registryDigest"]=_digest(result)
    return result
__all__=["ModelRegistryError","seal_registry_entry","validate_registry_entry"]
