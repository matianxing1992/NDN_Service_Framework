#!/usr/bin/env python3
"""Add measured setup to qualified management totals without changing raw data."""
import argparse
import json
from pathlib import Path
from analyze_credentials import analyze, estimate, ARMS, PHASES


def summarize(path):
    qualification=analyze(path,5)
    rows=[json.loads(line) for line in path.read_text().splitlines()]
    costs={(r["arm"],r["permissions"],r["repetition"],r["phase"]):r for r in rows
           if r.get("schema")=="ndnsf-system-cost-v1"}
    output=[]
    for permissions in (1,4,16):
        for phase in PHASES:
            totals={arm:[costs[(arm,permissions,repetition,phase)] for repetition in range(1,6)] for arm in ARMS}
            for arm,group in totals.items():
                cpu=[r["management_cpu_ms"]+r["setup_cpu_ms"] for r in group]
                wire=[r["management_wire_bytes"]+r["public_parameter_wire_bytes_once"] for r in group]
                output.append({"arm":arm,"permissions":permissions,"phase":phase,
                    "setup_cpu_ms":estimate([r["setup_cpu_ms"] for r in group]),
                    "total_cpu_ms":estimate(cpu),"total_signed_wire_bytes":estimate(wire),
                    "cpu_difference_from_batched_rsa_ms":estimate([cpu[i]-totals["BATCHED_RSA"][i]["management_cpu_ms"]
                        -totals["BATCHED_RSA"][i]["setup_cpu_ms"] for i in range(5)]),
                    "material_objects":estimate([r["management_objects"] for r in group])})
    return {"qualified":qualification["qualified"],"input_sha256":qualification["input_sha256"],
        "scope":"local component; setup plus management CPU, signed comparison transport including PP; no network or break-even claim",
        "identity_bootstrap":qualification["identity_bootstrap"],"costs":output}


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("input",type=Path)
    args=parser.parse_args();print(json.dumps(summarize(args.input),indent=2))
