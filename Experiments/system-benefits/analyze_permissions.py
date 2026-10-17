#!/usr/bin/env python3
"""Independent production grant/revoke qualification; no network baseline claim."""
import argparse
import json
from pathlib import Path
import re

from analyze_dynamic import ip_counts, require, rows


def analyze(directory):
    manifest=json.loads((directory/"manifest.json").read_text())
    grant_timeout=manifest["status"]=="MEASUREMENT_COMPLETE" and manifest.get("outcome")=="GRANT_TIMEOUT"
    negative=manifest["status"]=="MEASUREMENT_COMPLETE" and manifest.get("outcome") in ("RECOVERY_TIMEOUT","GRANT_TIMEOUT")
    require((manifest["status"]=="RUN_COMPLETE" or negative) and not manifest.get("cleanup_errors"),"incomplete run")
    require(manifest["users"]==["/example/hello/user/u"+str(i) for i in range(8)],"eight unique User identities")
    labels={"controller","provider",*("user"+str(i) for i in range(8)),
            *("capture-"+p for p in ("gs","drone1","drone2")),
            *(p+"-nfd" for p in ("gs","relay","drone1","drone2"))}
    require({r["label"] for r in manifest["children"]}==labels,"child inventory")
    require(all(r["exit"]==0 for r in manifest["children"]),"native or capture exit")
    controller=rows(directory/"controller.jsonl")
    provider=rows(directory/"provider.jsonl")
    require(controller[-1]["event"]=="shutdown" and controller[-1]["granted"]
            and controller[-1]["revoked"]== (not grant_timeout),"controller lifecycle")
    inventories=[r for r in controller if r["event"]=="bootstrap_key_inventory"]
    require(len(inventories)==1 and inventories[0]["oracle"] and inventories[0]["private_keys"]==1,
            "controller private-key isolation")
    require(provider[-1]["event"]=="shutdown" and provider[-1]["ready"],"provider lifecycle")
    executions=[r for r in provider if r["event"]=="execution"]
    execution_by_id={r["request_id"]:r for r in executions}
    require(len(execution_by_id)==len(executions),"duplicate native execution")
    commits={r["phase"]:r for r in controller if r["event"]=="commit"}
    checkpoints={r["phase"]:r for r in controller if r["event"]=="recovery_checkpoint"}
    require(set(commits)==set(checkpoints)==({"grant"} if grant_timeout else {"grant","revoke"}),"mutation/checkpoint inventory")
    grant,revoke=commits["grant"],commits.get("revoke")
    require(grant["oracle"] and (grant_timeout or revoke["oracle"] and grant["mono_ns"]<revoke["begin_ns"]),"native mutation order")
    successful=failed=old_inflight=unresolved=revoked_old_success=0
    recovery=[];denied=[]
    for i,identity in enumerate(manifest["users"]):
        data=rows(directory/("user"+str(i)+".jsonl"))
        final=data[-1]
        require(final["event"]=="shutdown" and final["ready"] and final["invalid"]==0
                and final["identity"]==identity,"User lifecycle/provenance")
        require(not any(r["event"].startswith("unexpected_revoked_") for r in data),"revoked callback")
        submitted={r["request_id"]:r for r in data if r["event"]=="submitted" and r["request_id"]!="/"}
        submitted_rows=[r for r in data if r["event"]=="submitted"]
        terminals=[r for r in data if r["event"]=="terminal"]
        require(len({r["command"] for r in submitted_rows})==len(submitted_rows),"duplicate submission command")
        require(len({r["command"] for r in terminals})==len(terminals),"duplicate terminal command")
        terminal_commands={r["command"] for r in terminals}
        unresolved+=sum(r["command"] not in terminal_commands for r in submitted_rows)
        require(terminals and any(r["verified_success"] and r["phase"]=="initial" for r in terminals),"initial real access")
        for terminal in terminals:
            require(terminal["begin_ns"]<=terminal["terminal_ns"],"negative access duration")
            if not terminal["verified_success"]:
                failed+=1;continue
            successful+=1
            require(grant_timeout or i!=0 or terminal.get("response_version")!=revoke["global"],"revoked identity succeeded in new epoch")
            require(not negative or grant_timeout or terminal.get("response_version")!=revoke["global"],"negative boundary has new-epoch success")
            request=terminal["request_id"]
            require(request in submitted and request in execution_by_id,"successful request missing native execution")
            execution=execution_by_id[request]
            require(execution["user"]==identity and execution["command"]==terminal["command"]
                    and execution["service"]==terminal["service"]
                    and terminal["begin_ns"]<=execution["begin_ns"]<=execution["end_ns"]<=terminal["terminal_ns"],
                    "execution binding/timing")
            if revoke and terminal["begin_ns"]<revoke["mono_ns"]<=terminal["terminal_ns"]:old_inflight+=1
            if revoke and i==0 and terminal["terminal_ns"]>=revoke["mono_ns"]:revoked_old_success+=1
        if i==0:
            extras=[r for r in data if r["event"]=="grant_extra_verified"]
            require(not extras if grant_timeout else extras and all(r["oracle"] and r["response_version"]==grant["global"] for r in extras),"grant EXTRA oracle")
            extra_success=any(r["verified_success"] and r["service"]=="/EXTRA" and r["phase"]=="grant"
                              and r.get("response_version")==grant["global"] for r in terminals)
            require(extra_success== (not grant_timeout),"grant real EXTRA outcome")
            denied=[r for r in data if r["event"]=="revoked_gate_denied"]
            require(not denied if negative else len(denied)==2 and {r["service"] for r in denied}=={"/HELLO","/EXTRA"},"revoked gate outcome")
            require(all(r["oracle"] and r["empty_authorization"] and r["global"]==revoke["global"]
                        and r["mono_ns"]>=revoke["mono_ns"] and r["request_id"]=="/" for r in denied),"new-version denial")
            require(final["extra_verified"]== (not grant_timeout) and final["denial_verified"]== (not negative),"target final oracle")
        else:
            if grant_timeout:
                require(not any(r["event"]=="survivor_recovery_verified" for r in data),"unexpected revoke recovery")
                continue
            observed=[r for r in data if r["event"]=="survivor_recovery_verified"
                      and r["begin_ns"]>=revoke["mono_ns"] and r["response_version"]==revoke["hello"]]
            if negative:
                require(not observed,"timeout boundary unexpectedly has survivor recovery")
                continue
            require(observed and all(r["oracle"] and r["permission_matrix"] for r in observed),"survivor matrix/epoch")
            first=min(observed,key=lambda r:r["mono_ns"])
            require(any(r["verified_success"] and r["request_id"]==first["request_id"]
                        and r["command"]==first["command"] for r in terminals),"recovery missing real successful access")
            recovery.append((first["mono_ns"]-revoke["mono_ns"])/1e6)
    forbidden={r["command"] for r in denied}
    require(not any(r["command"] in forbidden for r in executions),"revoked command executed")
    grant_end=manifest["recovery_timeout"]["observed_ns"] if grant_timeout else manifest["barriers"]["grant-extra"]["observed_ns"]
    revoke_end=None if grant_timeout else manifest["recovery_timeout"]["observed_ns"] if negative else manifest["barriers"]["revoke-seven-and-denied"]["observed_ns"]
    boundary=None
    if negative:
        timeout=manifest["recovery_timeout"]
        mutation=grant if grant_timeout else revoke
        budget=10 if grant_timeout else 20
        require(timeout["budget_seconds"]==budget and timeout["deadline_ns"]-timeout["begin_ns"]==budget*1000000000
                and mutation["mono_ns"]<=timeout["begin_ns"]<=timeout["deadline_ns"]<=timeout["observed_ns"],"negative observation deadline")
        require(manifest["drain_end_ns"]-manifest["drain_begin_ns"]>=2000000000
                and manifest["drain_begin_ns"]>=timeout["observed_ns"],"negative capture drain")
        required={"initial-eight","grant-commit","grant-checkpoint"}
        if not grant_timeout:required.update({"grant-extra","revoke-commit","revoke-checkpoint"})
        require(set(manifest["barriers"])==required,"negative prerequisite barriers")
        logs=(directory/"controller.log").read_text().splitlines()
        publications=[line for line in logs if any(marker in line for marker in
            ("NDNSF_POLICY_STATUS_PUBLISHED", "[POLICY-MANIFEST] Reply", "[PERMISSIONS/USER] Encrypted reply", "[PERMISSIONS/PROVIDER] Encrypted reply"))]
        require(len(publications)==4096,"negative cause not proven: original reply capacity")
        wall_end=(timeout["observed_ns"]-manifest["clock_anchor"]["mono_ns"]+manifest["clock_anchor"]["wall_ns"])/1e9
        wall_commit=(mutation["mono_ns"]-manifest["clock_anchor"]["mono_ns"]+manifest["clock_anchor"]["wall_ns"])/1e9
        require(max(float(line.split()[0]) for line in publications)<wall_end,"saturation not reached in observation")
        require(wall_end-float(publications[0].split()[0])<300,"reply retention expiry already reached")
        actors=["provider"] if grant_timeout else ["user"+str(i) for i in range(8)]
        require(all(any("PolicyStatus timeout:" in line and wall_commit<=float(line.split()[0])<wall_end
                        for line in (directory/(actor+".log")).read_text().splitlines()) for actor in actors),"missing real post-commit status timeout")
        post=[r for r in executions if mutation["mono_ns"]<=r["begin_ns"]<timeout["observed_ns"]]
        if grant_timeout:
            require(not any(r["service"]=="/EXTRA" for r in executions),"grant timeout has EXTRA execution")
            attempts=[r for r in rows(directory/"user0.jsonl") if r["event"]=="submitted" and r["phase"]=="grant"
                      and r["service"]=="/EXTRA" and r["installed_global"]==grant["global"]]
            require(attempts,"grant not actually attempted with new status")
            provider_log=(directory/"provider.log").read_text()
            require(any("Reject request with stale policy epoch requestId="+r["request_id"]+
                        " receivedEpoch="+str(grant["extra"]["epoch"])+" " in provider_log for r in attempts),"grant failure admission boundary missing")
            scoped=re.findall(r"Installed PolicyStatus service=/EXTRA generation=(\d+) epoch=(\d+)",provider_log)
            require(scoped and all(int(g)==grant["extra"]["generation"] and int(e)<grant["extra"]["epoch"]
                                      for g,e in scoped),"grant failure old scoped status not proven")
            user_log=(directory/"user0.log").read_text()
            marker="Installed PolicyStatus service=/EXTRA generation="+str(grant["extra"]["generation"])+" epoch="+str(grant["extra"]["epoch"])
            installs=[float(line.split()[0]) for line in user_log.splitlines() if marker in line]
            first_attempt=min(r["begin_ns"] for r in attempts)
            wall_attempt=(first_attempt-manifest["clock_anchor"]["mono_ns"]+manifest["clock_anchor"]["wall_ns"])/1e9
            require(installs and min(installs)<wall_attempt,"target new scoped status not installed before request")
        else:
            require(all(float(line.split()[0])<wall_commit for line in publications),"unexpected fresh reply after saturation")
            require(revoked_old_success>0 and not recovery,"negative stale-epoch outcome")
            require(post and all(r["request_epoch"]<revoke["global"]["epoch"] for r in post),"unexpected future epoch execution")
        boundary={"cause":"EXTRA scoped status did not advance; reply cache reached capacity during observation" if grant_timeout else "original control reply cache saturated","original_replies":4096,
            "retention_ms":300000}
        if grant_timeout:
            boundary.update({"post_commit_executions":len(post),"extra_executions":0,"revoke":"NOT_ATTEMPTED"})
        else:
            boundary.update({"post_commit_old_epoch_executions":len(post),
                "revoked_identity_old_epoch_executions":sum(r["user"]==manifest["users"][0] for r in post)})
    phases={"grant_recovery":(grant["begin_ns"],grant_end)}
    if revoke:phases["revoke_recovery"]=(revoke["begin_ns"],revoke_end)
    tail="after_grant_observation" if grant_timeout else "after_revoke_observation"
    wire={phase:{"ip_bytes":0,"packets":0} for phase in (*phases,tail,"whole_capture")}
    for peer in ("gs","drone1","drone2"):
        capture=manifest["capture"][peer]
        log=(directory/("capture-"+peer+".log")).read_text()
        drop=re.search(r"(\d+) packets dropped by kernel",log)
        require(drop and int(drop.group(1))==0 and capture["dropped_packets"]==0,"capture drops")
        for phase,(begin,end) in phases.items():
            counts=ip_counts(directory/capture["pcap"],manifest["clock_anchor"],begin,end)
            require(counts["window"]["packets"]>0,"empty recovery phase capture")
            for field in ("ip_bytes","packets"):wire[phase][field]+=counts["window"][field]
            if phase==("grant_recovery" if grant_timeout else "revoke_recovery"):
                for field in ("ip_bytes","packets"):wire[tail][field]+=counts["drain"][field]
            if phase=="grant_recovery":
                for field in ("ip_bytes","packets"):wire["whole_capture"][field]+=sum(r[field] for r in counts.values())
    result={"qualified":not negative,"production_qualified":not negative,"measurement_complete":True,
            "outcome":manifest["outcome"],"boundary":boundary,
            "scope":manifest["scope"],"permissions":manifest["permissions"],"seed":manifest["seed"],
            "successful_accesses":successful,"failed_accesses":failed,"inflight_crossing_revoke":None if grant_timeout else old_inflight,
            "unresolved_at_shutdown":unresolved,"revoked_identity_old_epoch_successes_after_commit":None if grant_timeout else revoked_old_success,
            "revoked_gate_denials":None if grant_timeout else len(denied),"survivor_recovery_ms":recovery,"wire":wire,
            "management_object_dedup":"unreported; physical-link IP bytes include forwarding copies",
            "mutations":{}}
    for phase,(begin,end) in phases.items():
        c=commits["grant" if phase.startswith("grant") else "revoke"]
        point=checkpoints[c["phase"]]
        require(c["begin_ns"]<=c["mono_ns"]<=end<=point["mono_ns"],"recovery/checkpoint order")
        result["mutations"][c["phase"]]={"commit_cpu_ms":c["commit_cpu_ms"],
            "commit_elapsed_ms":(c["mono_ns"]-c["begin_ns"])/1e6,
            "observed_recovery_ms":None if negative and c["phase"]==("grant" if grant_timeout else "revoke") else (end-c["mono_ns"])/1e6,
            "observation_elapsed_ms":(end-c["mono_ns"])/1e6,
            "process_cpu_through_checkpoint_ms":point["process_cpu_since_mutation_ms"],
            "checkpoint_monitor_lag_ms":(point["mono_ns"]-end)/1e6}
    return result


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("directory",type=Path)
    args=parser.parse_args();print(json.dumps(analyze(args.directory),indent=2))
