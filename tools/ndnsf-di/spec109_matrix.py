#!/usr/bin/env python3
"""Immutable keyed-cell state machine and scoped gate propagation."""
from __future__ import annotations
import copy,re
from typing import Any,Iterable,Mapping
DIGEST=re.compile(r"^sha256:[0-9a-f]{64}$"); TERMINAL={"PASS","FAIL","BLOCKED","DEFERRED","CANCELLED"}; EXECUTED={"PASS","FAIL","CANCELLED"}; ACTIVE={"SUBMITTED","RUNNING"}
class MatrixError(ValueError):pass
def _fail(code,detail=""):raise MatrixError(code+(":"+detail if detail else ""))
def _digest(value):return isinstance(value,str) and DIGEST.fullmatch(value) is not None
def validate_matrix(value:Mapping[str,object])->dict[str,Any]:
    if value.get("schemaVersion")!="2.0" or value.get("locked") is not True:_fail("MATRIX_HEADER_INVALID")
    if value.get("physicalProduction")!="DEFERRED":_fail("MATRIX_PHYSICAL_AUTHORITY_INVALID")
    cells=value.get("cells"); runs=value.get("runs")
    if not isinstance(cells,Mapping) or not cells:_fail("MATRIX_CELLS_INVALID")
    if not isinstance(runs,Mapping):_fail("MATRIX_RUNS_INVALID")
    referenced=set()
    for cid,row in cells.items():
        if not isinstance(row,Mapping):_fail("MATRIX_CELL_INVALID",str(cid))
        state=row.get("state"); run=row.get("runId"); evidence=row.get("evidenceDigest")
        if state in EXECUTED and (not isinstance(run,str) or not run or not _digest(evidence)):_fail("MATRIX_EXECUTED_EVIDENCE_MISSING",str(cid))
        if state in {"BLOCKED","DEFERRED"}:
            if row.get("gateScope") not in {"systemic","model-local","placement-local"} or not row.get("gateId") or not _digest(row.get("gateDigest")):_fail("MATRIX_GATE_INVALID",str(cid))
        if state=="NOT_STARTED" and (run is not None or evidence is not None):_fail("MATRIX_NOT_STARTED_BOUND")
        if value.get("finalized") is True and state not in TERMINAL:_fail("MATRIX_FINALIZED_NONTERMINAL",str(cid))
        if run is not None:
            if run in referenced:_fail("MATRIX_RUN_DUPLICATE",run)
            referenced.add(run)
    for rid,row in runs.items():
        if not isinstance(row,Mapping) or row.get("cellId") not in cells:_fail("MATRIX_RUN_CELL_INVALID",str(rid))
        if cells[row["cellId"]].get("runId")!=rid:_fail("MATRIX_RUN_REFERENCE_MISMATCH",str(rid))
    return {"status":"PASS","cellCount":len(cells),"runCount":len(runs)}
def transition_cell(value:Mapping[str,object],cell_id:str,state:str,*,run_id:str|None=None,evidence_digest:str|None=None,reason_code:str="",gate_scope:str="none",gate_id:str|None=None,gate_digest:str|None=None)->dict[str,Any]:
    result=copy.deepcopy(dict(value)); cells=result.get("cells",{})
    if cell_id not in cells:_fail("CELL_NOT_FOUND",cell_id)
    row=cells[cell_id]; old=row["state"]
    if old in TERMINAL:_fail("TERMINAL_IMMUTABLE",cell_id)
    allowed={"NOT_STARTED":{"SUBMITTED","BLOCKED","DEFERRED"},"SUBMITTED":{"RUNNING","PASS","FAIL","CANCELLED"},"RUNNING":{"PASS","FAIL","CANCELLED"}}
    if state not in allowed.get(old,set()):_fail("CELL_TRANSITION_INVALID",old+"->"+state)
    if state in ACTIVE|EXECUTED:
        rid=run_id or row.get("runId")
        if not isinstance(rid,str) or not rid:_fail("CELL_RUN_ID_REQUIRED")
        for cid,other in cells.items():
            if cid!=cell_id and other.get("runId")==rid:_fail("MATRIX_RUN_DUPLICATE",rid)
        row["runId"]=rid
    if state in EXECUTED:
        if not _digest(evidence_digest):_fail("CELL_EVIDENCE_REQUIRED")
        row["evidenceDigest"]=evidence_digest
    if state in {"BLOCKED","DEFERRED"}:
        if gate_scope not in {"systemic","model-local","placement-local"} or not gate_id or not _digest(gate_digest):_fail("CELL_GATE_REQUIRED")
        row.update(gateScope=gate_scope,gateId=gate_id,gateDigest=gate_digest)
    row["state"]=state; row["reasonCode"]=reason_code
    return result
def bundle_terminal(value:Mapping[str,object],cell_ids:Iterable[str])->bool:
    cells=value.get("cells",{}); ids=list(cell_ids)
    return bool(ids) and all(cid in cells and cells[cid].get("state") in TERMINAL for cid in ids)
def apply_gate(value:Mapping[str,object],*,source_cell:str,scope:str,gate_id:str,gate_digest:str)->dict[str,Any]:
    if scope not in {"systemic","model-local","placement-local"}:_fail("GATE_SCOPE_INVALID")
    result=copy.deepcopy(dict(value)); cells=result["cells"]
    if source_cell not in cells:_fail("CELL_NOT_FOUND",source_cell)
    source=cells[source_cell]
    for cid,row in cells.items():
        applies=scope=="systemic" or (scope=="model-local" and row.get("modelSize")==source.get("modelSize")) or (scope=="placement-local" and cid==source_cell)
        if applies and row.get("state")=="NOT_STARTED": result=transition_cell(result,cid,"BLOCKED",reason_code="SCOPED_GATE",gate_scope=scope,gate_id=gate_id,gate_digest=gate_digest); cells=result["cells"]
    return result
def placement_admission(*,model_bytes:int,gpu_memory_bytes:Iterable[int],stage_count:int,node_count:int,network_status:str)->dict[str,Any]:
    memories=list(gpu_memory_bytes)
    if model_bytes<=0 or stage_count<=0 or node_count<=0 or not memories or any(x<=0 for x in memories):_fail("PLACEMENT_INPUT_INVALID")
    usable=sum(memories)*85//100
    if usable<model_bytes:return {"status":"BLOCKED","reasonCode":"GPU_MEMORY_INSUFFICIENT","placement":None,"usableGpuBytes":usable}
    if node_count==1:return {"status":"PASS","reasonCode":"","placement":"one-node-multi-gpu","usableGpuBytes":usable}
    if network_status!="PASS":return {"status":"DEFERRED","reasonCode":"MULTINODE_NETWORK_EVIDENCE_MISSING","placement":None,"usableGpuBytes":usable}
    return {"status":"PASS","reasonCode":"","placement":"multi-node","usableGpuBytes":usable}
__all__=["MatrixError","TERMINAL","apply_gate","bundle_terminal","placement_admission","transition_cell","validate_matrix"]
