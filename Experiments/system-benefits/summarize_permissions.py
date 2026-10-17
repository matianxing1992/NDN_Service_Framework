#!/usr/bin/env python3
"""Requalify the entire production matrix; use repetitions as statistical units."""
import argparse
import hashlib
import json
from pathlib import Path
from analyze_credentials import estimate
from analyze_permissions import analyze
from analyze_dynamic import require


def summarize(directory):
    manifest=json.loads((directory/"manifest.json").read_text())
    require(manifest["status"]=="CAMPAIGN_COMPLETE" and not manifest.get("first_failure"),"incomplete campaign")
    cells=manifest["cells"]
    require(len(cells)==10 and {(r["permissions"],r["seed"]) for r in cells}
            =={(p,s) for p in (1,16) for s in range(20401,20406)},"complete ten-cell matrix")
    blocks={};hashes={};certificates=set();policies={};trust_profiles=set()
    for cell in cells:
        path=directory/cell["id"];own=json.loads((path/"manifest.json").read_text());result=analyze(path)
        require(result["measurement_complete"] and (result["permissions"],result["seed"])==(cell["permissions"],cell["seed"]),"cell identity")
        for kind in ("sources","binaries","libraries"):
            require(own[kind],"empty candidate inventory")
            for source,value in own[kind].items():
                require(source in manifest["candidate"] and manifest["candidate"][source]==value,"candidate drift: "+source)
        require(hashlib.sha256((path/"trust-schema.conf").read_bytes()).hexdigest()==own["trust"],
                "trust file changed")
        if "ndn_trust_root" in own:
            require(hashlib.sha256((path/"root.cert").read_bytes()).hexdigest()
                    ==own["ndn_trust_root"]["sha256"],"root certificate changed")
            for label,certificate in own["ndn_certificates"].items():
                require(hashlib.sha256((path/(label+".cert")).read_bytes()).hexdigest()
                        ==certificate["sha256"],"actor certificate changed")
            trust_profiles.add(json.dumps({"trust":own["trust"],"root":own["ndn_trust_root"],
                                           "leaves":own["ndn_certificates"]},sort_keys=True))
        else:
            require(own["trust"]==manifest["candidate"][str(Path(__file__).resolve().parents[2]/"examples/trust-schema.conf")],"trust identity")
            trust_profiles.add(own["trust"])
        certificates.add(own["controller_certificate"])
        policies.setdefault(cell["permissions"],set()).add(own["policy"])
        blocks[(cell["permissions"],cell["seed"])]=result
        hashes[cell["id"]]={str(p.relative_to(path)):hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in path.iterdir() if p.suffix in (".jsonl",".pcap") or p.name=="manifest.json"}
    require(len(certificates)==1 and len(trust_profiles)==1 and
            all(len(values)==1 for values in policies.values()),"bootstrap/policy mismatch")
    groups=[]
    for permissions in (1,16):
        rows=[blocks[(permissions,seed)] for seed in range(20401,20406)]
        successful=[r for r in rows if r["production_qualified"]]
        def conditional(values):
            values=[value for value in values if value is not None]
            return estimate(values) if values else {"n":0,"mean":None,"ci95_half_width":None}
        groups.append({"permissions":permissions,"blocks":5,
            "production_qualified_blocks":len(successful),
            "grant_timeout_blocks":sum(r["outcome"]=="GRANT_TIMEOUT" for r in rows),
            "recovery_timeout_blocks":sum(r["outcome"]=="RECOVERY_TIMEOUT" for r in rows),
            "revoke_not_attempted_blocks":sum("revoke" not in r["mutations"] for r in rows),
            "boundaries":[r["boundary"] for r in rows if r["boundary"]],
            "access_counts":{field:sum(r[field] for r in rows if r[field] is not None) for field in
                ("successful_accesses","failed_accesses","unresolved_at_shutdown","inflight_crossing_revoke",
                 "revoked_identity_old_epoch_successes_after_commit","revoked_gate_denials")},
            "access_count_contributing_blocks":{field:sum(r[field] is not None for r in rows) for field in
                ("successful_accesses","failed_accesses","unresolved_at_shutdown","inflight_crossing_revoke",
                 "revoked_identity_old_epoch_successes_after_commit","revoked_gate_denials")},
            "slowest_survivor_recovery_ms":conditional([max(r["survivor_recovery_ms"]) for r in successful]),
            "grant_by_outcome":{},"revoke_by_outcome":{}})
        mutation_fields=("commit_cpu_ms","commit_elapsed_ms","observed_recovery_ms",
                         "observation_elapsed_ms","process_cpu_through_checkpoint_ms","checkpoint_monitor_lag_ms")
        for phase,outcomes in (("grant",("GRANT_RECOVERED","GRANT_TIMEOUT")),
                               ("revoke",("RECOVERED","RECOVERY_TIMEOUT"))):
            for outcome in outcomes:
                group=[r for r in rows if (r["outcome"]!="GRANT_TIMEOUT" if outcome=="GRANT_RECOVERED" else r["outcome"]==outcome)]
                groups[-1][phase+"_by_outcome"][outcome]={"blocks":len(group),
                    "mutations":{field:conditional([r["mutations"][phase][field] for r in group]) for field in mutation_fields},
                    "wire":{period:{field:conditional([r["wire"][period][field] for r in group]) for field in ("ip_bytes","packets")}
                        for period in (phase+"_recovery", "whole_capture", * (("after_revoke_observation",) if phase=="revoke" else ("after_grant_observation",) if outcome=="GRANT_TIMEOUT" else ()))}}
    scripts=[Path(__file__).resolve(),Path(__file__).with_name("analyze_permissions.py"),
             Path(__file__).with_name("analyze_dynamic.py"),Path(__file__).with_name("analyze_credentials.py")]
    return {"measurement_complete":True,"all_production_qualified":all(r["production_qualified"] for r in blocks.values()),
        "scope":manifest["scope"],"statistical_unit":"five repetitions; t4 95% intervals when all five contribute",
        "raw_sha256":hashes,"candidate":manifest["candidate"],
        "postprocessing_sha256":{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in scripts},"results":groups}


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("directory",type=Path)
    args=parser.parse_args();print(json.dumps(summarize(args.directory),indent=2))
