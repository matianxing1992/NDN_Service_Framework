#!/usr/bin/env python3
"""Frozen, supervised matched dynamic matrix; fail immediately on missing qualification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

HERE=Path(__file__).resolve().parent
REPO=HERE.parents[1]
E3_CONDITIONS = (("stable", 4700), ("slow", 4700), ("fast", 4700),
                 ("stable", 100), ("fast", 100))


def e3_formal_cells():
    """Fixed CD17 design, independent of observed outcomes."""
    cells = []
    for index, seed in enumerate(range(20590, 20606)):
        offset = index % 5
        order = E3_CONDITIONS[offset:] + E3_CONDITIONS[:offset]
        if (index // 5) % 2:
            order = order[::-1]
        cells.extend({"seed": seed, "period_ms": period, "dwell_ms": 0,
                      "window_ms": 60000, "recovery_condition": "prepared",
                      "availability_profile": profile} for profile, period in order)
    return cells


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def candidate():
    sources=[HERE/p for p in ("dynamic-common.hpp","dynamic-provider.cpp","dynamic-user.cpp",
                             "dynamic-oracle.cpp","run_dynamic.py","analyze_dynamic.py","dynamic_campaign.py",
                             "analyze_credentials.py","summarize_dynamic.py","wscript")]
    sources.extend([HERE.parent/"targeted-invocation/common.hpp",HERE.parent/"targeted-invocation/run_minindn.py"])
    sources.extend([REPO/"examples/hello.policies",REPO/"examples/trust-schema.conf",
                    REPO/"specs/205-compact-protected-requests/contracts.md"])
    binaries=[Path("/usr/local/bin/"+p) for p in ("ndnsf-dynamic-provider","ndnsf-dynamic-user",
                                               "ndnsf-dynamic-oracle","App_ServiceController")]
    paths=set(sources+binaries)
    for binary in binaries:
        closure=subprocess.check_output(["ldd",str(binary)],env=dict(os.environ,LD_LIBRARY_PATH="/usr/local/lib"),text=True)
        if "not found" in closure:raise RuntimeError("missing installed dependency")
        for word in closure.split():
            if word.startswith("/") and Path(word).is_file():paths.add(Path(word).resolve())
    return {str(p):sha(p) for p in sorted(paths)}


def save(path,value):
    path.write_text(json.dumps(value,indent=2,sort_keys=True)+"\n")


def stop_unit(unit, log):
    result = subprocess.run(["systemctl", "stop", unit], stdout=log,
                            stderr=subprocess.STDOUT, timeout=60)
    if result.returncode:
        state = subprocess.run(["systemctl", "show", unit, "--property=LoadState", "--value"],
                               capture_output=True, text=True, timeout=10)
        if state.stdout.strip() != "not-found":
            raise RuntimeError("failed to stop owned experiment unit: " + unit)


def load_matrix(path):
    path = path.resolve()
    raw = path.read_bytes()
    if len(raw) > 131072:
        raise ValueError("matrix exceeds size bound")
    matrix = json.loads(raw)
    if (set(matrix) != {"version", "purpose", "design_reference", "cells"}
            or type(matrix["version"]) is not int or matrix["version"] not in (1, 2, 3)
            or matrix["purpose"] not in ("pilot", "formal")
            or not isinstance(matrix["design_reference"], str)
            or not matrix["design_reference"].strip()
            or not isinstance(matrix["cells"], list) or not 1 <= len(matrix["cells"]) <= 128):
        raise ValueError("invalid frozen matrix header")
    design = (path.parent / matrix["design_reference"]).resolve()
    if not design.is_file():
        raise ValueError("missing experiment design/freeze record")
    seen = set()
    recovery = matrix["version"] == 2
    irregular = matrix["version"] == 3
    if irregular and matrix["purpose"] == "formal" and matrix["cells"] != e3_formal_cells():
        raise ValueError("frozen E3 formal matrix/order mismatch")
    numeric_fields = {"period_ms", "dwell_ms", "seed", "window_ms"}
    for cell in matrix["cells"]:
        if (not isinstance(cell, dict)
                or set(cell) != numeric_fields | ({"recovery_condition"} if recovery or irregular else set())
                                             | ({"availability_profile"} if irregular else set())
                or any(type(cell[key]) is not int for key in numeric_fields)
                or cell["period_ms"] not in (100, 4700)
                or cell["dwell_ms"] not in (0, 500, 5000)
                or not 0 <= cell["seed"] <= 2147483647
                or cell["window_ms"] not in (3000, 60000)
                or matrix["purpose"] == "formal" and cell["window_ms"] != 60000):
            raise ValueError("invalid frozen matrix cell")
        if recovery and (cell["recovery_condition"] not in ("prepared", "reregister")
                         or cell["period_ms"] != 4700 or cell["dwell_ms"] != 0
                         or cell["window_ms"] != 60000):
            raise ValueError("invalid recovery matrix condition")
        if irregular and (cell["recovery_condition"] != "prepared" or cell["dwell_ms"] != 0
                          or cell["availability_profile"] not in ("stable", "slow", "fast")
                          or cell["window_ms"] != 60000):
            raise ValueError("invalid E3 pilot condition")
        identity = tuple(cell[key] for key in ("period_ms", "dwell_ms", "seed", "window_ms"))
        if recovery or irregular:
            identity += (cell["recovery_condition"],)
        if irregular:
            identity += (cell["availability_profile"],)
        if identity in seen:
            raise ValueError("duplicate frozen matrix cell")
        seen.add(identity)
    if recovery:
        cells = matrix["cells"]
        if len(cells) % 2:
            raise ValueError("incomplete recovery pair")
        for index in range(0, len(cells), 2):
            left, right = cells[index:index + 2]
            if (left["seed"] != right["seed"]
                    or {left["recovery_condition"], right["recovery_condition"]}
                    != {"prepared", "reregister"}):
                raise ValueError("unmatched recovery pair")
        if matrix["purpose"] == "formal":
            if [c["seed"] for c in cells[::2]] != list(range(20560, 20576)):
                raise ValueError("frozen recovery formal seeds")
            for index in range(16):
                order = ["prepared", "reregister"]
                if (index + index//4) % 2:
                    order.reverse()
                if [c["recovery_condition"] for c in cells[2*index:2*index+2]] != order:
                    raise ValueError("frozen recovery condition order")
    return matrix, raw, design


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir",type=Path,required=True)
    parser.add_argument("--matrix",type=Path,required=True)
    parser.add_argument("--workspace",default="/var/tmp/ndnsf-spec204-dynamic")
    parser.add_argument("--budget-seconds",type=int,default=14400)
    args=parser.parse_args()
    matrix, matrix_raw, design = load_matrix(args.matrix)
    if args.budget_seconds <= 0:
        raise ValueError("campaign budget must be positive")
    if os.geteuid()!=0:raise SystemExit("MiniNDN campaign requires root")
    output=args.output_dir.resolve();output.mkdir(parents=True,exist_ok=False)
    cells = matrix["cells"]
    (output / "matrix.json").write_bytes(matrix_raw)
    (output / "design.md").write_bytes(design.read_bytes())
    frozen=candidate();start=time.monotonic()
    manifest={"schema":"ndnsf-dynamic-campaign205-v2","status":"PARTIAL","candidate":frozen,
              "purpose":matrix["purpose"], "matrix_sha256":sha(output/"matrix.json"),
              "design_reference":str(design), "design_snapshot":"design.md",
              "design_sha256":sha(output/"design.md"),
              "budget_seconds":args.budget_seconds,"matrix":cells,"cells":[]}
    if matrix["version"] == 2:
        manifest["schema"] = "ndnsf-dynamic-campaign205-e2-v1"
    if matrix["version"] == 3:
        manifest["schema"] = "ndnsf-dynamic-campaign205-e3-v1"
    save(output/"manifest.json",manifest)
    def interrupted(signum,frame):
        raise KeyboardInterrupt("supervisor signal "+str(signum))
    signal.signal(signal.SIGTERM,interrupted)
    try:
        for index,cell in enumerate(cells):
            if sha(design) != manifest["design_sha256"]:
                raise RuntimeError("frozen experiment design changed")
            if candidate()!=frozen:raise RuntimeError("candidate changed before cell")
            remaining=args.budget_seconds-(time.monotonic()-start)
            if remaining<500:raise RuntimeError("supervisory budget insufficient for next complete cell")
            label=f"c{index:02d}-p{cell['period_ms']}-d{cell['dwell_ms']}-b{cell['seed']}"
            if matrix["version"] in (2, 3):
                label += "-" + cell["recovery_condition"]
            if matrix["version"] == 3:
                label += "-" + cell["availability_profile"]
            directory=output/label
            manifest["active_cell"]=label;save(output/"manifest.json",manifest)
            command=[sys.executable,str(HERE/"run_dynamic.py"),"--output-dir",str(directory),
                     "--workspace",args.workspace,"--period-ms",str(cell["period_ms"]),
                     "--dwell-ms",str(cell["dwell_ms"]),"--seed",str(cell["seed"]),
                     "--window-ms",str(cell["window_ms"])]
            if matrix["version"] in (2, 3):
                command.extend(["--recovery-condition", cell["recovery_condition"]])
            if matrix["version"] == 3:
                command.extend(["--availability-profile", cell["availability_profile"]])
            unit = f"ndnsf-dynamic205-{os.getpid()}-{index}"
            service = ["systemd-run", "--quiet", "--wait", "--pipe", "--collect",
                       "--unit=" + unit, "--working-directory=" + str(REPO),
                       "--property=KillMode=control-group", "--property=TimeoutStopSec=45s",
                       "--property=RuntimeMaxSec=600s"]
            for key, value in os.environ.items():
                if key.startswith(("NDNSF_", "NDN_")) or key in ("PYTHONPATH", "LD_LIBRARY_PATH"):
                    service.append("--setenv=" + key + "=" + value)
            service.extend(command)
            manifest["active_unit"] = unit
            save(output/"manifest.json",manifest)
            print("CELL_START",label,flush=True)
            with (output/(label+".log")).open("w") as log:
                process=subprocess.Popen(service,stdout=log,stderr=subprocess.STDOUT)
                try:
                    code=process.wait(timeout=min(500,remaining))
                except BaseException:
                    subprocess.run(["systemctl", "kill", "--kill-who=main", "--signal=SIGINT", unit],
                                   stdout=log, stderr=subprocess.STDOUT, timeout=10)
                    try:process.wait(timeout=45)
                    except subprocess.TimeoutExpired:
                        stop_unit(unit, log)
                        process.wait(timeout=10)
                    raise
                finally:
                    # --collect may already have removed a fully stopped unit.
                    state = subprocess.run(["systemctl", "show", unit, "--property=LoadState",
                                            "--value"], capture_output=True, text=True, timeout=10)
                    if state.stdout.strip() != "not-found":
                        stop_unit(unit, log)
            if code:raise RuntimeError("native cell failed: "+label)
            analysis=subprocess.run([sys.executable,str(HERE/"analyze_dynamic.py"),str(directory)],
                                    capture_output=True,text=True,timeout=30)
            if analysis.returncode:raise RuntimeError("cell qualification failed: "+label+": "+analysis.stderr[-1000:])
            result=json.loads(analysis.stdout)
            if not result["qualified"] or len(result["results"])!=4:raise RuntimeError("incomplete four-arm cell")
            if candidate()!=frozen:raise RuntimeError("candidate changed during cell")
            if sha(design) != manifest["design_sha256"]:
                raise RuntimeError("frozen experiment design changed during cell")
            save(output/(label+"-summary.json"),result)
            manifest["cells"].append({**cell,"id":label,"qualified":True,"summary":label+"-summary.json"})
            manifest["elapsed_seconds"]=time.monotonic()-start;save(output/"manifest.json",manifest)
            print("CELL_COMPLETE",len(manifest["cells"]),"/",len(cells),label,flush=True)
        manifest["status"]="CAMPAIGN_COMPLETE"
    except BaseException as error:
        manifest["first_failure"]=type(error).__name__+": "+str(error);raise
    finally:
        manifest["elapsed_seconds"]=time.monotonic()-start;save(output/"manifest.json",manifest)
    print("DYNAMIC_CAMPAIGN_COMPLETE",output,flush=True)
