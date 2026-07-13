#!/usr/bin/env python3
"""Safe Slurm rendering and local exactly-once reservation ledger."""
from __future__ import annotations
import json,os,re
from pathlib import Path
from typing import Mapping
SAFE=re.compile(r"^[A-Za-z0-9._/:@+,-]+$"); DIGEST=re.compile(r"^sha256:[0-9a-f]{64}$")
class JobError(ValueError):pass
def _fail(code,detail=""):raise JobError(code+(":"+detail if detail else ""))
def _safe(value,field):
    text=str(value)
    if SAFE.fullmatch(text) is None:_fail("SLURM_UNSAFE_"+field.upper())
    return text
def render_sbatch(value:Mapping[str,object])->str:
    required={"jobName","partition","account","qos","wallTime","cpus","memory","gpuType","gpuCount","runId","command"}
    if required-set(value):_fail("SLURM_FIELD_MISSING")
    cpus=value["cpus"]; gpus=value["gpuCount"]
    if isinstance(cpus,bool) or not isinstance(cpus,int) or cpus<1:_fail("SLURM_RESOURCE_INVALID")
    if isinstance(gpus,bool) or not isinstance(gpus,int) or gpus<0 or gpus>8:_fail("SLURM_RESOURCE_INVALID")
    v={k:_safe(value[k],k) for k in required}
    return "\n".join(["#!/bin/bash",f"#SBATCH --job-name={v['jobName']}",f"#SBATCH --partition={v['partition']}",f"#SBATCH --account={v['account']}",f"#SBATCH --qos={v['qos']}",f"#SBATCH --time={v['wallTime']}",f"#SBATCH --cpus-per-task={cpus}",f"#SBATCH --mem={v['memory']}",f"#SBATCH --gres=gpu:{v['gpuType']}:{gpus}","set -euo pipefail",f"export NDNSF_SPEC109_RUN_ID={v['runId']}",v["command"],""])
def reserve_run(path:Path|str,run_id:str,submission_digest:str)->dict[str,str]:
    _safe(run_id,"runId")
    if not isinstance(submission_digest,str) or DIGEST.fullmatch(submission_digest) is None:_fail("SUBMISSION_DIGEST_INVALID")
    target=Path(path); target.parent.mkdir(parents=True,exist_ok=True)
    payload={"runId":run_id,"submissionDigest":submission_digest,"state":"RESERVED"}
    try:
        fd=os.open(str(target),os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    except FileExistsError:_fail("RUN_ALREADY_RESERVED",run_id)
    with os.fdopen(fd,"w",encoding="utf-8") as stream:json.dump(payload,stream,sort_keys=True);stream.write("\n")
    return payload
__all__=["JobError","render_sbatch","reserve_run"]
